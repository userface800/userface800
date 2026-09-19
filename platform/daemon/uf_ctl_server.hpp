// uf_ctl_server.hpp — the daemon's control socket: accept clients, parse commands, hand them to the
// pump. Header-only so the daemon stays a single translation unit.
//
// THREADING IS THE POINT. The pump is a tight realtime loop on a 1 kHz interrupt; accept(), read()
// and a client that stops reading must never happen anywhere near it. So: one thread does all socket
// I/O and pushes parsed commands into a small lock-free queue, and the pump drains that queue at the
// same safe point it already handles requestedRate. Nothing here blocks the audio path, and a client
// that hangs affects only itself.
//
// Deliberately NOT in shared memory. Commands need rendezvous (how does a newly launched UI
// announce itself and get a write slot?), liveness (a socket says the instant a client dies; shm
// cannot tell crashed from slow, and one that dies mid-write leaves a torn message and a slot
// nothing will ever free) and multi-writer serialisation, which a socket gives for free.
#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../shared/uf_ctl_proto.hpp"

namespace uf {

// One decoded command. Deliberately POD and tiny: it crosses into the realtime pump, so it must be
// copyable with no allocation and no destructor.
struct CtlCommand {
    ctl::MsgType type;
    uint16_t     srcKind = 0;
    uint16_t     src = 0;               // Submix: the column copied FROM
    uint16_t     dest = 0;              // Submix: the column copied TO / cleared
    uint16_t     out = 0;
    uint32_t     coeff = 0;
    uint16_t     mask = 0;              // SetCellFlags / SetOutFlags: which uf::kMf* bits
    uint16_t     flags = 0;             // ...and what to set them to
    uint16_t     on = 0;                // SetStereo: pair or split.  Submix: 1 = clear
    uint32_t     rate = 0;              // SetSettings only: 0 = leave the rate alone
    ::uf::SettingsShadow settings{};    // SetSettings only
};

// Single-producer (socket thread) / single-consumer (pump) ring. Same shape as the audio rings, and
// for the same reason: the consumer is realtime and may not take a lock.
//
// FULL MEANS DROP THE OLDEST, not block. A command queue backing up is a UI misbehaving, and the one
// thing that must not happen is a UI stalling audio. Dropping is safe here because commands are
// idempotent state-sets — losing an intermediate fader position while a later one lands is
// inaudible, which is the same reasoning that makes coalescing correct.
class CtlQueue {
public:
    static constexpr uint32_t kSlots = 256;   // power of two

    bool push(const CtlCommand& c) {          // socket thread
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= kSlots) {                // full: drop the oldest by advancing the consumer
            tail_.store(t + 1, std::memory_order_release);
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        slot_[h & (kSlots - 1)] = c;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(CtlCommand* out) {               // pump thread
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        *out = slot_[t & (kSlots - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    CtlCommand slot_[kSlots]{};
    std::atomic<uint64_t> head_{0}, tail_{0};
    std::atomic<uint32_t> dropped_{0};
};

class CtlServer {
public:
    // RAII, because main() has several early returns (Ctrl-C while waiting for the device, a failed
    // buffer mapping) that would otherwise skip the explicit stop() at the end and leave the socket
    // file behind. The startup unlink makes a stale one harmless, but leaving one is still untidy
    // and misleads anyone checking whether a daemon is up.
    ~CtlServer() { stop(); }

    // Starts the listener thread. Returns false (and logs) if the socket cannot be created — the
    // daemon must keep streaming regardless, since audio does not depend on the control plane.
    bool start(CtlQueue* q) {
        queue_ = q;
        path_ = socket_path();

        // A stale socket file outlives an unclean exit and would make bind() fail forever. It is
        // ours by name, so removing it is safe; the alternative is a daemon that never accepts
        // clients again after one crash.
        ::unlink(path_.c_str());

        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) { std::fprintf(stderr, "uf-daemon: ctl socket() failed\n"); return false; }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path_.size() >= sizeof(addr.sun_path)) {
            std::fprintf(stderr, "uf-daemon: ctl socket path too long: %s\n", path_.c_str());
            ::close(fd_); fd_ = -1; return false;
        }
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0 || ::listen(fd_, 8) < 0) {
            std::fprintf(stderr, "uf-daemon: ctl bind/listen failed on %s\n", path_.c_str());
            ::close(fd_); fd_ = -1; return false;
        }
        run_.store(true);
        thread_ = std::thread([this] { serve(); });
        std::printf("uf-daemon: control socket at %s\n", path_.c_str());
        return true;
    }

    void stop() {
        run_.store(false);
        if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
        if (thread_.joinable()) thread_.join();
        if (!path_.empty()) ::unlink(path_.c_str());
    }

    static std::string socket_path() {
        if (const char* o = getenv(ctl::kCtlSocketEnv)) return o;
        const char* tmp = getenv("TMPDIR");
        std::string dir = tmp ? tmp : "/tmp/";
        if (dir.empty() || dir.back() != '/') dir += '/';
        return dir + ctl::kCtlSocketName;
    }

private:
    void serve() {
        std::vector<pollfd> fds;
        fds.push_back({fd_, POLLIN, 0});
        while (run_.load()) {
            if (::poll(fds.data(), fds.size(), 200) <= 0) continue;   // timeout: re-check run_
            if (fds[0].revents & POLLIN) {
                int c = ::accept(fd_, nullptr, nullptr);
                if (c >= 0) fds.push_back({c, POLLIN, 0});
            }
            for (size_t i = 1; i < fds.size();) {
                if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    if (!handle(fds[i].fd)) {          // closed or protocol error: drop the client
                        ::close(fds[i].fd);
                        fds.erase(fds.begin() + (long)i);
                        continue;
                    }
                }
                ++i;
            }
        }
        for (size_t i = 1; i < fds.size(); ++i) ::close(fds[i].fd);
    }

    // Reads ONE message. Returns false to drop the client.
    //
    // Length is validated against kMaxMsgBytes before it is trusted: a length off the wire that is
    // believed without checking is how a parser walks off the end of its buffer.
    bool handle(int c) {
        ctl::MsgHeader h{};
        const ssize_t n = ::recv(c, &h, sizeof h, MSG_WAITALL);
        if (n != (ssize_t)sizeof h) return false;                       // EOF or short read
        if (h.length < sizeof h || h.length > ctl::kMaxMsgBytes) return false;

        uint8_t buf[ctl::kMaxMsgBytes];
        std::memcpy(buf, &h, sizeof h);
        const uint32_t rest = h.length - (uint32_t)sizeof h;
        if (rest && ::recv(c, buf + sizeof h, rest, MSG_WAITALL) != (ssize_t)rest) return false;

        if (h.version != ctl::kCtlVersion) {
            // Refuse EXPLICITLY rather than letting two programs quietly disagree about a struct.
            if (h.type == (uint16_t)ctl::MsgType::Hello) {
                ctl::HelloAckMsg ack{};
                ack.hdr = {ctl::kCtlVersion, (uint16_t)ctl::MsgType::HelloAck, sizeof ack};
                ack.ok = 0; ack.daemonVersion = ctl::kCtlVersion;
                (void)::send(c, &ack, sizeof ack, 0);
            }
            return false;
        }

        switch ((ctl::MsgType)h.type) {
            case ctl::MsgType::Hello: {
                ctl::HelloAckMsg ack{};
                ack.hdr = {ctl::kCtlVersion, (uint16_t)ctl::MsgType::HelloAck, sizeof ack};
                ack.ok = 1; ack.daemonVersion = ctl::kCtlVersion;
                return ::send(c, &ack, sizeof ack, 0) == (ssize_t)sizeof ack;
            }
            case ctl::MsgType::SetMixer: {
                if (h.length != sizeof(ctl::SetMixerMsg)) return false;
                const auto* m = reinterpret_cast<const ctl::SetMixerMsg*>(buf);
                queue_->push({.type = ctl::MsgType::SetMixer, .srcKind = m->srcKind,
                              .src = m->src, .dest = m->dest, .coeff = m->coeff});
                return true;
            }
            case ctl::MsgType::SetFader: {
                if (h.length != sizeof(ctl::SetFaderMsg)) return false;
                const auto* m = reinterpret_cast<const ctl::SetFaderMsg*>(buf);
                queue_->push({.type = ctl::MsgType::SetFader, .out = m->out, .coeff = m->coeff});
                return true;
            }
            case ctl::MsgType::SetSettings: {
                if (h.length != sizeof(ctl::SetSettingsMsg)) return false;
                const auto* m = reinterpret_cast<const ctl::SetSettingsMsg*>(buf);
                queue_->push({.type = ctl::MsgType::SetSettings, .rate = m->rate, .settings = m->settings});
                return true;
            }
            case ctl::MsgType::SetCellFlags: {
                if (h.length != sizeof(ctl::SetCellFlagsMsg)) return false;
                const auto* m = reinterpret_cast<const ctl::SetCellFlagsMsg*>(buf);
                queue_->push({.type = ctl::MsgType::SetCellFlags, .srcKind = m->srcKind,
                              .src = m->src, .dest = m->dest,
                              .mask = m->mask, .flags = m->value});
                return true;
            }
            case ctl::MsgType::SetOutFlags: {
                if (h.length != sizeof(ctl::SetOutFlagsMsg)) return false;
                const auto* m = reinterpret_cast<const ctl::SetOutFlagsMsg*>(buf);
                queue_->push({.type = ctl::MsgType::SetOutFlags, .out = m->out,
                              .mask = m->mask, .flags = m->value});
                return true;
            }
            case ctl::MsgType::SetStereo: {
                if (h.length != sizeof(ctl::SetStereoMsg)) return false;
                const auto* m = reinterpret_cast<const ctl::SetStereoMsg*>(buf);
                queue_->push({.type = ctl::MsgType::SetStereo, .srcKind = m->srcKind,
                              .src = m->channel, .on = m->on});
                return true;
            }
            case ctl::MsgType::Submix: {
                if (h.length != sizeof(ctl::SubmixMsg)) return false;
                const auto* m = reinterpret_cast<const ctl::SubmixMsg*>(buf);
                queue_->push({.type = ctl::MsgType::Submix, .src = m->from, .dest = m->to,
                              .on = (uint16_t)(m->clear ? 1 : 0)});
                return true;
            }
            default:
                return true;   // unknown but well-formed: skip it, stay in sync (length told us how)
        }
    }

    CtlQueue* queue_ = nullptr;
    int fd_ = -1;
    std::string path_;
    std::atomic<bool> run_{false};
    std::thread thread_;
};

}  // namespace uf
