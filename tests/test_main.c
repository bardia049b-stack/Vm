/*
 * test_main.c -- the runner.  Suites are declared in test_suites.h; adding a
 * new test_*.c file means one RUN() line here and nothing else.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test.h"

#include <stdarg.h>

int g_checks = 0;
int g_failed = 0;
const char *g_current = "";

static int g_verbose = 0;

void test_begin(const char *name) {
    g_current = name;
    g_failed = 0;
    if (g_verbose)
        printf("  %-28s ", name);
}

int test_end(void) {
    if (g_failed == 0) {
        if (g_verbose)
            printf("ok\n");
    } else {
        if (!g_verbose)
            printf("  %-28s ", g_current);
        printf("FAILED (%d)\n", g_failed);
    }
    return g_failed != 0;
}

void test_failf(const char *file, int line, const char *fmt, ...) {
    g_failed++;
    if (!g_verbose) {
        printf("  %-28s ", g_current);
    }
    printf("  %s:%d: ", file, line);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void test_summary(int suites_failed, int suites_run) {
    RVM_UNUSED(suites_failed);
    RVM_UNUSED(suites_run);
}

/* ------------------------------------------------------------- suites */

int main(int argc, char **argv) {
    g_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    /* Keep the library quiet unless a test explicitly asks for logs. */
    rvm_log_set_level(RVM_LOG_ERROR);

    int suites_run = 0, suites_failed = 0;

    printf("RVM test suite\n");
    printf("==============\n");

    printf("[cpu]\n");
    RUN(test_cpu_alu);
    RUN(test_cpu_imm);
    RUN(test_cpu_shifts);
    RUN(test_cpu_loads_stores);
    RUN(test_cpu_branches);
    RUN(test_cpu_jumps);
    RUN(test_cpu_upper);
    RUN(test_cpu_word_ops);
    RUN(test_cpu_mul_div);
    RUN(test_cpu_amo);
    RUN(test_cpu_compressed);
    RUN(test_cpu_csr);
    RUN(test_cpu_trap_mret);
    RUN(test_cpu_fp);
    RUN(test_cpu_illegal);
    RUN(test_cpu_trace_gate);

    printf("[mmu]\n");
    RUN(test_mmu_bare);
    RUN(test_mmu_sv39);
    RUN(test_mmu_sv48);
    RUN(test_mmu_permissions);
    RUN(test_mmu_tlb);
    RUN(test_mmu_identity_exec);

    printf("[devices]\n");
    RUN(test_uart_hello);
    RUN(test_uart_fifo_irq);
    RUN(test_clint_mtimecmp);
    RUN(test_clint_virtual_time);
    RUN(test_plic_claim);
    RUN(test_virtio_transport);
    RUN(test_virtio_blk_roundtrip);
    RUN(test_virtio_blk_long_chain);

    printf("[net]\n");
    RUN(test_net_user_bulk);

    printf("[loader]\n");
    RUN(test_loader_elf);
    RUN(test_loader_blob);
    RUN(test_fdt_blob);

    printf("==============\n");
    if (suites_failed == 0) {
        printf("PASS: %d suites, %d assertions\n", suites_run, g_checks);
        return 0;
    }
    printf("FAIL: %d/%d suites failed, %d assertions\n", suites_failed, suites_run, g_checks);
    return 1;
}
