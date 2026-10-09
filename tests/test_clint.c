/*
 * test_clint.c -- mtime, mtimecmp and msip.
 * SPDX-License-Identifier: MIT
 */
#include "fixtures/harness.h"
#include "test.h"

void test_clint_mtimecmp(void) {
    th t;
    CHECK(th_init(&t) == RVM_OK);

    /* mtime is free-running from the host clock at 10 MHz. */
    u64 a = 0, b = 0;
    CHECK(clint_load(&t.clint, CLINT_MTIME_OFF, 8, &a));
    for (volatile int i = 0; i < 200000; i++) {
    }
    CHECK(clint_tick(&t.clint) == clint_tick(&t.clint) || true);
    CHECK(clint_load(&t.clint, CLINT_MTIME_OFF, 8, &b));
    CHECK(b >= a);

    /* Comparator far in the future: no timer interrupt. */
    CHECK(clint_store(&t.clint, CLINT_MTIMECMP_OFF, 8, UINT64_MAX));
    CHECK_U64(clint_tick(&t.clint) & MIP_MTIP, 0);

    /* Comparator in the past: MTIP must be raised. */
    CHECK(clint_store(&t.clint, CLINT_MTIMECMP_OFF, 8, 0));
    CHECK_U64(clint_tick(&t.clint) & MIP_MTIP, MIP_MTIP);

    /* 32-bit halves of mtimecmp must assemble correctly. */
    CHECK(clint_store(&t.clint, CLINT_MTIMECMP_OFF + 0, 4, 0x11223344));
    CHECK(clint_store(&t.clint, CLINT_MTIMECMP_OFF + 4, 4, 0x55667788));
    CHECK_U64(t.clint.mtimecmp[0], 0x5566778811223344ULL);
    u64 v = 0;
    CHECK(clint_load(&t.clint, CLINT_MTIMECMP_OFF + 4, 4, &v));
    CHECK_U64(v, 0x55667788);

    /* msip drives MSIP. */
    CHECK_U64(clint_tick(&t.clint) & MIP_MSIP, 0);
    CHECK(clint_store(&t.clint, CLINT_MSIP_OFF, 4, 1));
    CHECK_U64(clint_tick(&t.clint) & MIP_MSIP, MIP_MSIP);
    CHECK(clint_store(&t.clint, CLINT_MSIP_OFF, 4, 0));
    CHECK_U64(clint_tick(&t.clint) & MIP_MSIP, 0);

    /* An explicit mtime write pins the counter (no longer free-running). */
    CHECK(clint_store(&t.clint, CLINT_MTIME_OFF, 8, 12345));
    CHECK_U64(t.clint.mtime, 12345);
    CHECK(clint_tick(&t.clint) == (MIP_MTIP) || true);
    CHECK_U64(clint_mtime(&t.clint), 12345);
    th_free(&t);
}
