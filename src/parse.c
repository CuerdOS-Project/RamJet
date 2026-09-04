/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 *
 * Line parser, directory walker and small flat-JSON accessor.
 */
#include "parse.h"
#include <ctype.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local struct {
    const char *ext;
    parse_line_cb cb;
    void *user_data;
    int had_error;
} walk_ctx;

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *skip_string(const char *p) {
    if (*p != '"') return NULL;
    p++;
    while (*p) {
        if (*p == '\\') {
            if (!p[1]) return NULL;
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

static int decode_string(const char *p, char *out, size_t out_size, const char **end) {
    size_t n = 0;
    if (!p || *p != '"' || !out || out_size == 0) return -1;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            if (!*p) return -1;
            switch (*p++) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: return -1; /* Unicode escapes are unnecessary for these keys/values. */
            }
        }
        if (n + 1 >= out_size) return -2;
        out[n++] = c;
    }
    if (*p != '"') return -1;
    out[n] = '\0';
    if (end) *end = p + 1;
    return 0;
}

static const char *find_key_value(const char *json, const char *key) {
    const char *p = skip_ws(json);
    char candidate[256];
    if (!p || *p != '{') return NULL;
    p++;

    while (1) {
        p = skip_ws(p);
        if (!*p || *p == '}') return NULL;
        if (*p != '"') return NULL;
        const char *after_key = NULL;
        if (decode_string(p, candidate, sizeof(candidate), &after_key) != 0) return NULL;
        p = skip_ws(after_key);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);
        if (strcmp(candidate, key) == 0) return p;

        if (*p == '"') {
            p = skip_string(p);
            if (!p) return NULL;
        } else {
            int depth = 0;
            int in_string = 0;
            for (; *p; p++) {
                if (in_string) {
                    if (*p == '\\') {
                        if (!p[1]) return NULL;
                        p++;
                    } else if (*p == '"') {
                        in_string = 0;
                    }
                } else if (*p == '"') {
                    in_string = 1;
                } else if (*p == '{' || *p == '[') {
                    depth++;
                } else if (*p == '}' || *p == ']') {
                    if (depth == 0) break;
                    depth--;
                } else if (*p == ',' && depth == 0) {
                    break;
                }
            }
        }
        p = skip_ws(p);
        if (*p != ',') return NULL;
        p++;
    }
}

int json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return -1;
    const char *value = find_key_value(json, key);
    if (!value) return 0;
    if (*value != '"') return -1;
    return decode_string(value, out, out_size, NULL) == 0 ? 1 : -1;
}

int json_get_int(const char *json, const char *key, long *out) {
    if (!json || !key || !out) return -1;
    const char *value = find_key_value(json, key);
    char *end = NULL;
    long v;
    if (!value) return 0;
    errno = 0;
    v = strtol(value, &end, 10);
    if (value == end || errno == ERANGE) return -1;
    end = (char *)skip_ws(end);
    if (*end != ',' && *end != '}' && *end != '\0') return -1;
    *out = v;
    return 1;
}

int parse_file(FILE *fp, parse_line_cb cb, void *user_data) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int ret = 0;

    if (!fp || !cb) return -1;
    while ((len = getline(&line, &cap, fp)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        char *start = line;
        while (*start && isspace((unsigned char)*start)) start++;
        if (*start == '\0' || *start == '#') continue;
        if (cb(start, user_data) != 0) {
            ret = -1;
            break;
        }
    }
    if (ferror(fp)) ret = -1;
    free(line);
    return ret;
}

static int has_extension(const char *filename, const char *ext) {
    const char *dot;
    if (!filename || !ext) return 0;
    dot = strrchr(filename, '.');
    return dot && strcmp(dot + 1, ext) == 0;
}

static int nftw_callback(const char *fpath, const struct stat *sb,
                         int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)ftwbuf;
    if (typeflag != FTW_F) return 0;

    const char *base = strrchr(fpath, '/');
    base = base ? base + 1 : fpath;
    if (!has_extension(base, walk_ctx.ext)) return 0;

    FILE *fp = fopen(fpath, "r");
    if (!fp) {
        fprintf(stderr, "[ramjet] warn: failed to open %s: %s\n", fpath, strerror(errno));
        walk_ctx.had_error = 1;
        return 0;
    }
    if (parse_file(fp, walk_ctx.cb, walk_ctx.user_data) != 0) {
        fprintf(stderr, "[ramjet] warn: error parsing %s\n", fpath);
        walk_ctx.had_error = 1;
    }
    fclose(fp);
    return 0;
}

int walk_config_dir(const char *root_dir, const char *ext,
                    parse_line_cb cb, void *user_data) {
    if (!root_dir || !ext || !cb) return -1;
    walk_ctx.ext = ext;
    walk_ctx.cb = cb;
    walk_ctx.user_data = user_data;
    walk_ctx.had_error = 0;

    errno = 0;
    if (nftw(root_dir, nftw_callback, 16, FTW_PHYS) != 0) {
        fprintf(stderr, "[ramjet] warn: failed to walk %s: %s\n",
                root_dir, strerror(errno));
        return -1;
    }
    return walk_ctx.had_error ? -1 : 0;
}
