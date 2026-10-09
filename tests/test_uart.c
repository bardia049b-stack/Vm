/*
 * test_uart.c -- the ns16550a console.
 * SPDX-License-Identifier: MIT
 */
#include "fixtures/harness.h"
#include "test.h"

/*
 * "Hello, RVM!" is printed by real guest instructions: materialise the UART
 * base with lui, then a pair of addi/sb per character.  This is the same path
 * the Linux 8250 driver takes, only without the driver.
 */
void test_uart_hello(void) {
    th t;
    CHECK(th_init(&t) == RVM_OK);

    static const char msg[] = "Hello, RVM!\n";
    th_emit(&t, LUI(X_T0, 0x10000000)); /* 0x10000000 = UART0 */
    for (size_t i = 0; msg[i]; i++) {
        th_emit(&t, ADDI(X_T1, X_ZERO, (s8)msg[i]));
        th_emit(&t, SB(X_T1, X_T0, UART_THR));
    }
    th_run_all(&t);

    t.out[t.outlen] = 0;
    CHECK_STR((const char *)t.out, msg);
    CHECK_U64(t.uart.n_tx, sizeof(msg) - 1);
    th_free(&t);
}

/* DLAB, the receive FIFO, LSR bits and the interrupt line into the PLIC. */
void test_uart_fifo_irq(void) {
    th t;
    CHECK(th_init(&t) == RVM_OK);

    u64 lsr = 0, iir = 0;
    /* Nothing pending, transmitter empty. */
    CHECK(uart_load(&t.uart, UART_LSR, 1, &lsr));
    CHECK_U64(lsr & LSR_THRE, LSR_THRE);
    CHECK_U64(lsr & LSR_TEMT, LSR_TEMT);
    CHECK_U64(lsr & LSR_DR, 0);
    CHECK(uart_load(&t.uart, UART_IIR, 1, &iir));
    CHECK_U64(iir & 1, 1); /* bit0 = 1 means "no interrupt pending" */

    /* Enable the receive-data interrupt, then feed the FIFO from the host. */
    CHECK(uart_store(&t.uart, UART_IER, 1, IER_RDA));
    CHECK(uart_push(&t.uart, 'h'));
    CHECK(uart_push(&t.uart, 'i'));

    CHECK(uart_load(&t.uart, UART_LSR, 1, &lsr));
    CHECK_U64(lsr & LSR_DR, LSR_DR);
    CHECK(uart_load(&t.uart, UART_IIR, 1, &iir));
    CHECK_U64(iir & 0x0F, 0x04); /* received data available */

    /* The PLIC must have latched source 10 (the UART IRQ). */
    CHECK_U64(t.plic.pending[RVM_UART_IRQ / 32] & (1u << (RVM_UART_IRQ % 32)),
              1u << (RVM_UART_IRQ % 32));
    CHECK_U64(plic_update(&t.plic) & MIP_SEIP, 0); /* not enabled yet */
    t.plic.enable[PLIC_CTX_S][RVM_UART_IRQ / 32] |= 1u << (RVM_UART_IRQ % 32);
    t.plic.priority[RVM_UART_IRQ] = 1;
    CHECK_U64(plic_update(&t.plic) & MIP_SEIP, MIP_SEIP);

    /* Draining the FIFO returns the bytes in order and clears the IRQ. */
    u64 v = 0;
    CHECK(uart_load(&t.uart, UART_RBR, 1, &v));
    CHECK_U64(v, 'h');
    CHECK(uart_load(&t.uart, UART_RBR, 1, &v));
    CHECK_U64(v, 'i');
    CHECK_U64(t.uart.rx_count, 0);
    CHECK_U64(t.uart.irq_level, false);

    /* DLAB=1 must expose the divisor latches instead of THR/IER. */
    CHECK(uart_store(&t.uart, UART_LCR, 1, 0x83)); /* DLAB set, 8N1 */
    CHECK(uart_store(&t.uart, UART_DLL, 1, 0x24));
    CHECK(uart_load(&t.uart, UART_DLL, 1, &v));
    CHECK_U64(v, 0x24);
    CHECK(uart_store(&t.uart, UART_LCR, 1, 0x03)); /* DLAB clear again */
    CHECK(uart_load(&t.uart, UART_LCR, 1, &v));
    CHECK_U64(v, 0x03);

    /* Registers are byte-wide; a 4-byte access must be refused. */
    CHECK(!uart_load(&t.uart, UART_LSR, 4, &v));
    CHECK(!uart_store(&t.uart, UART_THR, 4, 0));
    th_free(&t);
}
