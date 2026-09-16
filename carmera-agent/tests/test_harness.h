#pragma once

#include <iostream>
#include <string>
#include <vector>

// Minimal, dependency-free test harness. Each TEST registers itself; run_all.cpp
// executes them and reports pass/fail counts.
namespace test {

struct Case {
    std::string   name;
    bool (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const std::string& n, bool (*f)()) {
        registry().push_back({n, f});
    }
};

} // namespace test

#define TEST(name)                                           \
    static bool name##_impl();                               \
    static ::test::Registrar name##_reg(#name, name##_impl); \
    static bool name##_impl()

#define ASSERT(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::cerr << "  ASSERT failed: " #cond " @ " __FILE__ ":"    \
                      << __LINE__ << "\n";                               \
            return false;                                                \
        }                                                                \
    } while (0)

#define ASSERT_EQ(a, b)                                                  \
    do {                                                                 \
        if (!((a) == (b))) {                                             \
            std::cerr << "  ASSERT_EQ failed: " #a " != " #b " @ "       \
                      << __FILE__ ":" << __LINE__ << "\n";               \
            return false;                                                \
        }                                                                \
    } while (0)
