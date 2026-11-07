/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 *
 * Minimal stderr logger with levels.
 */
#ifndef RAMJET_LOG_H
#define RAMJET_LOG_H

enum {
    RJ_LOG_DEBUG = 0,
    RJ_LOG_INFO  = 1,
    RJ_LOG_WARN  = 2,
    RJ_LOG_ERROR = 3
};

void rj_set_loglevel(int level);
int  rj_loglevel(void);
void rj_log(int level, const char *fmt, ...);

#endif
