/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 *
 * Optimized scan loop: PID cache, single rule resolution per process,
 * "(deleted)" executable handling and raw oom_score_adj writes.
 */
#include "ramjet.h"
#include "log.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int parse_pid(const char *s, pid_t *out) {
    char *end = NULL;
    long value;
    if (!s || !*s || !out) return -1;
    errno = 0;
    value = strtol(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || value <= 0 || value > INT_MAX) return -1;
    *out = (pid_t)value;
    return 0;
}

/* ------------------------------------------------------------------ */
/* /proc helpers                                                       */
/* ------------------------------------------------------------------ */

int ramjet_proc_exe(pid_t pid, char *buf, size_t bufsz) {
    static const char deleted_suffix[] = " (deleted)";
    const size_t suffix_len = sizeof(deleted_suffix) - 1;
    char link_path[64], target[PATH_MAX];
    char *base;
    size_t base_len;
    ssize_t len;

    if (!buf || bufsz < 2) return -1;
    snprintf(link_path, sizeof(link_path), "/proc/%d/exe", pid);
    len = readlink(link_path, target, sizeof(target) - 1);
    if (len < 0) return -1;
    target[len] = '\0';
    base = strrchr(target, '/');
    base = base ? base + 1 : target;

    /* The kernel appends " (deleted)" when the binary was replaced or
     * removed (auto-updating browsers, AppImages...). Rules match the
     * original name, so strip the suffix. */
    base_len = strlen(base);
    if (base_len > suffix_len &&
        strcmp(base + base_len - suffix_len, deleted_suffix) == 0) {
        base_len -= suffix_len;
        base[base_len] = '\0';
    }

    if (!*base) return -1;
    if (base_len >= bufsz) return -1;
    memcpy(buf, base, base_len + 1);
    return 0;
}

int ramjet_proc_stat(pid_t pid, unsigned long long *starttime, int *num_threads) {
    char path[64], buf[4096];
    char *p, *tok;
    ssize_t n;
    int fd, i;
    unsigned long long st = 0;
    int nth = 0;

    if (!starttime || !num_threads) return -1;
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    /* comm may contain spaces and parentheses: skip past the last ')'.
     * Tokens after it are fields 3.. ; num_threads is field 20 (token 17)
     * and starttime is field 22 (token 19). */
    p = strrchr(buf, ')');
    if (!p) return -1;
    p++;
    for (i = 0; i < 20; i++) {
        while (*p == ' ') p++;
        if (!*p) return -1;
        tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        if (i == 17) {
            nth = atoi(tok);
        } else if (i == 19) {
            char *end = NULL;
            st = strtoull(tok, &end, 10);
            if (!end || end == tok) return -1;
            break;
        }
    }
    if (st == 0) return -1;
    *starttime = st;
    *num_threads = nth;
    return 0;
}

/* ------------------------------------------------------------------ */
/* PID cache                                                           */
/* ------------------------------------------------------------------ */

static unsigned cache_bucket(pid_t pid) {
    return (unsigned)(((unsigned long)pid * 2654435761UL) &
                      (PROC_CACHE_BUCKETS - 1));
}

void proc_cache_init(ProcCache *c) {
    memset(c, 0, sizeof(*c));
}

static void cache_drop_entries(ProcCache *c) {
    for (int i = 0; i < PROC_CACHE_BUCKETS; i++) {
        ProcCacheEntry *e = c->buckets[i];
        while (e) {
            ProcCacheEntry *next = e->next;
            free(e);
            e = next;
        }
        c->buckets[i] = NULL;
    }
}

void proc_cache_clear(ProcCache *c) {
    if (!c) return;
    cache_drop_entries(c);
}

void proc_cache_reset(ProcCache *c) {
    if (!c) return;
    cache_drop_entries(c);
    c->built_at = 0;
}

void proc_cache_free(ProcCache *c) {
    if (!c) return;
    cache_drop_entries(c);
}

ProcCacheEntry *proc_cache_get(const ProcCache *c, pid_t pid,
                               unsigned long long starttime) {
    if (!c) return NULL;
    for (const ProcCacheEntry *e = c->buckets[cache_bucket(pid)]; e; e = e->next)
        if (e->pid == pid && e->starttime == starttime) return (ProcCacheEntry *)e;
    return NULL;
}

void proc_cache_put(ProcCache *c, pid_t pid, unsigned long long starttime,
                    int num_threads) {
    if (!c) return;
    unsigned b = cache_bucket(pid);
    ProcCacheEntry **pp = &c->buckets[b];

    /* Drop stale entries for the same PID (e.g. PID reuse). */
    while (*pp) {
        if ((*pp)->pid == pid) {
            ProcCacheEntry *dead = *pp;
            *pp = dead->next;
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }

    ProcCacheEntry *e = malloc(sizeof(*e));
    if (!e) return;
    e->pid = pid;
    e->starttime = starttime;
    e->num_threads = num_threads;
    e->next = c->buckets[b];
    c->buckets[b] = e;
}

/* ------------------------------------------------------------------ */
/* Scan and apply                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const RuleMap *rules;
    const ProcTypeMap *types;
    const RamjetConfig *cfg;
    ProcCache *cache;
    int errors;
} ApplyContext;

/* Apply the already-resolved rule to every non-leader thread. The rule
 * lookup is done once per process instead of once per thread. */
static void apply_to_threads(pid_t tgid, const Rule *rule, ApplyContext *ctx) {
    char task_dir[64];
    DIR *d;
    struct dirent *ent;

    snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", tgid);
    d = opendir(task_dir);
    if (!d) return; /* process may have exited */

    while ((ent = readdir(d)) != NULL) {
        pid_t tid;
        if (parse_pid(ent->d_name, &tid) != 0 || tid == tgid) continue;
        if (rule_apply(rule, ctx->types, ctx->cfg, tid, 0) < 0) ctx->errors++;
    }
    closedir(d);
}

static int apply_leader(pid_t pid, const char *exe_name, void *user_data) {
    ApplyContext *ctx = user_data;
    const Rule *r = rule_map_get(ctx->rules, exe_name);
    unsigned long long starttime = 0;
    int nthreads = 0;
    int have_stat = 0;

    if (!r) return 0;

    if (ctx->cache) {
        if (ramjet_proc_stat(pid, &starttime, &nthreads) == 0) {
            ProcCacheEntry *ce;
            have_stat = 1;
            ce = proc_cache_get(ctx->cache, pid, starttime);
            if (ce && nthreads <= ce->num_threads) {
                /* Same process, no new threads: priorities are already set.
                 * (TIDs are monotonic, so a growing count means new threads.) */
                rj_log(RJ_LOG_DEBUG, "pid %d ('%s') unchanged, skipping", (int)pid, exe_name);
                return 0;
            }
        }
    }

    if (rule_apply(r, ctx->types, ctx->cfg, pid, 1) < 0) ctx->errors++;
    apply_to_threads(pid, r, ctx);

    if (ctx->cache && have_stat)
        proc_cache_put(ctx->cache, pid, starttime, nthreads);
    return 0;
}

typedef int (*proc_callback)(pid_t pid, const char *exe_name, void *user_data);

static int iterate_procs(proc_callback cb, void *user_data) {
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        rj_log(RJ_LOG_ERROR, "failed to open /proc: %s", strerror(errno));
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(proc_dir)) != NULL) {
        pid_t pid;
        char exe_name[256];
        if (parse_pid(ent->d_name, &pid) != 0) continue;
        if (ramjet_proc_exe(pid, exe_name, sizeof(exe_name)) != 0) continue;
        if (cb(pid, exe_name, user_data) < 0) {
            closedir(proc_dir);
            return -1;
        }
    }
    closedir(proc_dir);
    return 0;
}

static time_t monotonic_secs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return time(NULL);
    return (time_t)ts.tv_sec;
}

int apply_all_rules(const RuleMap *rules, const ProcTypeMap *types,
                    const RamjetConfig *cfg, ProcCache *cache) {
    ApplyContext ctx;
    ProcCache *active = NULL;

    if (!rules || !types || !cfg) return -1;

    if (cfg->cache_ttl > 0 && !cfg->dry_run && cache) {
        time_t now = monotonic_secs();
        if (cache->built_at == 0 ||
            (time_t)(now - cache->built_at) >= (time_t)cfg->cache_ttl) {
            /* Periodic rebuild: drops dead PIDs and re-applies everything,
             * correcting any external priority drift within cache_ttl. */
            proc_cache_clear(cache);
            cache->built_at = now;
        }
        active = cache;
    }

    ctx.rules = rules;
    ctx.types = types;
    ctx.cfg = cfg;
    ctx.cache = active;
    ctx.errors = 0;

    if (iterate_procs(apply_leader, &ctx) != 0) return ctx.errors + 1;
    return ctx.errors;
}
