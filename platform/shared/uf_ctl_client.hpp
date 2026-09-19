// uf_ctl_client.hpp — connect to the daemon's control socket and send commands.
//
// Shared by every front-end (the CLIs and the TUI today, a GUI later), so the connect/handshake
// dance exists once. Clients do NOT open a dext user client: the dext serialises nothing, so two
// user clients issuing register transactions interleave inside its single AT context. Going through
// the daemon removes that hazard and is what lets a change survive a rate change, since the daemon
// holds the mixer shadow.
//
// Blocking and synchronous, which is right for a CLI or a UI event handler. Any realtime caller —
// should one ever exist — must NOT use this from an audio thread: push onto a lock-free queue and
// let a helper thread send it.
#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "uf_ctl_proto.hpp"

namespace uf::ctl {

class Client {
public:
    ~Client() { close(); }

    // Returns false and fills `err` on failure — the caller decides how loudly to complain.
    bool connect(std::string* err, uint32_t clientKind = 0) {
        path_ = socket_path();
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) { *err = "socket() failed"; return false; }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            *err = "no daemon listening at " + path_ + " — is uf-daemon running?";
            close();
            return false;
        }

        // Handshake first, always. A version mismatch is refused EXPLICITLY rather than letting two
        // programs quietly disagree about a struct layout.
        HelloMsg h{};
        h.hdr = {kCtlVersion, (uint16_t)MsgType::Hello, sizeof h};
        h.clientKind = clientKind;
        if (!send_all(&h, sizeof h)) { *err = "handshake send failed"; close(); return false; }

        HelloAckMsg ack{};
        if (::recv(fd_, &ack, sizeof ack, MSG_WAITALL) != (ssize_t)sizeof ack) {
            *err = "daemon closed the connection during handshake";
            close();
            return false;
        }
        if (!ack.ok) {
            *err = "protocol version mismatch: this tool speaks v" + std::to_string(kCtlVersion) +
                   ", the daemon speaks v" + std::to_string(ack.daemonVersion) +
                   " — rebuild both from the same tree";
            close();
            return false;
        }
        return true;
    }

    bool set_mixer(MixerSrcKind kind, uint16_t src, uint16_t dest, uint32_t coeff) {
        SetMixerMsg m{};
        m.hdr = {kCtlVersion, (uint16_t)MsgType::SetMixer, sizeof m};
        m.srcKind = (uint16_t)kind; m.src = src; m.dest = dest; m.coeff = coeff;
        return send_all(&m, sizeof m);
    }

    bool set_fader(uint16_t out, uint32_t coeff) {
        SetFaderMsg m{};
        m.hdr = {kCtlVersion, (uint16_t)MsgType::SetFader, sizeof m};
        m.out = out; m.coeff = coeff;
        return send_all(&m, sizeof m);
    }

    // `mask` says which flags this call speaks for; `value` what to set them to. Anything outside
    // the mask is left as the daemon has it, so two clients toggling different bits cannot clobber
    // each other.
    bool set_cell_flags(MixerSrcKind kind, uint16_t src, uint16_t dest,
                        uint16_t mask, uint16_t value) {
        SetCellFlagsMsg m{};
        m.hdr = {kCtlVersion, (uint16_t)MsgType::SetCellFlags, sizeof m};
        m.srcKind = (uint16_t)kind; m.src = src; m.dest = dest;
        m.mask = mask; m.value = value;
        return send_all(&m, sizeof m);
    }

    bool set_out_flags(uint16_t out, uint16_t mask, uint16_t value) {
        SetOutFlagsMsg m{};
        m.hdr = {kCtlVersion, (uint16_t)MsgType::SetOutFlags, sizeof m};
        m.out = out; m.mask = mask; m.value = value;
        return send_all(&m, sizeof m);
    }

    bool set_stereo(MixerSrcKind kind, uint16_t channel, bool on) {
        SetStereoMsg m{};
        m.hdr = {kCtlVersion, (uint16_t)MsgType::SetStereo, sizeof m};
        m.srcKind = (uint16_t)kind; m.channel = channel; m.on = on ? 1 : 0;
        return send_all(&m, sizeof m);
    }

    bool copy_submix(uint16_t from, uint16_t to) {
        SubmixMsg m{};
        m.hdr = {kCtlVersion, (uint16_t)MsgType::Submix, sizeof m};
        m.from = from; m.to = to; m.clear = 0;
        return send_all(&m, sizeof m);
    }

    bool clear_submix(uint16_t out) {
        SubmixMsg m{};
        m.hdr = {kCtlVersion, (uint16_t)MsgType::Submix, sizeof m};
        m.from = 0; m.to = out; m.clear = 1;
        return send_all(&m, sizeof m);
    }

    bool set_settings(const ::uf::SettingsShadow& st, uint32_t rate) {
        SetSettingsMsg m{};
        m.hdr = {kCtlVersion, (uint16_t)MsgType::SetSettings, sizeof m};
        m.rate = rate;
        m.settings = st;
        return send_all(&m, sizeof m);
    }

    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

    static std::string socket_path() {
        if (const char* o = getenv(kCtlSocketEnv)) return o;
        const char* tmp = getenv("TMPDIR");
        std::string dir = tmp ? tmp : "/tmp/";
        if (dir.empty() || dir.back() != '/') dir += '/';
        return dir + kCtlSocketName;
    }

private:
    bool send_all(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        while (n) {
            const ssize_t w = ::send(fd_, b, n, 0);
            if (w <= 0) return false;
            b += w; n -= (size_t)w;
        }
        return true;
    }

    int fd_ = -1;
    std::string path_;
};

}  // namespace uf::ctl
