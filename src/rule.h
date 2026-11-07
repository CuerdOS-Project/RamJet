/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#ifndef RAMJET_RULE_H
#define RAMJET_RULE_H

#include "class.h"
#include "proc_type.h"
#include <sys/types.h>

/* Forward declaration: full type lives in ramjet.h. */
typedef struct RamjetConfig RamjetConfig;

#define RULE_NAME_MAX 256

/*
 * A rule maps an executable name to scheduling parameters. Presence of each
 * parameter is tracked with its own has_* flag: -1 used to double as "unset",
 * which silently dropped legal values such as nice = -1.
 */
typedef struct {
    char name[RULE_NAME_MAX];
    char proc_type_name[PROC_TYPE_NAME_MAX];
    int nice;
    int has_nice;
    IoClass ioclass;
    int has_ioclass;
    int ionice;
    int has_ionice;
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
int rule_map_count(const RuleMap *map);
typedef void (*rule_map_iter_cb)(const Rule *r, void *user_data);
void rule_map_foreach(const RuleMap *map, rule_map_iter_cb cb, void *user_data);
int build_rules(RuleMap *map, const char *config_dir);

/* Effective values: rule field first, inherited type field second. */
int rule_effective_nice(const Rule *r, const ProcTypeMap *types, int *out);
int rule_effective_ioclass(const Rule *r, const ProcTypeMap *types, IoClass *out);
int rule_effective_ionice(const Rule *r, const ProcTypeMap *types, int *out);
int rule_effective_oom_score_adj(const Rule *r, const ProcTypeMap *types, int *out);

/*
 * Apply one rule to one task. oom_score_adj is process-wide and is only
 * applied for the thread-group leader. Returns 0 on success (a vanished
 * process is NOT an error), -1 on real errors.
 */
int rule_apply(const Rule *r, const ProcTypeMap *types, const RamjetConfig *cfg,
               pid_t pid, int is_process_leader);

#endif
