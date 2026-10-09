/* SPDX-License-Identifier: MIT */
#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

void ui_stdio_write(void *ud, const u8 *buf, size_t n) {
    RVM_UNUSED(ud);
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(STDOUT_FILENO, buf + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        done += (size_t)w;
    }
}

int ui_stdio_poll(void *ud, u8 *buf, size_t max) {
    RVM_UNUSED(ud);
    ssize_t r = read(STDIN_FILENO, buf, max);
    if (r < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return (int)r;
}

rvm_err ui_stdio_init(ui_stdio *u) {
    if (!u)
        return RVM_ERR_BADARG;
    memset(u, 0, sizeof(*u));
    if (!isatty(STDIN_FILENO)) {
        /* Piped or closed stdin: still usable, just not interactive. */
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl >= 0)
            fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
        u->ok = true;
        return RVM_OK;
    }
    struct termios *saved = (struct termios *)malloc(sizeof(struct termios));
    struct termios raw;
    if (!saved)
        return RVM_ERR_NOMEM;
    if (tcgetattr(STDIN_FILENO, saved) == 0) {
        raw = *saved;
        raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
            u->raw_mode = true;
    }
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl >= 0)
        fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    u->saved = saved;
    u->ok = true;
    return RVM_OK;
}

void ui_stdio_shutdown(ui_stdio *u) {
    if (!u)
        return;
    if (u->raw_mode && u->saved)
        tcsetattr(STDIN_FILENO, TCSANOW, (struct termios *)u->saved);
    free(u->saved);
    u->saved = NULL;
    u->raw_mode = false;
}

void ui_stdio_attach(vm_opts *o, ui_stdio *u) {
    RVM_UNUSED(u);
    o->write = ui_stdio_write;
    o->write_ud = NULL;
    o->poll = ui_stdio_poll;
    o->poll_ud = NULL;
}
