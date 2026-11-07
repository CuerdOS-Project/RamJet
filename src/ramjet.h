/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 *
 * Central configuration, PID cache and /proc helpers.
 */
#ifndef RAMJET_H
#define RAMJET_H

#include "rule.h"
#include "proc_type.h"
#include "log.h"
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef RAMJET_VERSION
#define RAMJET_VERSION "1.3.0"
#endif

/* Runtime configuration. Built from defaults + ananicy.conf + CLI flags. */
typedef struct RamjetConfig {
    char   config_dir[PATH_MAX];      /* where *.rules / *.types live          */
    char   conf_file[PATH_MAX];       /* ananicy.conf key=value file           */
    double check_freq;                /* seconds between scans                 */
    double cache_ttl;                 /* full re-apply period, 0 disables      */
    int    type_load;                 /* load .types files                     */
    int    rule_load;                 /* load .rules files                     */
    int    apply_nice;
    int    apply_ionice;
    int    apply_oom_score_adj;
    int    log_applied_rule;          /* log every applied rule (debug)        */
    int    loglevel;                  /* RJ_LOG_*                              */
    int    dry_run;                   /* log instead of applying               */
} RamjetConfig;

void config_defaults(RamjetConfig *cfg);
int  config_load(RamjetConfig *cfg);  /* defaults + ananicy.conf, CLI on top */

/*
 * PID cache: remembers which processes already carry the right priorities so
 * unchanged ones are skipped. Keyed by (pid, starttime); num_threads detects
 * newly spawned threads. Cleared every cache_ttl seconds (also removes dead
 * PIDs and corrects external priority drift).
 */
typedef struct ProcCacheEntry {
    pid_t pid;
    unsigned long long starttime;     /* /proc/pid/stat field 22 */
    int num_threads;                  /* /proc/pid/stat field 20 */
    struct ProcCacheEntry *next;
} ProcCacheEntry;

#define PROC_CACHE_BUCKETS 4096
typedef struct {
    ProcCacheEntry *buckets[PROC_CACHE_BUCKETS];
    time_t built_at;                  /* monotonic seconds of last rebuild */
} ProcCache;

void proc_cache_init(ProcCache *c);
void proc_cache_free(ProcCache *c);
void proc_cache_clear(ProcCache *c);  /* drop entries, keep built_at  */
void proc_cache_reset(ProcCache *c);  /* drop entries, zero built_at  */
ProcCacheEntry *proc_cache_get(const ProcCache *c, pid_t pid,
                               unsigned long long starttime);
void proc_cache_put(ProcCache *c, pid_t pid, unsigned long long starttime,
                    int num_threads);

/* /proc helpers (exposed for testing). */
int ramjet_proc_exe(pid_t pid, char *buf, size_t bufsz);
int ramjet_proc_stat(pid_t pid, unsigned long long *starttime, int *num_threads);

/* One full scan+apply pass. Returns number of real errors (ESRCH excluded). */
int apply_all_rules(const RuleMap *rules, const ProcTypeMap *types,
                    const RamjetConfig *cfg, ProcCache *cache);

#endif
