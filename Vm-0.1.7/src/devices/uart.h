/*
 * uart.h -- ns16550a compatible serial port.
 *
 * Linux' 8250 driver drives this; the device tree advertises
 * compatible = "ns16550a" with reg-shift = 0 and a 1-byte register stride.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_UART_H
#define RVM_UART_H

#include "../rvm.h"

#define UART_RX_SIZE 4096

/* Register offsets */
#define UART_RBR 0 /* receive buffer  (DLAB=0) / divisor latch low  (DLAB=1) */
#define UART_THR 0 /* transmit holding (DLAB=0) */
#define UART_DLL 0
#define UART_IER 1
#define UART_DLM 1
#define UART_IIR 2 /* read */
#define UART_FCR 2 /* write */
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_MSR 6
#define UART_SCR 7

/* LSR bits */
#define LSR_DR   0x01 /* data ready */
#define LSR_OE   0x02 /* overrun error */
#define LSR_THRE 0x20 /* transmit holding register empty */
#define LSR_TEMT 0x40 /* transmitter empty */

/* IER bits */
#define IER_RDA  0x01 /* received data available */
#define IER_THRE 0x02 /* transmitter holding register empty */

typedef struct uart {
    u8 ier, fcr, lcr, mcr, msr, scr, dll, dlm;

    /* Receive FIFO, a simple ring fed by the host (stdin or the soft keyboard) */
    u8 rx[UART_RX_SIZE];
    u32 rx_head;
    u32 rx_count;

    u32 irq;
    bool irq_level;

    /* Interrupt line -> PLIC */
    void (*raise)(void *ud, u32 irq, bool level);
    void *raise_ud;

    /* Character sink: stdout, a log file or the Android terminal view */
    void (*emit)(void *ud, u8 ch);
    void *emit_ud;

    u64 n_tx, n_rx;
} uart;

rvm_err uart_init(uart *u, u32 irq);
bool uart_load(void *dev, u64 off, u32 size, u64 *out);
bool uart_store(void *dev, u64 off, u32 size, u64 val);

/* Host -> guest.  Returns false if the FIFO is full (caller should retry). */
bool uart_push(uart *u, u8 ch);
/* Drain one byte the guest wrote; convenience for tests. */
void uart_set_sink(uart *u, void (*emit)(void *ud, u8 ch), void *ud);
void uart_set_irq(uart *u, void (*raise)(void *ud, u32 irq, bool level), void *ud);

#endif /* RVM_UART_H */
