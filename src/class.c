/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "class.h"
#include <strings.h>

int io_class_from_string(const char *s, IoClass *out) {
    if (!s || !out) return -1;
    if (strcasecmp(s, "idle") == 0) {
        *out = IO_CLASS_IDLE;
        return 0;
    }
    if (strcasecmp(s, "best-effort") == 0 || strcasecmp(s, "best_effort") == 0) {
        *out = IO_CLASS_BEST_EFFORT;
        return 0;
    }
    if (strcasecmp(s, "realtime") == 0 || strcasecmp(s, "real-time") == 0) {
        *out = IO_CLASS_REALTIME;
        return 0;
    }
    return -1;
}

int io_class_to_num(IoClass c) {
    switch (c) {
        case IO_CLASS_REALTIME: return 1;
        case IO_CLASS_BEST_EFFORT: return 2;
        case IO_CLASS_IDLE: return 3;
        default: return -1;
    }
}

const char *io_class_to_string(IoClass c) {
    switch (c) {
        case IO_CLASS_REALTIME:    return "realtime";
        case IO_CLASS_BEST_EFFORT: return "best-effort";
        case IO_CLASS_IDLE:        return "idle";
        default:                   return "unknown";
    }
}
