/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "cgroup.h"
#include "parse.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANANICY_CONFIG_DIR "/etc/ananicy.d"
#define CGROUP_CPU_BASE "/sys/fs/cgroup/cpu"
#define PERIOD_US 100000UL

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + c;
    return h;
}

static int valid_name(const char *name) {
    if (!name || !*name) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') return 0;
    }
    return 1;
}

void cgroup_map_init(CgroupMap *map) { memset(map, 0, sizeof(*map)); }

void cgroup_map_free(CgroupMap *map) {
    for (int i = 0; i < CGROUP_MAP_SIZE; i++) {
        CgroupEntry *e = map->buckets[i];
        while (e) {
            CgroupEntry *next = e->next;
            if (e->owns_fs) {
                char path[512];
                snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s", e->def.name);
                if (rmdir(path) != 0 && errno != ENOENT && errno != EBUSY) {
                    fprintf(stderr, "[ramjet] warn: failed to remove cgroup %s: %s\n",
                            e->def.name, strerror(errno));
                }
            }
            free(e);
            e = next;
        }
        map->buckets[i] = NULL;
    }
}

static int write_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "[ramjet] warn: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }
    int ok = fputs(value, fp) != EOF && fclose(fp) == 0;
    if (!ok) fprintf(stderr, "[ramjet] warn: failed to write %s\n", path);
    return ok ? 0 : -1;
}

static int create_cgroup_fs(const CgroupDef *def, int *created) {
    char path[512], val[64];
    long ncpus;
    unsigned long shares;
    long quota_us;

    if (!valid_name(def->name)) return -1;
    snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s", def->name);
    if (mkdir(path, 0755) == 0) {
        *created = 1;
    } else if (errno == EEXIST) {
        *created = 0;
    } else {
        fprintf(stderr, "[ramjet] warn: failed to create cgroup %s: %s\n", path, strerror(errno));
        return -1;
    }

    shares = 1024UL * def->cpu_quota / 100;
    if (shares < 2) shares = 2;
    snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s/cpu.shares", def->name);
    snprintf(val, sizeof(val), "%lu", shares);
    if (write_file(path, val) != 0) goto fail;

    snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s/cpu.cfs_period_us", def->name);
    snprintf(val, sizeof(val), "%lu", PERIOD_US);
    if (write_file(path, val) != 0) goto fail;

    ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus <= 0) ncpus = 1;
    quota_us = (long)(PERIOD_US * (unsigned long)ncpus * def->cpu_quota / 100);
    if (quota_us < 1000) quota_us = 1000;
    snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s/cpu.cfs_quota_us", def->name);
    snprintf(val, sizeof(val), "%ld", quota_us);
    if (write_file(path, val) == 0) return 0;

fail:
    if (*created) {
        snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s", def->name);
        rmdir(path);
        *created = 0;
    }
    return -1;
}

int cgroup_map_insert(CgroupMap *map, const CgroupDef *def) {
    unsigned long idx;
    CgroupEntry *entry;
    int created = 0;
    if (!map || !def || !valid_name(def->name)) return -1;
    if (create_cgroup_fs(def, &created) != 0) return -1;

    idx = hash_str(def->name) % CGROUP_MAP_SIZE;
    for (entry = map->buckets[idx]; entry; entry = entry->next) {
        if (strcmp(entry->def.name, def->name) == 0) {
            entry->def = *def;
            if (created) entry->owns_fs = 1;
            return 0;
        }
    }

    entry = malloc(sizeof(*entry));
    if (!entry) {
        if (created) {
            char path[512];
            snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s", def->name);
            rmdir(path);
        }
        fprintf(stderr, "[ramjet] error: out of memory inserting cgroup\n");
        return -1;
    }
    entry->def = *def;
    entry->owns_fs = created;
    entry->next = map->buckets[idx];
    map->buckets[idx] = entry;
    return 0;
}

const CgroupDef *cgroup_map_get(const CgroupMap *map, const char *name) {
    if (!map || !name) return NULL;
    unsigned long idx = hash_str(name) % CGROUP_MAP_SIZE;
    for (const CgroupEntry *e = map->buckets[idx]; e; e = e->next) {
        if (strcmp(e->def.name, name) == 0) return &e->def;
    }
    return NULL;
}

int cgroup_apply(const CgroupDef *cg, pid_t pid) {
    char path[512], val[32];
    if (!cg || pid <= 0) return -1;
    snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s/cgroup.procs", cg->name);
    snprintf(val, sizeof(val), "%d", pid);
    if (write_file(path, val) == 0) return 0;
    snprintf(path, sizeof(path), CGROUP_CPU_BASE "/%s/tasks", cg->name);
    return write_file(path, val);
}

typedef struct { CgroupMap *map; } BuildCgroupsCtx;

static int parse_cgroup_line(const char *json_line, void *user_data) {
    BuildCgroupsCtx *ctx = user_data;
    CgroupDef def;
    char name[CGROUP_NAME_MAX];
    long quota;
    int found;

    memset(&def, 0, sizeof(def));
    found = json_get_string(json_line, "cgroup", name, sizeof(name));
    if (found != 1 || !valid_name(name)) {
        fprintf(stderr, "[ramjet] warn: invalid cgroup name\n");
        return 0;
    }

    found = json_get_int(json_line, "CPUQuota", &quota);
    if (found == 0) found = json_get_int(json_line, "cpu_quota", &quota);
    if (found != 1 || quota < 1 || quota > 100) {
        fprintf(stderr, "[ramjet] warn: invalid CPUQuota for cgroup %s\n", name);
        return 0;
    }

    snprintf(def.name, sizeof(def.name), "%s", name);
    def.cpu_quota = (unsigned char)quota;
    cgroup_map_insert(ctx->map, &def);
    return 0;
}

int build_cgroups(CgroupMap *map) {
    struct stat st;
    cgroup_map_init(map);
    if (stat(CGROUP_CPU_BASE, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[ramjet] warn: cgroup v1 CPU controller not available at %s\n", CGROUP_CPU_BASE);
        return -1;
    }
    BuildCgroupsCtx ctx = { .map = map };
    return walk_config_dir(ANANICY_CONFIG_DIR, "cgroups", parse_cgroup_line, &ctx);
}
