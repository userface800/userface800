// Persisting the mixer state across daemon restarts. Pure file logic — the interesting cases are all
// about REFUSING a file we cannot trust, because silently misreading one would put unknown gains on
// somebody's outputs at startup.
#include "../platform/shared/uf_state_store.hpp"
#include "uf_test.hpp"

#include <cstdio>
#include <vector>

namespace {
const char* kPath = "/tmp/userface800_test_state.bin";
std::vector<uint32_t> pattern(uint32_t n) {
    std::vector<uint32_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = i * 7 + 1;
    return v;
}
uint32_t bytes_of(const std::vector<uint32_t>& v) { return (uint32_t)(v.size() * 4); }
}  // namespace

UF_TEST(round_trips) {
    auto out = pattern(2048);
    std::remove(kPath);
    UF_CHECK(uf::save_blob(kPath, out.data(), bytes_of(out)));
    std::vector<uint32_t> in(2048, 0);
    uint32_t n = 0;
    UF_CHECK_EQ(uf::load_blob(kPath, in.data(), bytes_of(in), &n), uf::kStateVersion);
    UF_CHECK_EQ(n, bytes_of(out));
    UF_CHECK(in == out);
    std::remove(kPath);
}

UF_TEST(missing_file_is_not_an_error_just_zero) {
    std::remove(kPath);
    std::vector<uint32_t> in(2048, 0xAA);
    UF_CHECK_EQ(uf::load_blob(kPath, in.data(), bytes_of(in), nullptr), 0u);
    UF_CHECK_EQ(in[0], 0xAAu);          // caller's buffer untouched — defaults survive
}

UF_TEST(a_payload_larger_than_the_buffer_is_refused) {
    auto out = pattern(2048);
    std::remove(kPath);
    UF_CHECK(uf::save_blob(kPath, out.data(), bytes_of(out)));
    std::vector<uint32_t> in(1024, 0);
    // Refuse, do not truncate: half a matrix is not a matrix.
    UF_CHECK_EQ(uf::load_blob(kPath, in.data(), bytes_of(in), nullptr), 0u);
    UF_CHECK_EQ(in[0], 0u);
    std::remove(kPath);
}

UF_TEST(a_bad_magic_or_version_is_refused) {
    std::remove(kPath);
    uint32_t cells[4] = {1, 2, 3, 4};
    uf::StateHeader h{0xDEADBEEF, uf::kStateVersion, sizeof cells, 0};
    FILE* f = std::fopen(kPath, "wb");
    std::fwrite(&h, sizeof h, 1, f); std::fwrite(cells, 1, sizeof cells, f); std::fclose(f);
    std::vector<uint32_t> in(4, 0);
    UF_CHECK_EQ(uf::load_blob(kPath, in.data(), 16, nullptr), 0u);

    h = {uf::kStateMagic, uf::kStateVersion + 99, sizeof cells, 0};
    f = std::fopen(kPath, "wb");
    std::fwrite(&h, sizeof h, 1, f); std::fwrite(cells, 1, sizeof cells, f); std::fclose(f);
    UF_CHECK_EQ(uf::load_blob(kPath, in.data(), 16, nullptr), 0u);
    std::remove(kPath);
}

// v1 files hold the rendered 2048-quadlet matrix and counted CELLS in the header where v2 counts
// BYTES. Still readable, and reported as v1 so the caller knows to adopt it into the model rather
// than treat it as a model blob — otherwise upgrading the daemon would silently reset the mixer.
UF_TEST(a_v1_file_is_still_read_and_reported_as_v1) {
    std::remove(kPath);
    auto out = pattern(2048);
    uf::StateHeader h{uf::kStateMagic, uf::kStateVersionCells, (uint32_t)out.size(), 0};
    FILE* f = std::fopen(kPath, "wb");
    std::fwrite(&h, sizeof h, 1, f);
    std::fwrite(out.data(), 4, out.size(), f);
    std::fclose(f);

    std::vector<uint32_t> in(2048, 0);
    uint32_t n = 0;
    UF_CHECK_EQ(uf::load_blob(kPath, in.data(), bytes_of(in), &n), uf::kStateVersionCells);
    UF_CHECK_EQ(n, bytes_of(out));      // normalised to bytes, so callers need not know the rule
    UF_CHECK(in == out);
    std::remove(kPath);
}

UF_TEST(a_truncated_file_is_refused) {
    auto out = pattern(2048);
    std::remove(kPath);
    UF_CHECK(uf::save_blob(kPath, out.data(), bytes_of(out)));
    // Chop the tail: a crash mid-write, or a full disk.
    FILE* f = std::fopen(kPath, "rb");
    std::vector<char> all(16 + 2048 * 4);
    size_t n = std::fread(all.data(), 1, all.size(), f); std::fclose(f);
    f = std::fopen(kPath, "wb"); std::fwrite(all.data(), 1, n / 2, f); std::fclose(f);

    std::vector<uint32_t> in(2048, 0);
    UF_CHECK_EQ(uf::load_blob(kPath, in.data(), bytes_of(in), nullptr), 0u);
    std::remove(kPath);
}

UF_TEST(save_is_atomic_leaving_no_temp_behind) {
    auto out = pattern(64);
    std::remove(kPath);
    UF_CHECK(uf::save_blob(kPath, out.data(), bytes_of(out)));
    // rename() means a reader sees the old file or the new one, never half of either.
    const std::string tmp = std::string(kPath) + ".tmp";
    FILE* leftover = std::fopen(tmp.c_str(), "rb");
    UF_CHECK(leftover == nullptr);
    if (leftover) std::fclose(leftover);
    std::remove(kPath);
}

UF_TEST_MAIN()
