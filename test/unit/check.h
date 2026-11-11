#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

// Minimal test harness: CHECK records a failure and keeps going.
namespace xlang_test {

inline int& failures() {
    static int count = 0;
    return count;
}

}  // namespace xlang_test

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ++xlang_test::failures();                                                      \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond << "\n"; \
        }                                                                                  \
    } while (false)

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        const auto& a_ = (actual);                                                         \
        const auto& e_ = (expected);                                                       \
        if (!(a_ == e_)) {                                                                 \
            ++xlang_test::failures();                                                      \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #actual     \
                      << "\n--- actual ---\n" << a_ << "\n--- expected ---\n" << e_ << "\n"; \
        }                                                                                  \
    } while (false)

// Runs one test function. With XLC_TEST_TRACE set, its name is printed first, so the last name on
// stderr identifies a test that crashes the process.
#define RUN(test)                                                                          \
    do {                                                                                   \
        if (std::getenv("XLC_TEST_TRACE") != nullptr) {                                    \
            std::cerr << "run " #test "\n";                                                \
        }                                                                                  \
        test();                                                                            \
    } while (false)
