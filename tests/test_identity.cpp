// RME unit identification.
#include "uf/protocol/identity.hpp"
#include "uf_test.hpp"

using namespace uf;

UF_TEST(rme_vendor_filter) {
    UF_CHECK(is_rme_vendor(kOuiRme));
    UF_CHECK(is_rme_vendor(0x000a35));
    UF_CHECK(!is_rme_vendor(0x00130e));  // Focusrite, e.g. — not RME
    UF_CHECK(!is_rme_vendor(0));
}

UF_TEST(model_from_version) {
    UF_CHECK(model_from_unit_version(0x000001) == UFModel::FF800);
    UF_CHECK(model_from_unit_version(0x000002) == UFModel::FF400);
    UF_CHECK(model_from_unit_version(0x000005) == UFModel::FF802);
    UF_CHECK(model_from_unit_version(0) == UFModel::Unknown);
    UF_CHECK(model_from_unit_version(0x999) == UFModel::Unknown);
}

UF_TEST(ff800_gate) {
    // A real FF800 unit: vendor OUI, specifier, model id all RME; version FF800.
    UF_CHECK(is_ff800(kOuiRme, kOuiRme, kModelIdFireface, 0x000001));
    // Right family, wrong model — an FF400 must be rejected (not precluded later).
    UF_CHECK(!is_ff800(kOuiRme, kOuiRme, kModelIdFireface, 0x000002));
    // Non-RME vendor.
    UF_CHECK(!is_ff800(0x00130e, kOuiRme, kModelIdFireface, 0x000001));
    // Wrong model id.
    UF_CHECK(!is_ff800(kOuiRme, kOuiRme, 0x000000, 0x000001));
}

UF_TEST_MAIN()
