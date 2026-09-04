/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "proc_type.h"
#include "parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANANICY_CONFIG_DIR "/etc/ananicy.d"

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c;
    return h;
}

void proc_type_map_init(ProcTypeMap *map) {
    memset(map, 0, sizeof(*map));
}

void proc_type_map_free(ProcTypeMap *map) {
    for (int i = 0; i < PROC_TYPE_MAP_SIZE; i++) {
        ProcTypeEntry *e = map->buckets[i];
        while (e) {
            ProcTypeEntry *next = e->next;
            free(e);
            e = next;
        }
        map->buckets[i] = NULL;
    }
}

void proc_type_map_insert(ProcTypeMap *map, const ProcType *pt) {
    unsigned long idx;
    ProcTypeEntry *e;
    if (!map || !pt || !pt->name[0]) return;
    idx = hash_str(pt->name) % PROC_TYPE_MAP_SIZE;
    for (e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->type.name, pt->name) == 0) {
            e->type = *pt;
            return;
        }
    }
    e = malloc(sizeof(*e));
    if (!e) {
        fprintf(stderr, "[ramjet] error: out of memory inserting proc type\n");
        return;
    }
    e->type = *pt;
    e->next = map->buckets[idx];
    map->buckets[idx] = e;
}

const ProcType *proc_type_map_get(const ProcTypeMap *map, const char *name) {
    if (!map || !name) return NULL;
    unsigned long idx = hash_str(name) % PROC_TYPE_MAP_SIZE;
    for (ProcTypeEntry *e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->type.name, name) == 0) return &e->type;
    }
    return NULL;
}

typedef struct { ProcTypeMap *map; } BuildTypesCtx;

static int parse_type_line(const char *json_line, void *user_data) {
    BuildTypesCtx *ctx = user_data;
    ProcType pt;
    char buf[PROC_TYPE_CGROUP_MAX];
    long value;
    int found;

    memset(&pt, 0, sizeof(pt));
    pt.nice = -1;
    pt.ionice = -1;

    found = json_get_string(json_line, "type", pt.name, sizeof(pt.name));
    if (found <= 0) {
        found = json_get_string(json_line, "proc_type", pt.name, sizeof(pt.name));
    }
    if (found <= 0 || !pt.name[0]) {
        fprintf(stderr, "[ramjet] warn: type entry has no valid type name\n");
        return 0;
    }

    if (json_get_int(json_line, "nice", &value) == 1) {
        if (value < -20 || value > 19) {
            fprintf(stderr, "[ramjet] warn: invalid nice value %ld for type %s\n", value, pt.name);
            return 0;
        }
        pt.nice = (int)value;
    }

    found = json_get_string(json_line, "ioclass", buf, sizeof(buf));
    if (found == 1) {
        if (io_class_from_string(buf, &pt.ioclass) == 0) {
            pt.has_ioclass = 1;
        } else {
            fprintf(stderr, "[ramjet] warn: unknown ioclass '%s' in type '%s'\n", buf, pt.name);
        }
    }

    if (json_get_int(json_line, "ionice", &value) == 1) {
        if (value < 0 || value > 7) {
            fprintf(stderr, "[ramjet] warn: invalid ionice value %ld for type %s\n", value, pt.name);
            return 0;
        }
        pt.ionice = (int)value;
    }

    found = json_get_string(json_line, "cgroup", buf, sizeof(buf));
    if (found == 1) snprintf(pt.cgroup, sizeof(pt.cgroup), "%s", buf);

    if (json_get_int(json_line, "oom_score_adj", &value) != 1) {
        json_get_int(json_line, "oom_scote_adj", &value); /* Backward-compatible typo. */
    } else {
        pt.has_oom_score_adj = 1;
    }
    if (pt.has_oom_score_adj && (value < -1000 || value > 1000)) {
        fprintf(stderr, "[ramjet] warn: invalid oom_score_adj %ld for type %s\n", value, pt.name);
        return 0;
    }
    if (!pt.has_oom_score_adj) {
        long legacy;
        if (json_get_int(json_line, "oom_scote_adj", &legacy) == 1 && legacy >= -1000 && legacy <= 1000) {
            pt.oom_score_adj = (int)legacy;
            pt.has_oom_score_adj = 1;
        }
    } else {
        pt.oom_score_adj = (int)value;
    }

    proc_type_map_insert(ctx->map, &pt);
    return 0;
}

int build_types(ProcTypeMap *map) {
    proc_type_map_init(map);
    BuildTypesCtx ctx = { .map = map };
    return walk_config_dir(ANANICY_CONFIG_DIR, "types", parse_type_line, &ctx);
}
