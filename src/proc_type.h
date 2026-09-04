/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#ifndef RAMJET_PROC_TYPE_H
#define RAMJET_PROC_TYPE_H

#include "class.h"
#define PROC_TYPE_NAME_MAX 256
#define PROC_TYPE_CGROUP_MAX 256

typedef struct {
    char name[PROC_TYPE_NAME_MAX];
    int nice;
    IoClass ioclass;
    int has_ioclass;
    int ionice;
    char cgroup[PROC_TYPE_CGROUP_MAX];
    int oom_score_adj;
    int has_oom_score_adj;
} ProcType;

#define PROC_TYPE_MAP_SIZE 512
typedef struct ProcTypeEntry {
    ProcType type;
    struct ProcTypeEntry *next;
} ProcTypeEntry;

typedef struct {
    ProcTypeEntry *buckets[PROC_TYPE_MAP_SIZE];
} ProcTypeMap;

void proc_type_map_init(ProcTypeMap *map);
void proc_type_map_free(ProcTypeMap *map);
void proc_type_map_insert(ProcTypeMap *map, const ProcType *pt);
const ProcType *proc_type_map_get(const ProcTypeMap *map, const char *name);
int build_types(ProcTypeMap *map);

#endif
