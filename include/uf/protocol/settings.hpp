// settings.hpp — FF800 extended-settings shadow + CR0/CR1/CR2 assembly (portable).
//
// The full analog/clock configuration is a host-side shadow; on any change, rebuild the three
// quadlets CR0/CR1/CR2 and block-write them to the conf register 0xfc88f014 (spec/02 §2.5,
// spec/06). Assembly adapted from FFADO fireface_hw.cpp::set_hardware_params (GPL-2.0); bit
// values from spec/06. Validated against the spec/09 §9.2 worked example.
//
// Spec-vs-driver note (spec/06 flags this [?]): FFADO unconditionally ORs
// CR2 FREQ0|FREQ1|DSPEED|QSSPEED (=0x1E, "hardwired in other drivers") and, via an `=`/`==`
// assignment bug, always sets WORD_CLOCK_1x. snd-fireface (the primary reference) never writes the
// CR block at all, and the spec/09 §9.2 vector has CR2=0x80000001. We follow snd-fireface/spec and
// leave FREQ/speed = 0 and word_clock_1x caller-controlled; confirm on the Sonoma FF800.
#pragma once
#include "uf/protocol/endian.hpp"
#include "uf/protocol/registers.hpp"

namespace uf {

// Input/output/phones gain ranges (spec/06). Names are the RME front-panel ranges.
enum class InputLevel { LoGain, P4dBu, M10dBV };
enum class OutputLevel { HiGain, P4dBu, M10dBV };
enum class PhonesLevel { P4dBu, M10dBV, HiGain };

// Per-input physical source select (FF800 input options, spec/06). Unset emits no bits — matches
// the spec/09 §9.2 vector, which omits input-option bits ("+ input-option bits as desired").
enum class InputSource { Unset, Front, Rear };

// Clock sync reference for the CR2 SYNC_REF field (spec/06).
enum class SyncRef { Adat1, Adat2, Spdif, WordClock, Tco };

// The complete FF800 configuration shadow. Defaults are a clean master-clock/coax/+4dBu setup.
struct SettingsShadow {
    // Phantom power per mic input (FF800 mics 7,8,9,10).
    bool phantom7 = false, phantom8 = false, phantom9 = false, phantom10 = false;

    InputLevel input_level = InputLevel::P4dBu;
    OutputLevel output_level = OutputLevel::P4dBu;
    PhonesLevel phones_level = PhonesLevel::P4dBu;

    bool filter = false;  // ch1 speaker-emulation / filter (FPGA LED)
    bool drive = false;   // ch1 instrument drive/fuzz (FPGA LED when true, else CPLD bit)

    InputSource input1 = InputSource::Unset;  // front uses filter variant when `filter` set
    InputSource input7 = InputSource::Unset;
    InputSource input8 = InputSource::Unset;

    // SPDIF.
    bool spdif_out_pro = false, spdif_out_emphasis = false, spdif_out_nonaudio = false;
    bool spdif_out_optical = false;  // optical SPDIF on ADAT2 port
    bool spdif_in_optical = false;   // false = coax

    // Clock.
    bool clock_master = true;               // true = master, false = autosync/slave
    SyncRef sync_ref = SyncRef::Adat1;      // sync reference when slaved
    bool word_clock_1x = false;             // single-speed word-clock out
    bool disable_limiter = false;           // FF800: only effective with front instrument input
};

// The three assembled conf-block quadlets, written together to 0xfc88f014.
struct ConfBlock {
    u32 cr0, cr1, cr2;
};

constexpr u32 assemble_cr0(const SettingsShadow& s) {
    u32 v = 0;
    if (s.phantom7) v |= cr0::kPhantomMic7;
    if (s.phantom8) v |= cr0::kPhantomMic8;
    if (s.phantom9) v |= cr0::kPhantomMic9;
    if (s.phantom10) v |= cr0::kPhantomMic10;
    if (s.filter) v |= cr0::kFilterCh1;
    if (s.drive) v |= cr0::kDriveCh1;  // FPGA LED mirror; paired CPLD drive bit is in CR1
    // Input level (FPGA LED mirror; the real level is CR1).
    switch (s.input_level) {
        case InputLevel::LoGain: v |= cr0::kInLevelLoGain; break;
        case InputLevel::P4dBu: v |= cr0::kInLevelP4dBu; break;
        case InputLevel::M10dBV: v |= cr0::kInLevelM10dBV; break;
    }
    switch (s.output_level) {
        case OutputLevel::HiGain: v |= cr0::kOutLevelHiGain; break;
        case OutputLevel::P4dBu: v |= cr0::kOutLevelP4dBu; break;
        case OutputLevel::M10dBV: v |= cr0::kOutLevelM10dBV; break;
    }
    switch (s.phones_level) {
        case PhonesLevel::P4dBu: v |= cr0::kPhonesP4dBu; break;  // 0
        case PhonesLevel::M10dBV: v |= cr0::kPhonesM10dBV; break;
        case PhonesLevel::HiGain: v |= cr0::kPhonesHiGain; break;
    }
    return v;
}

constexpr u32 assemble_cr1(const SettingsShadow& s) {
    u32 v = 0;
    // Actual input/output level (CPLD).
    switch (s.input_level) {
        case InputLevel::LoGain: v |= cr1::kInLevelLoGain; break;  // 0
        case InputLevel::P4dBu: v |= cr1::kInLevelP4dBu; break;
        case InputLevel::M10dBV: v |= cr1::kInLevelM10dBV; break;
    }
    switch (s.output_level) {
        case OutputLevel::HiGain: v |= cr1::kOutLevelHiGain; break;
        case OutputLevel::P4dBu: v |= cr1::kOutLevelP4dBu; break;
        case OutputLevel::M10dBV: v |= cr1::kOutLevelM10dBV; break;
    }
    if (s.drive) v |= cr1::kInstrumentDrive;  // CPLD drive; paired with the CR0 FPGA LED bit
    // Input 1: front uses the filter variant when the filter is on; else the plain-front bit.
    switch (s.input1) {
        case InputSource::Front: v |= s.filter ? cr1::kInput1FrontFilter : cr1::kInput1Front; break;
        case InputSource::Rear: v |= cr1::kInput1Rear; break;
        case InputSource::Unset: break;
    }
    switch (s.input7) {
        case InputSource::Front: v |= cr1::kInput7Front; break;
        case InputSource::Rear: v |= cr1::kInput7Rear; break;
        case InputSource::Unset: break;
    }
    switch (s.input8) {
        case InputSource::Front: v |= cr1::kInput8Front; break;
        case InputSource::Rear: v |= cr1::kInput8Rear; break;
        case InputSource::Unset: break;
    }
    return v;
}

constexpr u32 sync_ref_bits(SyncRef r) {
    switch (r) {
        case SyncRef::Adat1: return cr2::kSyncRefAdat1;
        case SyncRef::Adat2: return cr2::kSyncRefAdat2;
        case SyncRef::Spdif: return cr2::kSyncRefSpdif;
        case SyncRef::WordClock: return cr2::kSyncRefWordClock;
        case SyncRef::Tco: return cr2::kSyncRefTco;
    }
    return 0;
}

constexpr u32 assemble_cr2(const SettingsShadow& s) {
    u32 v = cr2::kDropAndStop;  // always set (spec/06; FFADO)
    if (s.clock_master) v |= cr2::kClockMaster;
    if (s.spdif_out_pro) v |= cr2::kSpdifOutPro;
    if (s.spdif_out_emphasis) v |= cr2::kSpdifOutEmphasis;
    if (s.spdif_out_nonaudio) v |= cr2::kSpdifOutNonAudio;
    if (s.spdif_out_optical) v |= cr2::kSpdifOutOptical;
    if (s.spdif_in_optical) v |= cr2::kSpdifInOptical;  // else coax (0)
    v |= sync_ref_bits(s.sync_ref);
    if (s.word_clock_1x) v |= cr2::kWordClock1x;
    if (s.disable_limiter) v |= cr2::kDisableLimiter;
    // FREQ0/FREQ1/DSPEED/QSSPEED intentionally left 0 (see file header).
    return v;
}

constexpr ConfBlock assemble_conf_block(const SettingsShadow& s) {
    return {assemble_cr0(s), assemble_cr1(s), assemble_cr2(s)};
}

}  // namespace uf
