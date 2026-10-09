/* SPDX-License-Identifier: MIT */
#include "uart.h"

rvm_err uart_init(uart *u, u32 irq) {
    if (!u)
        return RVM_ERR_BADARG;
    memset(u, 0, sizeof(*u));
    u->irq = irq;
    u->lcr = 0x03; /* 8N1, the sane default */
    u->dll = 0x0C; /* 9600 baud at a 1.8432 MHz reference */
    u->dlm = 0x00;
    return RVM_OK;
}

void uart_set_sink(uart *u, void (*emit)(void *ud, u8 ch), void *ud) {
    u->emit = emit;
    u->emit_ud = ud;
}

void uart_set_irq(uart *u, void (*raise)(void *ud, u32 irq, bool level), void *ud) {
    u->raise = raise;
    u->raise_ud = ud;
}

static inline bool dlab(const uart *u) {
    return (u->lcr & 0x80) != 0;
}

/* Interrupt Identification Register: bit0 = 1 means "nothing pending". */
static u8 uart_iir(const uart *u) {
    u8 v = 0x01;
    if ((u->ier & IER_RDA) && u->rx_count > 0)
        v = 0x04;
    else if (u->ier & IER_THRE)
        v = 0x02; /* THR is always empty: we flush synchronously */
    if (u->fcr & 0x01)
        v |= 0xC0; /* advertise 16550A FIFOs */
    return v;
}

static void uart_update_irq(uart *u) {
    bool level = (uart_iir(u) & 0x01) == 0;
    if (level != u->irq_level) {
        u->irq_level = level;
        if (u->raise)
            u->raise(u->raise_ud, u->irq, level);
    }
}

bool uart_push(uart *u, u8 ch) {
    if (u->rx_count >= UART_RX_SIZE)
        return false;
    u32 i = (u->rx_head + u->rx_count) % UART_RX_SIZE;
    u->rx[i] = ch;
    u->rx_count++;
    u->n_rx++;
    uart_update_irq(u);
    return true;
}

static u8 uart_pop(uart *u) {
    if (u->rx_count == 0)
        return 0;
    u8 ch = u->rx[u->rx_head];
    u->rx_head = (u->rx_head + 1) % UART_RX_SIZE;
    u->rx_count--;
    uart_update_irq(u);
    return ch;
}

bool uart_load(void *dev, u64 off, u32 size, u64 *out) {
    uart *u = (uart *)dev;
    if (size != 1)
        return false; /* 8250 registers are byte wide */
    *out = 0;

    switch (off & 7) {
    case UART_RBR:
        *out = dlab(u) ? u->dll : uart_pop(u);
        break;
    case UART_IER:
        *out = dlab(u) ? u->dlm : u->ier;
        break;
    case UART_IIR:
        *out = uart_iir(u);
        break;
    case UART_LCR:
        *out = u->lcr;
        break;
    case UART_MCR:
        *out = u->mcr;
        break;
    case UART_LSR:
        *out = LSR_THRE | LSR_TEMT | (u->rx_count ? LSR_DR : 0);
        break;
    case UART_MSR:
        *out = u->msr;
        break;
    case UART_SCR:
        *out = u->scr;
        break;
    }
    return true;
}

bool uart_store(void *dev, u64 off, u32 size, u64 val) {
    uart *u = (uart *)dev;
    if (size != 1)
        return false;
    u8 v = (u8)val;

    switch (off & 7) {
    case UART_THR:
        if (dlab(u)) {
            u->dll = v;
        } else {
            u->n_tx++;
            if (u->emit)
                u->emit(u->emit_ud, v);
            uart_update_irq(u);
        }
        break;
    case UART_IER:
        if (dlab(u))
            u->dlm = v;
        else {
            u->ier = v & 0x0F;
            uart_update_irq(u);
        }
        break;
    case UART_FCR:
        u->fcr = v;
        if (v & 0x02) { /* clear the receive FIFO */
            u->rx_head = 0;
            u->rx_count = 0;
        }
        uart_update_irq(u);
        break;
    case UART_LCR:
        u->lcr = v;
        break;
    case UART_MCR:
        u->mcr = v;
        break;
    case UART_LSR:
        break; /* read only */
    case UART_MSR:
        u->msr = v;
        break;
    case UART_SCR:
        u->scr = v;
        break;
    }
    return true;
}
