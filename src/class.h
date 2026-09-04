/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 *
 * I/O scheduling class helpers.
 */
#ifndef RAMJET_CLASS_H
#define RAMJET_CLASS_H

typedef enum {
    IO_CLASS_REALTIME = 1,
    IO_CLASS_BEST_EFFORT = 2,
    IO_CLASS_IDLE = 3,
} IoClass;

int io_class_from_string(const char *s, IoClass *out);
int io_class_to_num(IoClass c);
const char *io_class_to_string(IoClass c);

#endif
