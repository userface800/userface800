// uf_test.hpp — a tiny, dependency-free unit-test harness for the portable core.
//
// Rationale: the portable protocol core must be testable on any platform with no
// network fetches (Catch2/GoogleTest). This header gives just enough: self-registering test
// cases, a handful of CHECK macros, and a main() that runs all tests and reports a summary.
// One test executable per module links this header + its own translation unit.
//
// Usage:
//   #include "uf_test.hpp"
//   UF_TEST(name_of_test) { UF_CHECK_EQ(a, b); }
//   UF_TEST_MAIN()
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace uf::test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

// Thrown by a failing CHECK to abort the current case (but not the whole run).
struct Failure {
    std::string msg;
};

inline int& failure_count() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back(Case{name, std::move(fn)});
    }
};

// Format an integer both as decimal and hex for readable diffs.
template <typename T>
std::string show(const T& v) {
    if constexpr (std::is_integral_v<T>) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lld (0x%llx)",
                      static_cast<long long>(v),
                      static_cast<unsigned long long>(static_cast<uint64_t>(v)));
        return buf;
    } else {
        return std::string(v);
    }
}

inline int run_all() {
    int passed = 0, failed = 0;
    for (auto& c : registry()) {
        int before = failure_count();
        try {
            c.fn();
        } catch (const Failure& f) {
            std::printf("  ! %s\n", f.msg.c_str());
        } catch (const std::exception& e) {
            std::printf("  ! %s: unexpected exception: %s\n", c.name, e.what());
            failure_count()++;
        }
        if (failure_count() == before) {
            std::printf("[ PASS ] %s\n", c.name);
            ++passed;
        } else {
            std::printf("[ FAIL ] %s\n", c.name);
            ++failed;
        }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace uf::test

#define UF_TEST(NAME)                                                              \
    static void uf_test_##NAME();                                                  \
    static ::uf::test::Registrar uf_reg_##NAME(#NAME, uf_test_##NAME);             \
    static void uf_test_##NAME()

#define UF_FAIL(MSG)                                                               \
    do {                                                                           \
        ::uf::test::failure_count()++;                                             \
        throw ::uf::test::Failure(std::string(__FILE__) + ":" +                    \
                                  std::to_string(__LINE__) + ": " + (MSG));        \
    } while (0)

#define UF_CHECK(COND)                                                             \
    do {                                                                           \
        if (!(COND)) UF_FAIL(std::string("CHECK failed: ") + #COND);               \
    } while (0)

#define UF_CHECK_EQ(A, B)                                                          \
    do {                                                                           \
        auto _a = (A);                                                             \
        auto _b = (B);                                                             \
        if (!(_a == _b))                                                           \
            UF_FAIL(std::string("CHECK_EQ failed: " #A " == " #B "\n      lhs = ") \
                    + ::uf::test::show(_a) + "\n      rhs = " +                     \
                    ::uf::test::show(_b));                                          \
    } while (0)

#define UF_TEST_MAIN()                                                            \
    int main() { return ::uf::test::run_all(); }
