/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "ramjet.h"
#include "parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    idx = ramjet_hash(pt->name) % PROC_TYPE_MAP_SIZE;
    for (e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->type.name, pt->name) == 0) {
            rj_log(RJ_LOG_WARN,
                   "type '%s' defined more than once; the definition loaded last wins",
                   pt->name);
            e->type = *pt;
            return;
        }
    }
    e = malloc(sizeof(*e));
    if (!e) {
        rj_log(RJ_LOG_ERROR, "out of memory inserting proc type");
        return;
    }
    e->type = *pt;
    e->next = map->buckets[idx];
    map->buckets[idx] = e;
}

const ProcType *proc_type_map_get(const ProcTypeMap *map, const char *name) {
    if (!map || !name) return NULL;
    unsigned long idx = ramjet_hash(name) % PROC_TYPE_MAP_SIZE;
    for (const ProcTypeEntry *e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->type.name, name) == 0) return &e->type;
    }
    return NULL;
}

int proc_type_map_count(const ProcTypeMap *map) {
    int count = 0;
    if (!map) return 0;
    for (int i = 0; i < PROC_TYPE_MAP_SIZE; i++)
        for (const ProcTypeEntry *e = map->buckets[i]; e; e = e->next) count++;
    return count;
}

typedef struct { ProcTypeMap *map; } BuildTypesCtx;

static int parse_type_line(const char *json_line, void *user_data) {
    BuildTypesCtx *ctx = user_data;
    ProcType pt;
    char buf[32];
    long value;
    int found;

    memset(&pt, 0, sizeof(pt));

    found = json_get_string(json_line, "type", pt.name, sizeof(pt.name));
    if (found <= 0)
        found = json_get_string(json_line, "proc_type", pt.name, sizeof(pt.name));
    if (found <= 0 || !pt.name[0]) {
        rj_log(RJ_LOG_WARN, "type entry has no valid type name");
        return 0;
    }

    if (json_get_int(json_line, "nice", &value) == 1) {
        if (value < -20 || value > 19) {
            rj_log(RJ_LOG_WARN, "invalid nice %ld for type '%s': field ignored",
                   value, pt.name);
        } else {
            pt.nice = (int)value;
            pt.has_nice = 1;
        }
    }

    found = json_get_string(json_line, "ioclass", buf, sizeof(buf));
    if (found == 1) {
        if (io_class_from_string(buf, &pt.ioclass) == 0) {
            pt.has_ioclass = 1;
        } else {
            rj_log(RJ_LOG_WARN, "unknown ioclass '%s' in type '%s'", buf, pt.name);
        }
    }

    if (json_get_int(json_line, "ionice", &value) == 1) {
        if (value < 0 || value > 7) {
            rj_log(RJ_LOG_WARN, "invalid ionice %ld for type '%s': field ignored",
                   value, pt.name);
        } else {
            pt.ionice = (int)value;
            pt.has_ionice = 1;
        }
    }

    found = json_get_int(json_line, "oom_score_adj", &value) == 1;
    if (!found)
        found = json_get_int(json_line, "oom_scote_adj", &value) == 1; /* legacy typo key */
    if (found) {
        if (value < -1000 || value > 1000) {
            rj_log(RJ_LOG_WARN, "invalid oom_score_adj %ld for type '%s': field ignored",
                   value, pt.name);
        } else {
            pt.oom_score_adj = (int)value;
            pt.has_oom_score_adj = 1;
        }
    }

    proc_type_map_insert(ctx->map, &pt);
    return 0;
}

int build_types(ProcTypeMap *map, const char *config_dir) {
    proc_type_map_init(map);
    BuildTypesCtx ctx = { .map = map };
    return walk_config_dir(config_dir, "types", parse_type_line, &ctx);
}
