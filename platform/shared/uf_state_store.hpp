// uf_state_store.hpp — persist the daemon's device state across restarts.
//
// The mixer shadow already survives a rate change, a wedge recovery and a bus reset. It did
// not survive the daemon exiting, so a reboot or a launchd restart still lost your monitoring. This
// is the other half of "save and restore device state".
//
// Deliberately dumb: a magic, a version, and the raw 2048-quadlet matrix. No schema, no partial
// upgrades — a version we do not recognise is IGNORED rather than guessed at, and the daemon comes
// up with defaults. Silently misreading a saved state would put unknown gains on someone's outputs,
// which is a worse outcome than losing a routing.
//
// The file is written from a SNAPSHOT taken by the pump, never from the live shadow: see the daemon
// for why. Pure logic (a path and a byte buffer), so it is unit-testable.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace uf {

inline constexpr uint32_t kStateMagic   = 0x55465331;   // "UFS1"

// v1 was the rendered 2048-quadlet matrix. v2 is the MixerModel blob — gains and flags kept apart,
// so a mute survives a restart with the gain still under it (uf_mixer_model.hpp). v1 files are still
// READ, and adopted into the model, so upgrading does not throw away someone's routing.
inline constexpr uint32_t kStateVersionCells = 1;
inline constexpr uint32_t kStateVersion      = 2;

struct StateHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t bytes;      // payload size; in v1 this counted CELLS, and v1 is read by that rule
    uint32_t reserved;
};

// ~/Library/Application Support/UserFace800/state.bin — the macOS home for app data, and $HOME so it
// works for whoever runs it rather than whoever wrote it.
inline std::string state_dir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/UserFace800";
}
inline std::string state_path() {
    if (const char* o = getenv("UF_STATE_FILE")) return o;   // tests, and anyone wanting it elsewhere
    return state_dir() + "/state.bin";
}

// Write atomically: a partial file left by a crash mid-write would be read back as garbage gains on
// the outputs. Write a temp file, then rename — rename is atomic, so a reader sees the old file or
// the new one and never half of either.
inline bool save_blob(const std::string& path, const void* data, uint32_t bytes) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    StateHeader h{kStateMagic, kStateVersion, bytes, 0};
    bool ok = std::fwrite(&h, sizeof h, 1, f) == 1
           && std::fwrite(data, 1, bytes, f) == bytes;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { std::remove(tmp.c_str()); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    return true;
}

// Reads whichever version is on disk into `data`, up to `cap` bytes.
//
// Returns the version read, or 0 if the file is absent, truncated, or carries a magic/version we do
// not know — in every one of those cases the caller carries on with defaults rather than trying to
// salvage it. `*bytesOut` gets the payload size, which is how a caller tells a v1 matrix from a v2
// model without a second read.
inline uint32_t load_blob(const std::string& path, void* data, uint32_t cap, uint32_t* bytesOut) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    StateHeader h{};
    uint32_t version = 0;
    if (std::fread(&h, sizeof h, 1, f) == 1 && h.magic == kStateMagic) {
        // v1's header counted cells, not bytes. Normalising here keeps the difference in one place
        // instead of leaking a "which unit is this" question into every caller.
        const uint32_t bytes = (h.version == kStateVersionCells) ? h.bytes * 4u : h.bytes;
        if ((h.version == kStateVersionCells || h.version == kStateVersion) && bytes && bytes <= cap
            && std::fread(data, 1, bytes, f) == bytes) {
            version = h.version;
            if (bytesOut) *bytesOut = bytes;
        }
    }
    std::fclose(f);
    return version;
}

}  // namespace uf
