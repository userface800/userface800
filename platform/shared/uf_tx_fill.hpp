// uf_tx_fill.hpp — what samples go into a transmit packet, frame by frame.
//
// This is the daemon's paced-fill inner loop, lifted out of uf-daemon.cpp so it can be tested with
// no FF800 attached. That is not tidiness: the bug this replaces was fifteen lines of pure logic
// that no test could reach while it lived inside the pump.
//
// The bug — worth stating precisely, because the shape of this class is a direct response to it. The
// old loop latched a single `dry` flag for a whole pump tick. One failed acquireRead() therefore
// silenced every remaining frame of that tick, ~256 frames at 192 kHz, so a ring that was empty for
// an instant cost a millisecond of audio. Under load 300-400 of the 750 ticks/s latched, gating
// roughly 40% of the output to silence in ragged chunks — which is a ring modulator, and is exactly
// what it sounded like.
//
// So the two rules this encodes are:
//   1. the dry latch is scoped to a PACKET, never to a tick — beginPacket() resets it, and a miss
//      costs one packet (sytInterval frames, 125 us at 192 kHz) rather than the rest of the tick;
//   2. a miss repeats the last real frame rather than writing zeros, decaying to silence so a long
//      dry spell fades out instead of parking DC on the analog outputs.
//
// Deliberately NOT per-frame retries: that would hammer acquireRead() and bury the ring's underrun
// counter, which is the one number that says the safety cushion is too small.
#pragma once
#include <cstdint>
#include <cstring>
#include "uf_shm_ring.hpp"

namespace uf {

class TxFill {
public:
    // channels: how many the device carries at this rate (dbq) — 12 at 4x, 20 at 2x, 28 at 1x.
    explicit TxFill(uint32_t channels)
        : channels_(channels > shm::kMaxChannels ? shm::kMaxChannels : channels) {}

    // Start a pump tick. Clears the per-tick statistics only; the held frame and the ring cursor
    // deliberately persist, because they are the state that makes a gap continuous rather than a
    // discontinuity.
    void beginTick() { dryTick_ = false; }

    // Start a packet: this is what re-arms the single acquire attempt. Calling it per packet rather
    // than per tick IS the fix.
    void beginPacket() { dryPacket_ = false; }

    // One frame, `channels()` samples wide, top-justified int32 as the ring carries them. Never
    // returns null: on a ring miss it returns the decaying held frame.
    const int32_t* frame(shm::Ring& play) {
        if (slot_ && pos_ >= slot_->frameCount) { play.commitRead(); slot_ = nullptr; }
        if (!slot_ && !dryPacket_) {
            slot_ = play.acquireRead();
            pos_  = 0;
            if (!slot_) { dryPacket_ = true; dryTick_ = true; }
        }
        if (slot_) {
            const uint32_t nsrc = slot_->channelCount ? slot_->channelCount : channels_;
            const int32_t* src  = slot_->audio + (std::size_t)pos_ * nsrc;
            for (uint32_t c = 0; c < channels_; ++c) last_[c] = (c < nsrc) ? src[c] : 0;
            ++pos_;
            heldRun_ = 0;
        } else {
            // Decay toward zero with a ~256-frame time constant, but only from the SECOND held frame
            // on: the first repeat must be the last real sample exactly, or every gap starts with a
            // step of its own. Small positive values stick below 256, which the encoder's >>8 turns
            // into a true zero on the wire, so the output does reach silence.
            if (heldRun_) for (uint32_t c = 0; c < channels_; ++c) last_[c] -= last_[c] >> 8;
            ++heldRun_;
            ++held_;
        }
        return last_;
    }

    // Give the ring back any slot still held. Call before tearing a session down, or the ring's
    // consumer cursor never advances past it.
    void release(shm::Ring& play) {
        if (slot_) { play.commitRead(); slot_ = nullptr; pos_ = 0; }
    }

    // The plugin restarting hands us a different mapping; a slot pointer into the old one is an
    // orphan. Drop it without committing — that ring is gone.
    void reset() { slot_ = nullptr; pos_ = 0; dryPacket_ = false; dryTick_ = false; }

    uint32_t channels() const { return channels_; }
    bool     dryTick()  const { return dryTick_; }     // did any packet this tick come up empty
    uint64_t held()     const { return held_; }        // frames we had to invent, cumulative

private:
    const shm::Slot* slot_ = nullptr;
    uint32_t pos_ = 0;
    uint32_t channels_;
    uint64_t held_ = 0;
    uint32_t heldRun_ = 0;      // consecutive held frames; 0 means the last frame was real
    bool     dryPacket_ = false;
    bool     dryTick_   = false;
    int32_t  last_[shm::kMaxChannels] = {0};
};

}  // namespace uf
