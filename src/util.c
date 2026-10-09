/* SPDX-License-Identifier: MIT */
#include "rvm.h"

#include <time.h>

/* ------------------------------------------------------------------ errors */

const char *rvm_strerror(rvm_err e) {
    switch (e) {
    case RVM_OK:
        return "ok";
    case RVM_ERR:
        return "generic failure";
    case RVM_ERR_NOMEM:
        return "out of memory";
    case RVM_ERR_IO:
        return "I/O error";
    case RVM_ERR_RANGE:
        return "address out of range";
    case RVM_ERR_BADARG:
        return "invalid argument";
    case RVM_ERR_UNSUPPORTED:
        return "unsupported operation";
    case RVM_ERR_NOTFOUND:
        return "not found";
    }
    return "unknown error";
}

/* ----------------------------------------------------------------- logging */

static rvm_loglevel g_level = RVM_LOG_INFO;
static rvm_log_fn g_log_fn;
static void *g_log_ud;

void rvm_log_set_sink(rvm_log_fn fn, void *ud) {
    g_log_fn = fn;
    g_log_ud = ud;
}
static rvm_trace_fn g_trace_fn;
static void *g_trace_ud;
static u64 g_trace_from;
static bool g_trace_armed;

void rvm_log_set_level(rvm_loglevel lvl) {
    g_level = lvl;
}
rvm_loglevel rvm_log_get_level(void) {
    return g_level;
}

static const char *level_name(rvm_loglevel l) {
    switch (l) {
    case RVM_LOG_TRACE:
        return "TRACE";
    case RVM_LOG_DEBUG:
        return "DEBUG";
    case RVM_LOG_INFO:
        return "INFO ";
    case RVM_LOG_WARN:
        return "WARN ";
    case RVM_LOG_ERROR:
        return "ERROR";
    default:
        return "?????";
    }
}

void rvm_vlog(rvm_loglevel lvl, const char *fmt, va_list ap) {
    if (lvl < g_level || fmt == NULL)
        return;
    char buf[512];
    if (vsnprintf(buf, sizeof buf, fmt, ap) < 0)
        return;
    if (g_log_fn) {
        g_log_fn(g_log_ud, lvl, buf);
        return;
    }
    FILE *out = (lvl >= RVM_LOG_WARN) ? stderr : stdout;
    fprintf(out, "[%s] %s\n", level_name(lvl), buf);
    fflush(out);
}

void rvm_log(rvm_loglevel lvl, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rvm_vlog(lvl, fmt, ap);
    va_end(ap);
}

void rvm_trace_set(rvm_trace_fn fn, void *ud) {
    g_trace_fn = fn;
    g_trace_ud = ud;
}

/* Kept out of the header so the CPU can reach it without exposing globals. */
rvm_trace_fn rvm_trace_get(void **ud) {
    if (ud)
        *ud = g_trace_ud;
    return g_trace_fn;
}

void rvm_trace_from(u64 pc) {
    g_trace_from = pc;
    g_trace_armed = (pc == 0); /* no gate: emit from the first instruction */
}

bool rvm_trace_armed(u64 pc) {
    if (g_trace_armed)
        return true;
    if (g_trace_from != 0 && pc == g_trace_from) {
        g_trace_armed = true;
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------- time */

u64 rvm_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
