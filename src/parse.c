/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 *
 * Line parser, deterministic directory walker and small flat-JSON accessor.
 */
#include "parse.h"
#include "log.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* djb2 hash shared by the rule and type maps. */
unsigned long ramjet_hash(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c;
    return h;
}

/* ------------------------------------------------------------------ */
/* Flat JSON accessors                                                 */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* "key = value" lines (ananicy.conf)                                  */
/* ------------------------------------------------------------------ */

int parse_kv_line(const char *line, char *key, size_t key_size,
                  char *value, size_t value_size) {
    const char *p, *eq, *ke, *v, *ve;
    size_t klen, vlen;

    if (!line || !key || !value || key_size == 0 || value_size == 0) return -1;
    p = skip_ws(line);
    if (!*p || *p == '#') return 0;

    eq = strchr(p, '=');
    if (!eq) return -1;

    ke = eq;
    while (ke > p && isspace((unsigned char)ke[-1])) ke--;
    klen = (size_t)(ke - p);
    if (klen == 0 || klen >= key_size) return -1;
    memcpy(key, p, klen);
    key[klen] = '\0';

    v = skip_ws(eq + 1);
    ve = v + strlen(v);
    while (ve > v && isspace((unsigned char)ve[-1])) ve--;
    vlen = (size_t)(ve - v);
    if (vlen == 0 || vlen >= value_size) return -1;
    memcpy(value, v, vlen);
    value[vlen] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* Line file parsing                                                   */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Deterministic directory walker.                                     */
/* nftw(3) traverses in readdir order, so "00-..." override files had  */
/* no guaranteed priority: files are collected and parsed in sorted    */
/* path order instead.                                                 */
/* ------------------------------------------------------------------ */

static int has_extension(const char *filename, const char *ext) {
    const char *dot;
    if (!filename || !ext) return 0;
    dot = strrchr(filename, '.');
    return dot && strcmp(dot + 1, ext) == 0;
}

typedef struct {
    char **items;
    size_t count, cap;
} PathList;

static void path_list_free(PathList *pl) {
    for (size_t i = 0; i < pl->count; i++) free(pl->items[i]);
    free(pl->items);
    pl->items = NULL;
    pl->count = pl->cap = 0;
}

static int path_list_add(PathList *pl, const char *path) {
    if (pl->count == pl->cap) {
        size_t ncap = pl->cap ? pl->cap * 2 : 64;
        char **nitems = realloc(pl->items, ncap * sizeof(*nitems));
        if (!nitems) return -1;
        pl->items = nitems;
        pl->cap = ncap;
    }
    char *dup = strdup(path);
    if (!dup) return -1;
    pl->items[pl->count++] = dup;
    return 0;
}

static int collect_files(const char *dir, const char *ext, int depth, PathList *pl) {
    char full[PATH_MAX + 256];
    DIR *d;
    struct dirent *ent;

    if (depth > 8) return 0; /* sanity limit for recursive configs */
    d = opendir(dir);
    if (!d) {
        rj_log(RJ_LOG_WARN, "failed to open %s: %s", dir, strerror(errno));
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        struct stat st;
        int rc, type = 0; /* 1 = dir, 2 = regular file */

        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        rc = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (rc < 0 || (size_t)rc >= sizeof(full)) continue;

        if (ent->d_type == DT_DIR) {
            type = 1;
        } else if (ent->d_type == DT_REG) {
            type = 2;
        } else if (ent->d_type == DT_UNKNOWN) {
            if (lstat(full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) type = 1;
            else if (S_ISREG(st.st_mode)) type = 2;
        }
        /* symlinks and special files are ignored (FTW_PHYS semantics) */

        if (type == 1) {
            collect_files(full, ext, depth + 1, pl);
        } else if (type == 2 && has_extension(ent->d_name, ext)) {
            if (path_list_add(pl, full) != 0)
                rj_log(RJ_LOG_WARN, "out of memory collecting %s", full);
        }
    }
    closedir(d);
    return 0;
}

static int cmp_path(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int walk_config_dir(const char *root_dir, const char *ext,
                    parse_line_cb cb, void *user_data) {
    PathList pl = { 0 };
    int had_error = 0;

    if (!root_dir || !ext || !cb) return -1;
    if (collect_files(root_dir, ext, 0, &pl) != 0) {
        path_list_free(&pl);
        return -1;
    }
    if (pl.count > 1)
        qsort(pl.items, pl.count, sizeof(char *), cmp_path);

    for (size_t i = 0; i < pl.count; i++) {
        FILE *fp = fopen(pl.items[i], "r");
        if (!fp) {
            rj_log(RJ_LOG_WARN, "failed to open %s: %s", pl.items[i], strerror(errno));
            had_error = 1;
            continue;
        }
        if (parse_file(fp, cb, user_data) != 0) {
            rj_log(RJ_LOG_WARN, "error parsing %s", pl.items[i]);
            had_error = 1;
        }
        fclose(fp);
    }
    path_list_free(&pl);
    return had_error ? -1 : 0;
}
