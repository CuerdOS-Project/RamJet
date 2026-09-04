/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#ifndef RAMJET_PARSE_H
#define RAMJET_PARSE_H

#include <stddef.h>
#include <stdio.h>

typedef int (*parse_line_cb)(const char *json_line, void *user_data);

int parse_file(FILE *fp, parse_line_cb cb, void *user_data);
int walk_config_dir(const char *root_dir, const char *ext,
                    parse_line_cb cb, void *user_data);

/* Minimal JSON accessors for the flat Ananicy objects used by RamJet. */
int json_get_string(const char *json, const char *key, char *out, size_t out_size);
int json_get_int(const char *json, const char *key, long *out);

#endif
