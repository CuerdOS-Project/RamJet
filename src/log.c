/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "log.h"
#include <stdarg.h>
#include <stdio.h>

static int g_level = RJ_LOG_INFO;

void rj_set_loglevel(int level) {
    if (level >= RJ_LOG_DEBUG && level <= RJ_LOG_ERROR) g_level = level;
}

int rj_loglevel(void) {
    return g_level;
}

void rj_log(int level, const char *fmt, ...) {
    static const char *names[4] = { "debug", "info", "warn", "error" };
    va_list ap;

    if (level < g_level) return;
    if (level < RJ_LOG_DEBUG) level = RJ_LOG_DEBUG;
    if (level > RJ_LOG_ERROR) level = RJ_LOG_ERROR;
    fprintf(stderr, "[ramjet] %s: ", names[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
