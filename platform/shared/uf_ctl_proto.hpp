// uf_ctl_proto.hpp — the daemon's control protocol: what clients send it over the Unix socket.
//
// Direction matters and decides the mechanism:
//
//   daemon -> clients   meters, state, notifications      SHARED MEMORY
//                       one writer, N readers; cost is O(1) in client count, and a stalled or
//                       crashed UI structurally cannot backpressure the realtime pump.
//   clients -> daemon   set a fader / cell / setting      THIS SOCKET
//                       needs rendezvous (how does a newly launched UI announce itself?), liveness
//                       (a socket reports a dead client instantly; shared memory cannot tell crashed
//                       from slow), and safe serialisation of multiple writers.
//
// Fixed-size binary, not JSON. A UI dragging faders sends hundreds of messages a second, and every
// client is ours and can share this header — so the compiler checks the protocol instead of a schema
// nobody validates. Debugging is served by the CLIs, which pretty-print; `nc` could not have anyway.
//
// Everything is host-endian: both ends are the same machine by construction (a Unix socket).
#pragma once
#include <cstdint>

#include "uf/protocol/mixer.hpp"
#include "uf/protocol/settings.hpp"

namespace uf::ctl {

// Bump when the wire layout changes in a way an older peer would misread. The daemon refuses a
// client whose version it does not know, EXPLICITLY — the alternative is two programs quietly
// disagreeing about a struct, which is how the shm ring bit us twice.
inline constexpr uint16_t kCtlVersion = 2;

// The socket lives beside the shm rings, under the user's own runtime dir. Must be reachable by a
// front-end process.
inline constexpr char kCtlSocketEnv[]  = "UF_CTL_SOCKET";     // override for tests
inline constexpr char kCtlSocketName[] = "userface800.ctl";         // under $TMPDIR

enum class MsgType : uint16_t {
    Hello       = 1,   // client -> daemon, first message: version handshake
    HelloAck    = 2,   // daemon -> client: accepted (or refused, with `ok` = 0)
    SetMixer    = 3,   // client -> daemon: one matrix cell
    SetFader    = 4,   // client -> daemon: one per-output fader
    SetSettings = 5,   // client -> daemon: the whole settings block + rate
    SetCellFlags = 6,  // client -> daemon: mute / phase-invert one crosspoint
    SetOutFlags  = 7,  // client -> daemon: mute / loopback one hardware output
    SetStereo    = 8,  // client -> daemon: pair (or unpair) a channel into stereo
    Submix       = 9,  // client -> daemon: copy or clear a whole output column
};

// Defined with the addressing it belongs to (uf/protocol/mixer.hpp), not duplicated here: the wire
// carries it as a uint16_t and the enum decides what a src index MEANS, so the two must not be able
// to drift apart.
using MixerSrcKind = ::uf::MixerSrcKind;

// Every message begins with this. `length` counts the WHOLE message including the header, so a
// reader can skip a type it does not know instead of losing sync — the same lesson as the AR-request
// walk, where an unrecognised packet froze the cursor and killed MIDI in permanently.
struct MsgHeader {
    uint16_t version;
    uint16_t type;
    uint32_t length;
};

struct HelloMsg {
    MsgHeader hdr;
    uint32_t  clientKind;   // informational: 0 unknown, 1 CLI, 2 TUI, 3 GUI
    uint32_t  reserved;
};

struct HelloAckMsg {
    MsgHeader hdr;
    uint32_t  ok;             // 0 = refused (version mismatch); 1 = accepted
    uint32_t  daemonVersion;  // what the daemon speaks, so a client can report the mismatch usefully
};

// coeff uses the device's own encoding (uf/protocol/mixer.hpp): 0 = mute, 0x8000 = unity,
// 0x10000 = +6 dB. dB conversion belongs in the client — the daemon should not re-derive it.
//
// This is the crosspoint's GAIN MAGNITUDE, never a negative coefficient. Phase inversion travels as
// a flag in SetCellFlagsMsg instead, because the daemon has to keep magnitude and sign apart anyway
// to make un-inverting restorable (see uf_mixer_model.hpp).
struct SetMixerMsg {
    MsgHeader hdr;
    uint16_t  srcKind;   // MixerSrcKind
    uint16_t  src;       // 0-based
    uint16_t  dest;      // 0-based
    uint16_t  pad;
    uint32_t  coeff;
};

struct SetFaderMsg {
    MsgHeader hdr;
    uint16_t  out;       // 0-based
    uint16_t  pad;
    uint32_t  coeff;
};

// The Fireface-Settings half: phantom, input/output levels, input sources, SPDIF, clock mode.
//
// Carries the WHOLE SettingsShadow rather than one field, because that matches both the hardware and
// the tool: the FF800's settings register is write-only and several settings share it, so a change
// means re-assembling and rewriting the lot. uf-set already worked that way. Sending the shadow also
// means the daemon adopts it wholesale, which is what makes a setting survive the next session start
// — writing the register directly did not, since session_start re-applies g_settings over it.
//
// SettingsShadow is trivially copyable (48 bytes), so it goes on the wire as-is; both ends are the
// same machine and build from the same header.
struct SetSettingsMsg {
    MsgHeader hdr;
    uint32_t  rate;         // 0 = leave the rate alone, just apply the settings
    uint32_t  reserved;
    ::uf::SettingsShadow settings;
};

// Mute and phase-invert for ONE crosspoint (uf::kMfMuted / uf::kMfInverted).
//
// Separate from SetMixerMsg rather than a field on it, because they are set independently and by
// different gestures: dragging a fader must not clear a mute, and muting must not have to know the
// gain. The daemon holds both and renders the quadlet.
// Sent as MASK + VALUE, applied as `flags = (flags & ~mask) | (value & mask)`, rather than a whole
// replacement word. A client that had to send all the flags would first have to READ them, and
// between its read and its write another client's change would be silently reverted. Toggling one
// bit should not require knowing the others.
struct SetCellFlagsMsg {
    MsgHeader hdr;
    uint16_t  srcKind;   // MixerSrcKind
    uint16_t  src;
    uint16_t  dest;
    uint16_t  mask;      // which uf::kMf* bits this message speaks for
    uint16_t  value;     // what to set them to
    uint16_t  pad;
};

// Mute and Loopback for one hardware output (uf::kMfMuted / uf::kMfRec).
//
// Loopback (manual §27.5) is device state, not a UI convenience: it sends that output's mix to the
// recording software in place of the corresponding hardware input, via the output-record mask at
// 0x801c0080. Changing it makes the daemon rewrite that whole 28-quadlet block.
struct SetOutFlagsMsg {
    MsgHeader hdr;
    uint16_t  out;
    uint16_t  mask;      // as SetCellFlagsMsg: mask + value, never a whole-word replace
    uint16_t  value;
    uint16_t  pad;
};

// Pair a channel into stereo, or split it back to mono. Host-side only — the device has no notion of
// it — but it belongs in the daemon because every front-end must agree about it, and because it
// should survive a restart along with the routing it describes.
struct SetStereoMsg {
    MsgHeader hdr;
    uint16_t  srcKind;   // MixerSrcKind — inputs and playback pair independently (manual §25.3)
    uint16_t  channel;   // either channel of the pair; pairs are (0,1), (2,3), ...
    uint16_t  on;
    uint16_t  pad;
};

// Copy or clear a submix — a whole destination column of the matrix (manual §27.2, §27.3).
//
// A column at a time rather than 28 SetMixer messages, so a copy either happens or does not: 28
// separate messages could be interleaved with someone else's edit and leave a half-copied submix.
struct SubmixMsg {
    MsgHeader hdr;
    uint16_t  from;      // ignored when clearing
    uint16_t  to;
    uint32_t  clear;     // 1 = clear `to`, 0 = copy `from` onto `to`
};

// The largest message we will ever read, so a receiver can use one fixed buffer and reject anything
// claiming to be bigger rather than trusting a length off the wire.
inline constexpr uint32_t kMaxMsgBytes = 256;

}  // namespace uf::ctl
