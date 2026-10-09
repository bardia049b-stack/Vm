/*
 * ui.h -- host console plumbing.
 *
 * The VM core never touches stdio directly; it is handed a write callback and
 * a poll callback.  On the desktop those are implemented here (stdout plus
 * non-blocking stdin).  On Android the same two callbacks are implemented in
 * JNI against a Canvas terminal view, which is why android/jni/rvm_jni.c only
 * has to fill in this tiny struct.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_UI_H
#define RVM_UI_H

#include "../vm/vm.h"

typedef struct ui_stdio {
    bool raw_mode;   /* terminal put into raw mode for interactive use */
    bool ok;
    void *saved;     /* opaque termios backup */
} ui_stdio;

/* Enter raw, non-blocking mode so keystrokes reach the guest one at a time. */
rvm_err ui_stdio_init(ui_stdio *u);
void ui_stdio_shutdown(ui_stdio *u);

/* Fill vm_opts with the stdio callbacks. */
void ui_stdio_attach(vm_opts *o, ui_stdio *u);

/* vm_poll_fn implementation: reads whatever stdin has buffered, non-blocking. */
int ui_stdio_poll(void *ud, u8 *buf, size_t max);
void ui_stdio_write(void *ud, const u8 *buf, size_t n);

#endif /* RVM_UI_H */
