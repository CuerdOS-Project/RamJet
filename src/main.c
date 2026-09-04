/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "ramjet.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_terminated = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_terminated = 1;
}

static int setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    return 0;
}

static int interruptible_sleep(time_t seconds) {
    struct timespec remaining = { .tv_sec = seconds, .tv_nsec = 0 };
    while (!g_terminated) {
        if (nanosleep(&remaining, &remaining) == 0) return 0;
        if (errno != EINTR) return -1;
    }
    return 1;
}

static int map_count_rules(const RuleMap *map) {
    int count = 0;
    for (int i = 0; i < RULE_MAP_SIZE; i++)
        for (RuleEntry *e = map->buckets[i]; e; e = e->next) count++;
    return count;
}

static int map_count_types(const ProcTypeMap *map) {
    int count = 0;
    for (int i = 0; i < PROC_TYPE_MAP_SIZE; i++)
        for (ProcTypeEntry *e = map->buckets[i]; e; e = e->next) count++;
    return count;
}

static int map_count_cgroups(const CgroupMap *map) {
    int count = 0;
    for (int i = 0; i < CGROUP_MAP_SIZE; i++)
        for (CgroupEntry *e = map->buckets[i]; e; e = e->next) count++;
    return count;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (setup_signals() != 0) {
        fprintf(stderr, "[ramjet] error: failed to register signal handlers: %s\n", strerror(errno));
        return 1;
    }

    CgroupMap cgroups;
    ProcTypeMap types;
    RuleMap rules;

    if (build_cgroups(&cgroups) != 0)
        fprintf(stderr, "[ramjet] warn: cgroups unavailable or failed to load\n");
    fprintf(stderr, "[ramjet] info: %d cgroups loaded\n", map_count_cgroups(&cgroups));

    if (build_types(&types) != 0)
        fprintf(stderr, "[ramjet] warn: failed to load some types\n");
    fprintf(stderr, "[ramjet] info: %d types loaded\n", map_count_types(&types));

    if (build_rules(&rules, &types) != 0)
        fprintf(stderr, "[ramjet] warn: failed to load some rules\n");
    fprintf(stderr, "[ramjet] info: %d rules loaded\n", map_count_rules(&rules));

    while (!g_terminated) {
        int errors = apply_all_rules(&rules, &types, &cgroups);
        if (errors > 0) fprintf(stderr, "[ramjet] warn: %d errors during rule application\n", errors);
        if (interruptible_sleep(5) < 0) {
            fprintf(stderr, "[ramjet] warn: sleep failed: %s\n", strerror(errno));
            break;
        }
    }

    rule_map_free(&rules);
    proc_type_map_free(&types);
    cgroup_map_free(&cgroups);
    fprintf(stderr, "[ramjet] info: shutting down\n");
    return 0;
}
