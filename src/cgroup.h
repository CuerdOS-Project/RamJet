/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#ifndef RAMJET_CGROUP_H
#define RAMJET_CGROUP_H

#include <sys/types.h>
#define CGROUP_NAME_MAX 256

typedef struct {
    char name[CGROUP_NAME_MAX];
    unsigned char cpu_quota;
} CgroupDef;

typedef struct CgroupEntry {
    CgroupDef def;
    int owns_fs;
    struct CgroupEntry *next;
} CgroupEntry;
#define CGROUP_MAP_SIZE 64

typedef struct { CgroupEntry *buckets[CGROUP_MAP_SIZE]; } CgroupMap;

void cgroup_map_init(CgroupMap *map);
void cgroup_map_free(CgroupMap *map);
int cgroup_map_insert(CgroupMap *map, const CgroupDef *def);
const CgroupDef *cgroup_map_get(const CgroupMap *map, const char *name);
int cgroup_apply(const CgroupDef *cg, pid_t pid);
int build_cgroups(CgroupMap *map);

#endif
