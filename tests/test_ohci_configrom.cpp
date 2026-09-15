// OHCI — config-ROM identity parsing against a synthetic FF800 ROM.
#include <vector>
#include "uf/ohci/configrom.hpp"
#include "uf_test.hpp"

using namespace uf;
using namespace uf::ohci;

// Build a minimal but structurally-valid FF800 config ROM.
static std::vector<u32> ff800_rom() {
    return {
        //  idx 0: bus_info_length = 4, crc_length = 4, rom_crc (dummy)
        (4u << 24) | (4u << 16) | 0x0000,
        0x31333934,                    // idx 1: "1394" bus name
        0x20FF7000,                    // idx 2: bus-info capabilities (dummy)
        0x000A3512,                    // idx 3: GUID hi (OUI 0x000a35 in top bytes)
        0x34567890,                    // idx 4: GUID lo
        //  idx 5: root-dir header: length = 2 entries, crc (dummy)
        (2u << 16) | 0x0000,
        (kKeyVendor << 24) | 0x000A35, // idx 6: Module_Vendor_ID = RME OUI
        (0xD1u << 24) | 0x000002,      // idx 7: Unit_Directory, offset +2 -> idx 9
        0x00000000,                    // idx 8: padding
        //  idx 9: unit-dir header: length = 3 entries, crc (dummy)
        (3u << 16) | 0x0000,
        (kKeySpecifierId << 24) | 0x000A35, // idx 10: Unit_Spec_ID = RME OUI
        (kKeyVersion << 24) | 0x000001,     // idx 11: Unit_SW_Version = FF800
        (kKeyModel << 24) | 0x101800,       // idx 12: Model_ID = Fireface
    };
}

UF_TEST(parse_ff800_rom) {
    auto rom = ff800_rom();
    auto id = parse_config_rom(rom.data(), rom.size());
    UF_CHECK_EQ(id.vendor_id, 0x000A35u);
    UF_CHECK_EQ(id.specifier_id, 0x000A35u);
    UF_CHECK_EQ(id.model_id, 0x101800u);
    UF_CHECK_EQ(id.version, 0x000001u);
    UF_CHECK(id.has_unit);
    UF_CHECK(rom_is_ff800(id));   // the identity gate passes for a real FF800 ROM
}

UF_TEST(reject_ff400_rom) {
    auto rom = ff800_rom();
    rom[11] = (kKeyVersion << 24) | 0x000002;  // version FF400 instead of FF800
    auto id = parse_config_rom(rom.data(), rom.size());
    UF_CHECK_EQ(id.version, 0x000002u);
    UF_CHECK(!rom_is_ff800(id));   // right family, wrong model -> rejected
}

UF_TEST(robust_to_truncation) {
    auto rom = ff800_rom();
    // Truncated ROM (only the bus-info block) must not read out of bounds.
    auto id = parse_config_rom(rom.data(), 3);
    UF_CHECK(!id.has_unit);
    UF_CHECK(!rom_is_ff800(id));
    UF_CHECK_EQ(parse_config_rom(nullptr, 0).vendor_id, 0u);
}

UF_TEST_MAIN()
