/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "rule.h"
#include "parse.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define ANANICY_CONFIG_DIR "/etc/ananicy.d"

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c;
    return h;
}

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
    idx = hash_str(r->name) % RULE_MAP_SIZE;
    for (RuleEntry *e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->rule.name, r->name) == 0) {
            e->rule = *r;
            return;
        }
    }
    RuleEntry *entry = malloc(sizeof(*entry));
    if (!entry) {
        fprintf(stderr, "[ramjet] error: out of memory inserting rule\n");
        return;
    }
    entry->rule = *r;
    entry->next = map->buckets[idx];
    map->buckets[idx] = entry;
}

const Rule *rule_map_get(const RuleMap *map, const char *name) {
    if (!map || !name) return NULL;
    unsigned long idx = hash_str(name) % RULE_MAP_SIZE;
    for (const RuleEntry *e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->rule.name, name) == 0) return &e->rule;
    }
    return NULL;
}

const ProcType *rule_type(const Rule *r, const ProcTypeMap *types) {
    return (r && types && r->proc_type_name[0]) ?
        proc_type_map_get(types, r->proc_type_name) : NULL;
}

const char *rule_effective_cgroup(const Rule *r, const ProcTypeMap *types) {
    const ProcType *pt;
    if (r->cgroup[0]) return r->cgroup;
    pt = rule_type(r, types);
    return pt && pt->cgroup[0] ? pt->cgroup : NULL;
}

int rule_effective_nice(const Rule *r, const ProcTypeMap *types) {
    const ProcType *pt;
    if (r->nice != -1) return r->nice;
    pt = rule_type(r, types);
    return pt ? pt->nice : -1;
}

int rule_effective_ioclass(const Rule *r, const ProcTypeMap *types, IoClass *out) {
    const ProcType *pt;
    if (!out) return -1;
    if (r->has_ioclass) {
        *out = r->ioclass;
        return 0;
    }
    pt = rule_type(r, types);
    if (pt && pt->has_ioclass) {
        *out = pt->ioclass;
        return 0;
    }
    return -1;
}

int rule_effective_ionice(const Rule *r, const ProcTypeMap *types) {
    const ProcType *pt;
    if (r->ionice != -1) return r->ionice;
    pt = rule_type(r, types);
    return pt ? pt->ionice : -1;
}

int rule_effective_oom_score_adj(const Rule *r, const ProcTypeMap *types, int *out) {
    const ProcType *pt;
    if (!out) return -1;
    if (r->has_oom_score_adj) {
        *out = r->oom_score_adj;
        return 0;
    }
    pt = rule_type(r, types);
    if (pt && pt->has_oom_score_adj) {
        *out = pt->oom_score_adj;
        return 0;
    }
    return -1;
}

static int apply_nice(int nice_val, pid_t pid) {
    if (setpriority(PRIO_PROCESS, (id_t)pid, nice_val) == 0) return 0;
    if (errno == ESRCH) return -1;
    fprintf(stderr, "[ramjet] warn: setpriority(%d, %d) failed: %s\n",
            pid, nice_val, strerror(errno));
    return -1;
}

static int apply_oom_score_adj(int value, pid_t pid) {
    char path[64], text[32];
    snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
    snprintf(text, sizeof(text), "%d", value);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        if (errno == ESRCH) return -1;
        fprintf(stderr, "[ramjet] warn: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }
    int ok = fprintf(fp, "%s", text) >= 0 && fclose(fp) == 0;
    if (!ok) fprintf(stderr, "[ramjet] warn: failed to set oom_score_adj for pid %d\n", pid);
    return ok ? 0 : -1;
}

static int apply_ionice(IoClass ioclass, int ionice_val, pid_t pid) {
    char class_arg[8], nice_arg[16], pid_arg[16];
    snprintf(class_arg, sizeof(class_arg), "%d", io_class_to_num(ioclass));
    snprintf(pid_arg, sizeof(pid_arg), "%d", pid);

    if (ionice_val >= 0 && (ioclass == IO_CLASS_REALTIME || ioclass == IO_CLASS_BEST_EFFORT)) {
        snprintf(nice_arg, sizeof(nice_arg), "%d", ionice_val);
        char *const argv[] = { (char *)"ionice", (char *)"-c", class_arg,
                               (char *)"-n", nice_arg, (char *)"-p", pid_arg, NULL };
        pid_t child = fork();
        if (child == 0) {
            execvp(argv[0], argv);
            _exit(127);
        }
        if (child < 0) return -1;
        int status;
        if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
        return 0;
    }

    char *const argv[] = { (char *)"ionice", (char *)"-c", class_arg,
                           (char *)"-p", pid_arg, NULL };
    pid_t child = fork();
    if (child == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (child < 0) return -1;
    int status;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

int rule_apply(const Rule *r, const ProcTypeMap *types, pid_t pid) {
    int errors = 0;
    int value;
    IoClass ioclass;

    if (!r) return -1;
    int nice_val = rule_effective_nice(r, types);
    if (nice_val != -1 && apply_nice(nice_val, pid) != 0) errors++;

    if (rule_effective_ioclass(r, types, &ioclass) == 0 &&
        apply_ionice(ioclass, rule_effective_ionice(r, types), pid) != 0) errors++;

    if (rule_effective_oom_score_adj(r, types, &value) == 0 &&
        apply_oom_score_adj(value, pid) != 0) errors++;

    return errors ? -1 : 0;
}

typedef struct { RuleMap *map; const ProcTypeMap *types; } BuildRulesCtx;

static int parse_rule_line(const char *json_line, void *user_data) {
    BuildRulesCtx *ctx = user_data;
    Rule r;
    char buf[RULE_CGROUP_MAX];
    long value;
    int found;

    memset(&r, 0, sizeof(r));
    r.nice = -1;
    r.ionice = -1;

    found = json_get_string(json_line, "name", r.name, sizeof(r.name));
    if (found != 1 || !r.name[0]) return 0;
    if (json_get_string(json_line, "type", r.proc_type_name, sizeof(r.proc_type_name)) < 0) return 0;

    if (json_get_int(json_line, "nice", &value) == 1) {
        if (value < -20 || value > 19) {
            fprintf(stderr, "[ramjet] warn: invalid nice value %ld for rule %s\n", value, r.name);
            return 0;
        }
        r.nice = (int)value;
    }

    found = json_get_string(json_line, "io-class", buf, sizeof(buf));
    if (found == 0) found = json_get_string(json_line, "ioclass", buf, sizeof(buf));
    if (found == 1) {
        if (io_class_from_string(buf, &r.ioclass) == 0) r.has_ioclass = 1;
        else fprintf(stderr, "[ramjet] warn: unknown ioclass '%s' in rule '%s'\n", buf, r.name);
    }

    if (json_get_int(json_line, "ionice", &value) == 1) {
        if (value < 0 || value > 7) {
            fprintf(stderr, "[ramjet] warn: invalid ionice value %ld for rule %s\n", value, r.name);
            return 0;
        }
        r.ionice = (int)value;
    }

    if (json_get_string(json_line, "cgroup", r.cgroup, sizeof(r.cgroup)) < 0) return 0;

    if (json_get_int(json_line, "oom_score_adj", &value) == 1) {
        if (value < -1000 || value > 1000) {
            fprintf(stderr, "[ramjet] warn: invalid oom_score_adj %ld for rule %s\n", value, r.name);
            return 0;
        }
        r.oom_score_adj = (int)value;
        r.has_oom_score_adj = 1;
    }

    rule_map_insert(ctx->map, &r);
    return 0;
}

int build_rules(RuleMap *map, const ProcTypeMap *types) {
    rule_map_init(map);
    BuildRulesCtx ctx = { .map = map, .types = types };
    return walk_config_dir(ANANICY_CONFIG_DIR, "rules", parse_rule_line, &ctx);
}
