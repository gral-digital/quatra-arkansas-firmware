/**
 * @file test_common.h
 * @brief Minimal assertion helpers for the host unit tests.
 *
 * Single-header, no dependencies. Each test executable links against the
 * Quatra sources directly (see tests/Makefile).
 */
#ifndef QUATRA_TEST_COMMON_H
#define QUATRA_TEST_COMMON_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int    g_test_pass = 0;
static int    g_test_fail = 0;
static const char *g_suite_name = "";

#define TEST_BEGIN(name) \
    do { g_test_pass = 0; g_test_fail = 0; g_suite_name = (name); \
         printf("\n== %s ==\n", g_suite_name); } while (0)

#define TEST_END() \
    do { printf("-- %s: %d passed, %d failed\n", \
                g_suite_name, g_test_pass, g_test_fail); \
         if (g_test_fail) return 1; } while (0)

#define CHECK(cond, fmt, ...) \
    do { \
        if (cond) { g_test_pass++; } \
        else { g_test_fail++; \
               printf("  FAIL %s:%d: " fmt "\n", \
                      __FILE__, __LINE__, ##__VA_ARGS__); } \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                    \
    do {                                                                     \
        double _a = (double)(actual), _e = (double)(expected),               \
               _t = (double)(tol);                                           \
        double _d = fabs(_a - _e);                                           \
        if (_d <= _t) { g_test_pass++; }                                     \
        else { g_test_fail++;                                                \
               printf("  FAIL %s:%d: %s ≈ %s (got %.6f, want %.6f ±%.6f, "  \
                      "diff %.6f)\n",                                        \
                      __FILE__, __LINE__, #actual, #expected,                \
                      _a, _e, _t, _d); }                                     \
    } while (0)

#define CHECK_EQ(actual, expected) \
    CHECK((long long)(actual) == (long long)(expected), \
          "%s == %s (got %lld, want %lld)", \
          #actual, #expected, (long long)(actual), (long long)(expected))

#define CHECK_TRUE(cond)  CHECK((cond), "%s should be true",  #cond)
#define CHECK_FALSE(cond) CHECK(!(cond), "%s should be false", #cond)

#endif /* QUATRA_TEST_COMMON_H */
