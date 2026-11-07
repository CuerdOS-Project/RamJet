/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#ifndef RAMJET_PARSE_H
#define RAMJET_PARSE_H

#include <stddef.h>
#include <stdio.h>

typedef int (*parse_line_cb)(const char *line, void *user_data);

int parse_file(FILE *fp, parse_line_cb cb, void *user_data);

/* Collects matching files below root_dir (sorted, symlinks not followed). */
int walk_config_dir(const char *root_dir, const char *ext,
                    parse_line_cb cb, void *user_data);

/* Parses "key = value" lines (ananicy.conf). Returns 1 when a pair was
 * parsed, 0 for blank/comment lines, -1 for malformed lines. */
int parse_kv_line(const char *line, char *key, size_t key_size,
                  char *value, size_t value_size);

/* Minimal JSON accessors for the flat Ananicy objects used by RamJet. */
int json_get_string(const char *json, const char *key, char *out, size_t out_size);
int json_get_int(const char *json, const char *key, long *out);

/* djb2 hash shared by the rule/type maps and the warn-once table. */
unsigned long ramjet_hash(const char *s);

#endif
