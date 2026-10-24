#pragma once

// Minimal test harness so the C++ projects need no external test framework.
// Usage: CHECK(cond); CHECK_EQ(a, b); ... return netlab::test::finish();

#include <cstdio>
#include <sstream>

namespace netlab::test {

inline int& failures() {
  static int count = 0;
  return count;
}

inline int finish() {
  if (failures() > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures());
    return 1;
  }
  std::puts("all checks passed");
  return 0;
}

}  // namespace netlab::test

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++netlab::test::failures();                                                \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    const auto& check_a_ = (a);                                                         \
    const auto& check_b_ = (b);                                                         \
    if (!(check_a_ == check_b_)) {                                                      \
      std::ostringstream check_os_;                                                     \
      check_os_ << check_a_ << " != " << check_b_;                                      \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%s)\n", __FILE__, __LINE__, \
                   #a, #b, check_os_.str().c_str());                                    \
      ++netlab::test::failures();                                                       \
    }                                                                                   \
  } while (0)

#define CHECK_THROWS(expr)                                                           \
  do {                                                                               \
    bool check_threw_ = false;                                                       \
    try {                                                                            \
      (void)(expr);                                                                  \
    } catch (...) {                                                                  \
      check_threw_ = true;                                                           \
    }                                                                                \
    if (!check_threw_) {                                                             \
      std::fprintf(stderr, "%s:%d: expected exception from %s\n", __FILE__, __LINE__, #expr); \
      ++netlab::test::failures();                                                    \
    }                                                                                \
  } while (0)
