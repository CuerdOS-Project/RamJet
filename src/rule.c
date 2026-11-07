/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ramjet.h"
#include "parse.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <linux/ioprio.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Rule map                                                            */
/* ------------------------------------------------------------------ */

void rule_map_init(RuleMap *map) { memset(map, 0, sizeof(*map)); }

void rule_map_free(RuleMap *map) {
    for (int i = 0; i < RULE_MAP_SIZE; i++) {
        RuleEntry *e = map->buckets[i];
        while (e) {
            RuleEntry *next = e->next;
            free(e);
            e = next;
        }
        map->buckets[i] = NULL;
    }
}

void rule_map_insert(RuleMap *map, const Rule *r) {
    unsigned long idx;
    if (!map || !r || !r->name[0]) return;
    idx = ramjet_hash(r->name) % RULE_MAP_SIZE;
    for (RuleEntry *e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->rule.name, r->name) == 0) {
            rj_log(RJ_LOG_WARN,
                   "rule '%s' defined more than once; the definition loaded last wins",
                   r->name);
            e->rule = *r;
            return;
        }
    }
    RuleEntry *entry = malloc(sizeof(*entry));
    if (!entry) {
        rj_log(RJ_LOG_ERROR, "out of memory inserting rule");
        return;
    }
    entry->rule = *r;
    entry->next = map->buckets[idx];
    map->buckets[idx] = entry;
}

const Rule *rule_map_get(const RuleMap *map, const char *name) {
    if (!map || !name) return NULL;
    unsigned long idx = ramjet_hash(name) % RULE_MAP_SIZE;
    for (const RuleEntry *e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->rule.name, name) == 0) return &e->rule;
    }
    return NULL;
}

int rule_map_count(const RuleMap *map) {
    int count = 0;
    if (!map) return 0;
    for (int i = 0; i < RULE_MAP_SIZE; i++)
        for (const RuleEntry *e = map->buckets[i]; e; e = e->next) count++;
    return count;
}

void rule_map_foreach(const RuleMap *map, rule_map_iter_cb cb, void *user_data) {
    if (!map || !cb) return;
    for (int i = 0; i < RULE_MAP_SIZE; i++)
        for (const RuleEntry *e = map->buckets[i]; e; e = e->next)
            cb(&e->rule, user_data);
}

/* ------------------------------------------------------------------ */
/* Effective values (rule field, then inherited type field)            */
/* ------------------------------------------------------------------ */

static const ProcType *rule_type(const Rule *r, const ProcTypeMap *types) {
    return (r && types && r->proc_type_name[0]) ?
        proc_type_map_get(types, r->proc_type_name) : NULL;
}

int rule_effective_nice(const Rule *r, const ProcTypeMap *types, int *out) {
    const ProcType *pt;
    if (!r || !out) return -1;
    if (r->has_nice) { *out = r->nice; return 0; }
    pt = rule_type(r, types);
    if (pt && pt->has_nice) { *out = pt->nice; return 0; }
    return -1;
}

int rule_effective_ioclass(const Rule *r, const ProcTypeMap *types, IoClass *out) {
    const ProcType *pt;
    if (!r || !out) return -1;
    if (r->has_ioclass) { *out = r->ioclass; return 0; }
    pt = rule_type(r, types);
    if (pt && pt->has_ioclass) { *out = pt->ioclass; return 0; }
    return -1;
}

int rule_effective_ionice(const Rule *r, const ProcTypeMap *types, int *out) {
    const ProcType *pt;
    if (!r || !out) return -1;
    if (r->has_ionice) { *out = r->ionice; return 0; }
    pt = rule_type(r, types);
    if (pt && pt->has_ionice) { *out = pt->ionice; return 0; }
    return -1;
}

int rule_effective_oom_score_adj(const Rule *r, const ProcTypeMap *types, int *out) {
    const ProcType *pt;
    if (!r || !out) return -1;
    if (r->has_oom_score_adj) { *out = r->oom_score_adj; return 0; }
    pt = rule_type(r, types);
    if (pt && pt->has_oom_score_adj) { *out = pt->oom_score_adj; return 0; }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Warn-once table: avoids log spam for persistent errors (e.g. EPERM  */
/* without CAP_SYS_NICE). Entries live until exit; they are bounded by */
/* the number of distinct (rule, operation, errno) combinations.       */
/* ------------------------------------------------------------------ */

#define WARN_ONCE_BUCKETS 257
typedef struct WarnOnceEntry {
    char key[352];
    struct WarnOnceEntry *next;
} WarnOnceEntry;
static WarnOnceEntry *warn_once_table[WARN_ONCE_BUCKETS];

static int should_warn_once(const char *rule_name, const char *what, int err) {
    char key[sizeof(((WarnOnceEntry *)0)->key)];
    WarnOnceEntry *e;
    unsigned long idx;

    snprintf(key, sizeof(key), "%s|%s|%d", rule_name, what, err);
    idx = ramjet_hash(key) % WARN_ONCE_BUCKETS;
    for (e = warn_once_table[idx]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return 0;
    e = malloc(sizeof(*e));
    if (!e) return 1;
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->next = warn_once_table[idx];
    warn_once_table[idx] = e;
    return 1;
}

static void apply_warn(const char *rule_name, const char *what, pid_t pid) {
    if (!should_warn_once(rule_name, what, errno)) return;
    rj_log(RJ_LOG_WARN, "%s for rule '%s' (pid %d) failed: %s [further occurrences suppressed]",
           what, rule_name, (int)pid, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* Appliers. Convention: 0 = applied, 1 = process gone (not an error), */
/* -1 = real error (counted and warn-once'd).                          */
/* ------------------------------------------------------------------ */

static int apply_nice(int nice_val, pid_t pid, const char *rule_name) {
    if (setpriority(PRIO_PROCESS, (id_t)pid, nice_val) == 0) return 0;
    if (errno == ESRCH) return 1;
    apply_warn(rule_name, "setpriority", pid);
    return -1;
}

static int apply_oom_score_adj(int value, pid_t pid, const char *rule_name) {
    char path[64], text[32];
    int fd, len, rc = 0;

    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
    len = snprintf(text, sizeof(text), "%d", value);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (errno == ENOENT || errno == ESRCH) return 1;
        apply_warn(rule_name, "open oom_score_adj", pid);
        return -1;
    }
    if (write(fd, text, (size_t)len) != (ssize_t)len) {
        apply_warn(rule_name, "write oom_score_adj", pid);
        rc = -1;
    }
    close(fd);
    return rc;
}

static int apply_ionice(IoClass ioclass, int ionice_val, pid_t pid,
                        const char *rule_name) {
    int class_num = io_class_to_num(ioclass);
    int data;
    int priority;

    switch (class_num) {
        case IOPRIO_CLASS_RT:
        case IOPRIO_CLASS_BE:
            data = ionice_val >= 0 ? ionice_val : 4;
            if (data > 7) {
                if (should_warn_once(rule_name, "ionice range", EINVAL))
                    rj_log(RJ_LOG_WARN, "ionice level %d out of range for rule '%s'",
                           ionice_val, rule_name);
                return -1;
            }
            break;
        case IOPRIO_CLASS_IDLE:
            data = 7;
            break;
        default:
            if (should_warn_once(rule_name, "ionice class", EINVAL))
                rj_log(RJ_LOG_WARN, "invalid io class for rule '%s'", rule_name);
            return -1;
    }

    priority = IOPRIO_PRIO_VALUE(class_num, data);
    if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, (int)pid, priority) == 0)
        return 0;
    if (errno == ESRCH) return 1;
    apply_warn(rule_name, "ioprio_set", pid);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Rule application                                                    */
/* ------------------------------------------------------------------ */

int rule_apply(const Rule *r, const ProcTypeMap *types, const RamjetConfig *cfg,
               pid_t pid, int is_process_leader) {
    int errors = 0, rc, value, level;
    IoClass ioclass;

    if (!r || !cfg) return -1;

    if (cfg->apply_nice && rule_effective_nice(r, types, &value) == 0) {
        if (cfg->dry_run) {
            rj_log(RJ_LOG_INFO, "dry-run: nice %d -> pid %d ('%s')",
                   value, (int)pid, r->name);
        } else {
            rc = apply_nice(value, pid, r->name);
            if (rc < 0) errors++;
        }
    }

    if (cfg->apply_ionice) {
        int has_class = rule_effective_ioclass(r, types, &ioclass) == 0;
        int has_level = rule_effective_ionice(r, types, &level) == 0;
        if (has_level && !has_class)
            ioclass = IO_CLASS_BEST_EFFORT;  /* Ananicy default class */
        if (has_class || has_level) {
            if (cfg->dry_run) {
                rj_log(RJ_LOG_INFO, "dry-run: io %s/%d -> pid %d ('%s')",
                       io_class_to_string(ioclass), has_level ? level : 4,
                       (int)pid, r->name);
            } else {
                rc = apply_ionice(ioclass, has_level ? level : -1, pid, r->name);
                if (rc < 0) errors++;
            }
        }
    }

    /* oom_score_adj is process-wide; apply it via the thread-group leader only. */
    if (is_process_leader && cfg->apply_oom_score_adj &&
        rule_effective_oom_score_adj(r, types, &value) == 0) {
        if (cfg->dry_run) {
            rj_log(RJ_LOG_INFO, "dry-run: oom_score_adj %d -> pid %d ('%s')",
                   value, (int)pid, r->name);
        } else {
            rc = apply_oom_score_adj(value, pid, r->name);
            if (rc < 0) errors++;
        }
    }

    if (cfg->log_applied_rule && !cfg->dry_run)
        rj_log(RJ_LOG_DEBUG, "applied rule '%s' to pid %d", r->name, (int)pid);

    return errors ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Rule loading                                                        */
/* ------------------------------------------------------------------ */

typedef struct { RuleMap *map; } BuildRulesCtx;

static int parse_rule_line(const char *json_line, void *user_data) {
    BuildRulesCtx *ctx = user_data;
    Rule r;
    char buf[32];
    long value;
    int found;

    memset(&r, 0, sizeof(r));

    found = json_get_string(json_line, "name", r.name, sizeof(r.name));
    if (found != 1 || !r.name[0]) return 0;
    if (json_get_string(json_line, "type", r.proc_type_name,
                        sizeof(r.proc_type_name)) < 0) return 0;

    if (json_get_int(json_line, "nice", &value) == 1) {
        if (value < -20 || value > 19) {
            rj_log(RJ_LOG_WARN, "invalid nice %ld for rule '%s': field ignored",
                   value, r.name);
        } else {
            r.nice = (int)value;
            r.has_nice = 1;
        }
    }

    found = json_get_string(json_line, "io-class", buf, sizeof(buf));
    if (found == 0) found = json_get_string(json_line, "ioclass", buf, sizeof(buf));
    if (found == 1) {
        if (io_class_from_string(buf, &r.ioclass) == 0) r.has_ioclass = 1;
        else rj_log(RJ_LOG_WARN, "unknown ioclass '%s' in rule '%s'", buf, r.name);
    }

    if (json_get_int(json_line, "ionice", &value) == 1) {
        if (value < 0 || value > 7) {
            rj_log(RJ_LOG_WARN, "invalid ionice %ld for rule '%s': field ignored",
                   value, r.name);
        } else {
            r.ionice = (int)value;
            r.has_ionice = 1;
        }
    }

    if (json_get_int(json_line, "oom_score_adj", &value) == 1) {
        if (value < -1000 || value > 1000) {
            rj_log(RJ_LOG_WARN, "invalid oom_score_adj %ld for rule '%s': field ignored",
                   value, r.name);
        } else {
            r.oom_score_adj = (int)value;
            r.has_oom_score_adj = 1;
        }
    }

    rule_map_insert(ctx->map, &r);
    return 0;
}

int build_rules(RuleMap *map, const char *config_dir) {
    rule_map_init(map);
    BuildRulesCtx ctx = { .map = map };
    return walk_config_dir(config_dir, "rules", parse_rule_line, &ctx);
}
