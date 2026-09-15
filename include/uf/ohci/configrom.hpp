// configrom.hpp — IEEE-1212 config-ROM parsing for device identity (portable).
//
// After OHCI-2 gives us async reads, UFAsyncIO reads a node's config ROM (CSR space, base
// 0xffff_f0000400) and this extracts the identity fields the identity gate needs: module vendor OUI
// (root dir), and the unit directory's specifier_id / version / model_id. Feed the result to
// uf::is_ff800 (identity.hpp). Pure quadlet math — host-tested against a synthetic FF800 ROM.
//
// ROM layout (IEEE 1212): quad[0] = [bus_info_length:8][crc_length:8][rom_crc:16]; the bus-info
// block spans the next bus_info_length quadlets; then the root directory (a [length:16][crc:16]
// header followed by `length` entries), each entry = [key:8][value:24] where key = [type:2][id:6].
#pragma once
#include "uf/protocol/endian.hpp"
#include "uf/protocol/identity.hpp"

namespace uf::ohci {

// IEEE-1212 / CSR key ids (low 6 bits of the entry key byte).
inline constexpr u32 kKeyVendor = 0x03;      // Module_Vendor_ID (root dir, immediate)
inline constexpr u32 kKeyUnitDir = 0x11;     // Unit_Directory (root dir, directory → full byte 0xD1)
inline constexpr u32 kKeySpecifierId = 0x12; // Unit_Spec_ID (unit dir, immediate)
inline constexpr u32 kKeyVersion = 0x13;     // Unit_SW_Version (unit dir, immediate)
inline constexpr u32 kKeyModel = 0x17;       // Model_ID (unit dir, immediate)

// Entry-key type field (top 2 bits): 0=immediate, 1=CSR-offset, 2=leaf, 3=directory.
enum KeyType : u32 { kTypeImmediate = 0, kTypeCsrOffset = 1, kTypeLeaf = 2, kTypeDirectory = 3 };

struct RomIdentity {
    u32 vendor_id = 0;
    u32 specifier_id = 0;
    u32 model_id = 0;
    u32 version = 0;
    bool has_unit = false;   // a unit directory was found + parsed
};

constexpr u32 entry_key_id(u32 e) { return (e >> 24) & 0x3f; }
constexpr u32 entry_key_type(u32 e) { return (e >> 30) & 0x3; }
constexpr u32 entry_value(u32 e) { return e & 0x00ffffff; }

// Parse config-ROM quadlets (host order), `count` long, starting at the ROM base (bus-info header).
// Best-effort + bounds-checked: unknown/missing fields stay 0.
inline RomIdentity parse_config_rom(const u32* q, size_t count) {
    RomIdentity id{};
    if (count < 1) return id;

    const u32 bus_info_len = (q[0] >> 24) & 0xff;
    const size_t root = 1 + bus_info_len;            // index of the root-directory header
    if (root >= count) return id;

    const u32 root_len = (q[root] >> 16) & 0xffff;   // number of root-dir entries
    size_t unit_dir = 0;
    for (u32 i = 1; i <= root_len && root + i < count; ++i) {
        const u32 e = q[root + i];
        const u32 kid = entry_key_id(e);
        if (kid == kKeyVendor && entry_key_type(e) == kTypeImmediate) {
            id.vendor_id = entry_value(e);
        } else if (kid == kKeyUnitDir && entry_key_type(e) == kTypeDirectory) {
            unit_dir = root + i + entry_value(e);     // offset is relative to this entry
        }
    }

    if (unit_dir != 0 && unit_dir < count) {
        const u32 unit_len = (q[unit_dir] >> 16) & 0xffff;
        for (u32 i = 1; i <= unit_len && unit_dir + i < count; ++i) {
            const u32 e = q[unit_dir + i];
            switch (entry_key_id(e)) {
                case kKeySpecifierId: id.specifier_id = entry_value(e); break;
                case kKeyVersion: id.version = entry_value(e); break;
                case kKeyModel: id.model_id = entry_value(e); break;
                default: break;
            }
        }
        id.has_unit = true;
    }
    return id;
}

// The identity gate applied to a parsed ROM: is this node an RME Fireface 800?
inline bool rom_is_ff800(const RomIdentity& id) {
    return is_ff800(id.vendor_id, id.specifier_id, id.model_id, id.version);
}

}  // namespace uf::ohci
