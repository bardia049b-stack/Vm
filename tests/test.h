/*
 * test.h -- a deliberately tiny assertion framework.
 *
 * No external test dependency: a few counters, a failure printer and a RUN
 * macro.  Keeps `make test` working on a bare CI runner and on Android.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_TEST_H
#define RVM_TEST_H

#include "../src/rvm.h"

#include "test_suites.h"

extern int g_checks;
extern int g_failed;
extern const char *g_current;

void test_begin(const char *name);
int test_end(void);

/* Negative tests deliberately provoke error logs; bracket them so the runner's
 * output stays readable without hiding a genuine failure. */
#define QUIET_BEGIN() rvm_log_set_level(RVM_LOG_OFF)
#define QUIET_END() rvm_log_set_level(RVM_LOG_ERROR)
void test_failf(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void test_summary(int suites_failed, int suites_run);

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        g_checks++;                                                                              \
        if (!(cond)) test_failf(__FILE__, __LINE__, "CHECK(%s) failed", #cond);                  \
    } while (0)

#define CHECK_U64(a, b)                                                                          \
    do {                                                                                         \
        u64 _x = (u64)(a), _y = (u64)(b);                                                        \
        g_checks++;                                                                              \
        if (_x != _y)                                                                            \
            test_failf(__FILE__, __LINE__, "%s != %s: got 0x%llx (%lld), want 0x%llx (%lld)",     \
                       #a, #b, (unsigned long long)_x, (long long)_x, (unsigned long long)_y,     \
                       (long long)_y);                                                           \
    } while (0)

#define CHECK_S64(a, b) CHECK_U64(a, b)

#define CHECK_STR(a, b)                                                                          \
    do {                                                                                         \
        g_checks++;                                                                              \
        if (strcmp((a), (b)) != 0)                                                               \
            test_failf(__FILE__, __LINE__, "strings differ: \"%s\" vs \"%s\"", (a), (b));         \
    } while (0)

#define CHECK_MEM(a, b, n)                                                                       \
    do {                                                                                         \
        g_checks++;                                                                              \
        if (memcmp((a), (b), (n)) != 0)                                                          \
            test_failf(__FILE__, __LINE__, "memory differs (%u bytes)", (unsigned)(n));           \
    } while (0)

#define RUN(fn)                                                                                  \
    do {                                                                                         \
        suites_run++;                                                                            \
        test_begin(#fn);                                                                         \
        fn();                                                                                    \
        if (test_end()) suites_failed++;                                                         \
    } while (0)

#endif /* RVM_TEST_H */
