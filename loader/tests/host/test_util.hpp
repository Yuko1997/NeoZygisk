#pragma once

#include <cstdio>
#include <string>

inline int g_checks = 0;
inline int g_failures = 0;
inline std::string g_section;

inline void section(const std::string &name) {
    g_section = name;
    std::printf("-- %s\n", name.c_str());
}

inline void fail(const char *file, int line, const std::string &message) {
    g_failures++;
    std::printf("FAIL %s:%d [%s] %s\n", file, line, g_section.c_str(), message.c_str());
}

inline void check(bool condition, const char *file, int line, const char *expression) {
    g_checks++;
    if (!condition) {
        g_failures++;
        std::printf("FAIL %s:%d [%s] %s\n", file, line, g_section.c_str(), expression);
    }
}

inline void check_equal(const std::string &actual, const std::string &expected,
                        const char *file, int line, const char *expression) {
    g_checks++;
    if (actual != expected) {
        g_failures++;
        std::printf("FAIL %s:%d [%s] %s\n  expected: %s\n  actual:   %s\n", file, line,
                    g_section.c_str(), expression, expected.c_str(), actual.c_str());
    }
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(actual, expected) \
    check_equal((actual), (expected), __FILE__, __LINE__, #actual " == " #expected)

inline int test_summary(const char *suite) {
    std::printf("\n%s: %d checks, %d failure(s)\n", suite, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
