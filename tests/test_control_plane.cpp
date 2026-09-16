// The clock-source vocabulary shared by the plugin (which publishes it to CoreAudio) and the daemon
// (which turns it back into register bits). Both ends must agree on the numbering, and the IDs are
// persisted by the OS/DAWs across launches, so renumbering silently changes what a saved setup
// selects — hence pinning the values, not just the round trip.
#include "../platform/shared/uf_control.hpp"
#include "uf_test.hpp"

using namespace uf;
using namespace uf::ctl;

UF_TEST(clock_source_ids_are_stable) {
    // Append-only. Changing any of these breaks saved selections.
    UF_CHECK_EQ((uint32_t)ClockSource::Internal, 0u);
    UF_CHECK_EQ((uint32_t)ClockSource::WordClock, 1u);
    UF_CHECK_EQ((uint32_t)ClockSource::Adat1, 2u);
    UF_CHECK_EQ((uint32_t)ClockSource::Adat2, 3u);
    UF_CHECK_EQ((uint32_t)ClockSource::Spdif, 4u);
    UF_CHECK_EQ((uint32_t)ClockSource::Tco, 5u);
    UF_CHECK(is_valid_clock_source(5));
    UF_CHECK(!is_valid_clock_source(6));
}

UF_TEST(only_internal_is_master) {
    UF_CHECK(clock_source_is_master(ClockSource::Internal));
    for (auto s : kAllClockSources)
        if (s != ClockSource::Internal && clock_source_is_master(s))
            UF_FAIL("a slaved source claimed master mode");
}

UF_TEST(every_source_maps_to_a_sync_ref) {
    UF_CHECK(clock_source_sync_ref(ClockSource::WordClock) == SyncRef::WordClock);
    UF_CHECK(clock_source_sync_ref(ClockSource::Adat1) == SyncRef::Adat1);
    UF_CHECK(clock_source_sync_ref(ClockSource::Adat2) == SyncRef::Adat2);
    UF_CHECK(clock_source_sync_ref(ClockSource::Spdif) == SyncRef::Spdif);
    UF_CHECK(clock_source_sync_ref(ClockSource::Tco) == SyncRef::Tco);
}

UF_TEST(status_round_trips_through_the_device_view) {
    // What we ask for, pushed through the device's own status encoding, must come back as the same
    // source — otherwise Audio MIDI Setup would show a different source than the one just selected.
    struct { ClockSource sel; SyncSource reported; } cases[] = {
        {ClockSource::WordClock, SyncSource::WordClock},
        {ClockSource::Adat1, SyncSource::Adat1},
        {ClockSource::Adat2, SyncSource::Adat2},
        {ClockSource::Spdif, SyncSource::Spdif},
        {ClockSource::Tco, SyncSource::Tco},
    };
    for (auto& c : cases)
        UF_CHECK(clock_source_from_status(false, c.reported) == c.sel);

    // Master overrides whatever the sync field says — the crystal is driving.
    UF_CHECK(clock_source_from_status(true, SyncSource::Adat1) == ClockSource::Internal);
    // A slaved device reporting no reference has fallen back; report Internal rather than a source
    // that is not actually locked.
    UF_CHECK(clock_source_from_status(false, SyncSource::None) == ClockSource::Internal);
}

UF_TEST(every_source_has_a_name) {
    for (auto s : kAllClockSources) {
        const std::string n = clock_source_name(s);
        if (n.empty() || n == "?") UF_FAIL("clock source without a name");
    }
    UF_CHECK_EQ(std::string(clock_source_name(ClockSource::Internal)), std::string("Internal"));
    UF_CHECK_EQ(std::string(clock_source_name(ClockSource::WordClock)), std::string("Word Clock"));
}

UF_TEST_MAIN()
