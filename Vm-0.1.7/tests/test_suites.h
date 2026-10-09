/* SPDX-License-Identifier: MIT */
/*
 * test_suites.h -- one declaration per suite, shared by every test file.
 *
 * -Wmissing-prototypes wants a visible prototype before each non-static
 * definition, and the runner needs the same list; keeping it here means a new
 * suite is declared exactly once.  test.h pulls this in, so any test_*.c file
 * only has to add its definition and a RUN() line in test_main.c.
 */
#ifndef RVM_TEST_SUITES_H
#define RVM_TEST_SUITES_H

void test_cpu_alu(void);
void test_cpu_imm(void);
void test_cpu_shifts(void);
void test_cpu_loads_stores(void);
void test_cpu_branches(void);
void test_cpu_jumps(void);
void test_cpu_upper(void);
void test_cpu_word_ops(void);
void test_cpu_mul_div(void);
void test_cpu_amo(void);
void test_cpu_compressed(void);
void test_cpu_csr(void);
void test_cpu_trap_mret(void);
void test_cpu_fp(void);
void test_cpu_illegal(void);
void test_cpu_trace_gate(void);
void test_mmu_bare(void);
void test_mmu_sv39(void);
void test_mmu_sv48(void);
void test_mmu_permissions(void);
void test_mmu_tlb(void);
void test_mmu_identity_exec(void);
void test_uart_hello(void);
void test_uart_fifo_irq(void);
void test_clint_mtimecmp(void);
void test_plic_claim(void);
void test_virtio_transport(void);
void test_virtio_blk_roundtrip(void);
void test_loader_elf(void);
void test_loader_blob(void);
void test_fdt_blob(void);

#endif /* RVM_TEST_SUITES_H */
