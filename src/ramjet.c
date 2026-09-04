/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "ramjet.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int get_proc_exe(pid_t pid, char *buf, size_t bufsz) {
    char link_path[64], target[PATH_MAX];
    ssize_t len;
    if (!buf || bufsz < 2) return -1;
    snprintf(link_path, sizeof(link_path), "/proc/%d/exe", pid);
    len = readlink(link_path, target, sizeof(target) - 1);
    if (len < 0) return -1;
    target[len] = '\0';
    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;
    if (!*base) return -1;
    size_t base_len = strlen(base);
    if (base_len >= bufsz) return -1;
    memcpy(buf, base, base_len + 1);
    return 0;
}

static int iterate_threads(pid_t pid, const char *exe_name, proc_callback cb, void *user_data) {
    char task_dir[64];
    DIR *d;
    struct dirent *ent;
    snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", pid);
    d = opendir(task_dir);
    if (!d) return 0; /* Process may have exited. */

    while ((ent = readdir(d)) != NULL) {
        pid_t tid;
        if (parse_pid(ent->d_name, &tid) != 0 || tid == pid) continue;
        int ret = cb(tid, exe_name, user_data);
        if (ret != 0) {
            closedir(d);
            return ret;
        }
    }
    closedir(d);
    return 0;
}

int iterate_procs(proc_callback cb, void *user_data) {
    if (!cb) return -1;
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        fprintf(stderr, "[ramjet] error: failed to open /proc: %s\n", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(proc_dir)) != NULL) {
        pid_t pid;
        char exe_name[256];
        if (parse_pid(ent->d_name, &pid) != 0) continue;
        if (get_proc_exe(pid, exe_name, sizeof(exe_name)) != 0) continue;

        int ret = cb(pid, exe_name, user_data);
        if (ret != 0) {
            closedir(proc_dir);
            return ret;
        }
        ret = iterate_threads(pid, exe_name, cb, user_data);
        if (ret != 0) {
            closedir(proc_dir);
            return ret;
        }
    }
    closedir(proc_dir);
    return 0;
}

typedef struct {
    const RuleMap *rules;
    const ProcTypeMap *types;
    CgroupMap *cgroups;
    int error_count;
} ApplyContext;

static int apply_rule_callback(pid_t pid, const char *exe_name, void *user_data) {
    ApplyContext *ctx = user_data;
    const Rule *r = rule_map_get(ctx->rules, exe_name);
    if (!r) return 0;

    if (rule_apply(r, ctx->types, pid) != 0) ctx->error_count++;

    const char *cgroup_name = rule_effective_cgroup(r, ctx->types);
    if (cgroup_name) {
        const CgroupDef *cg = cgroup_map_get(ctx->cgroups, cgroup_name);
        if (!cg || cgroup_apply(cg, pid) != 0) ctx->error_count++;
    }
    return 0;
}

int apply_all_rules(const RuleMap *rules, const ProcTypeMap *types, CgroupMap *cgroups) {
    ApplyContext ctx = { .rules = rules, .types = types, .cgroups = cgroups, .error_count = 0 };
    if (iterate_procs(apply_rule_callback, &ctx) != 0) return ctx.error_count + 1;
    return ctx.error_count;
}
