// identity.hpp — RME FireWire unit identification (portable core).
//
// The FF800 presents as a plain IOFireWireUnit (no AV/C), so discovery must (1) match
// IOFireWireUnit services and (2) filter them to RME by the config-ROM vendor OUI, then
// (3) confirm the specific model from the unit directory. This header holds the pure
// identification facts + predicates; the IOKit config-ROM reads live in the macOS layer.
//
// Facts adapted from Linux snd-fireface ff.c (GPL-2.0): OUI_RME, the unit-version enum, and
// the shared Fireface model_id 0x101800 (the unit *version* is what discriminates models).
#pragma once
#include "uf/protocol/endian.hpp"

namespace uf {

// RME's IEEE-1394 vendor OUI. Appears as both the config-ROM vendor_id and, in the unit
// directory, the specifier_id. (snd-fireface OUI_RME.)
inline constexpr u32 kOuiRme = 0x000a35;

// True if a matched FireWire unit's config-ROM vendor id is RME. This is the first filter
// applied to every IOFireWireUnit match before routing to the RME construction path.
constexpr bool is_rme_vendor(u32 vendor_id) { return vendor_id == kOuiRme; }

// Every Fireface shares one unit model_id; the unit *version* is the model discriminator.
// (snd-fireface snd_ff_id_table: all rows have model_id 0x101800, specifier_id OUI_RME.)
inline constexpr u32 kModelIdFireface = 0x101800;

// Unit-directory version values that discriminate the Fireface model (snd-fireface
// enum snd_ff_unit_version). We only drive the FF800; the rest are recognised so we can
// reject them cleanly without precluding future support.
enum class UFModel : u32 {
    Unknown = 0,
    FF800 = 0x000001,
    FF400 = 0x000002,
    UFX = 0x000003,
    UCX = 0x000004,
    FF802 = 0x000005,
};

// Map a unit-directory version to a model. Values outside the known set are Unknown.
constexpr UFModel model_from_unit_version(u32 version) {
    switch (version) {
        case static_cast<u32>(UFModel::FF800): return UFModel::FF800;
        case static_cast<u32>(UFModel::FF400): return UFModel::FF400;
        case static_cast<u32>(UFModel::UFX): return UFModel::UFX;
        case static_cast<u32>(UFModel::UCX): return UFModel::UCX;
        case static_cast<u32>(UFModel::FF802): return UFModel::FF802;
        default: return UFModel::Unknown;
    }
}

// The identity gate: a matched unit is a Fireface 800 iff its vendor OUI, specifier id, and
// model id are RME's and its unit version is FF800. specifier_id equals the vendor OUI for
// all Firefaces (snd-fireface id table), so it is checked against kOuiRme too.
constexpr bool is_ff800(u32 vendor_id, u32 specifier_id, u32 model_id, u32 version) {
    return is_rme_vendor(vendor_id) && specifier_id == kOuiRme &&
           model_id == kModelIdFireface &&
           model_from_unit_version(version) == UFModel::FF800;
}

}  // namespace uf
