/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#ifndef RAMJET_RULE_H
#define RAMJET_RULE_H

#include "class.h"
#include "proc_type.h"
#include <sys/types.h>

#define RULE_NAME_MAX 256
#define RULE_CGROUP_MAX 256

typedef struct {
    char name[RULE_NAME_MAX];
    char proc_type_name[PROC_TYPE_NAME_MAX];
    int nice;
    IoClass ioclass;
    int has_ioclass;
    int ionice;
    char cgroup[RULE_CGROUP_MAX];
    int oom_score_adj;
    int has_oom_score_adj;
} Rule;

#define RULE_MAP_SIZE 1024
typedef struct RuleEntry {
    Rule rule;
    struct RuleEntry *next;
} RuleEntry;

typedef struct {
    RuleEntry *buckets[RULE_MAP_SIZE];
} RuleMap;

void rule_map_init(RuleMap *map);
void rule_map_free(RuleMap *map);
void rule_map_insert(RuleMap *map, const Rule *r);
const Rule *rule_map_get(const RuleMap *map, const char *name);
int build_rules(RuleMap *map, const ProcTypeMap *types);

const char *rule_effective_cgroup(const Rule *r, const ProcTypeMap *types);
int rule_effective_nice(const Rule *r, const ProcTypeMap *types);
int rule_effective_ioclass(const Rule *r, const ProcTypeMap *types, IoClass *out);
int rule_effective_ionice(const Rule *r, const ProcTypeMap *types);
int rule_effective_oom_score_adj(const Rule *r, const ProcTypeMap *types, int *out);
int rule_apply(const Rule *r, const ProcTypeMap *types, pid_t pid);

#endif
