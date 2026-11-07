/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#include "ramjet.h"
#include "log.h"
#include "parse.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_terminated = 0;
static volatile sig_atomic_t g_reload = 0;

static void signal_handler(int sig) {
    if (sig == SIGHUP) g_reload = 1;
    else g_terminated = 1;
}

static int setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    if (sigaction(SIGHUP, &sa, NULL) != 0) return -1;
    return 0;
}

static int interruptible_sleep(double seconds) {
    struct timespec remaining;
    time_t whole = (time_t)seconds;

    remaining.tv_sec = whole;
    remaining.tv_nsec = (long)((seconds - (double)whole) * 1e9);
    if (remaining.tv_nsec < 0) remaining.tv_nsec = 0;
    while (!g_terminated) {
        if (nanosleep(&remaining, &remaining) == 0) return 0;
        if (errno != EINTR) return -1;
        if (g_reload) return 0; /* wake up early for reload */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

void config_defaults(RamjetConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->config_dir, sizeof(cfg->config_dir), "%s", "/etc/ananicy.d");
    snprintf(cfg->conf_file, sizeof(cfg->conf_file), "%s", "/etc/ananicy.conf");
    cfg->check_freq = 5.0;
    cfg->cache_ttl = 60.0;
    cfg->type_load = 1;
    cfg->rule_load = 1;
    cfg->apply_nice = 1;
    cfg->apply_ionice = 1;
    cfg->apply_oom_score_adj = 1;
    cfg->log_applied_rule = 0;
    cfg->loglevel = RJ_LOG_INFO;
    cfg->dry_run = 0;
}

static int parse_bool(const char *v, int *out) {
    if (!v) return -1;
    if (!strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "1")) {
        *out = 1;
        return 0;
    }
    if (!strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "off") || !strcmp(v, "0")) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int parse_loglevel(const char *v, int *out) {
    if (!v) return -1;
    if (!strcmp(v, "trace") || !strcmp(v, "debug")) { *out = RJ_LOG_DEBUG; return 0; }
    if (!strcmp(v, "info"))  { *out = RJ_LOG_INFO;  return 0; }
    if (!strcmp(v, "warn") || !strcmp(v, "warning")) { *out = RJ_LOG_WARN; return 0; }
    if (!strcmp(v, "error") || !strcmp(v, "critical")) { *out = RJ_LOG_ERROR; return 0; }
    return -1;
}

static void apply_conf_key(RamjetConfig *cfg, const char *key, const char *value) {
    int b, level;

    if (!strcmp(key, "check_freq")) {
        char *end = NULL;
        double d = strtod(value, &end);
        if (end && *end == '\0' && d >= 0.01 && d <= 86400.0) {
            cfg->check_freq = d;
        } else {
            rj_log(RJ_LOG_WARN, "invalid check_freq '%s' ignored", value);
        }
    } else if (!strcmp(key, "type_load")) {
        if (parse_bool(value, &b) == 0) cfg->type_load = b;
    } else if (!strcmp(key, "rule_load")) {
        if (parse_bool(value, &b) == 0) cfg->rule_load = b;
    } else if (!strcmp(key, "apply_nice")) {
        if (parse_bool(value, &b) == 0) cfg->apply_nice = b;
    } else if (!strcmp(key, "apply_ionice")) {
        if (parse_bool(value, &b) == 0) cfg->apply_ionice = b;
    } else if (!strcmp(key, "apply_oom_score_adj")) {
        if (parse_bool(value, &b) == 0) cfg->apply_oom_score_adj = b;
    } else if (!strcmp(key, "log_applied_rule")) {
        if (parse_bool(value, &b) == 0) cfg->log_applied_rule = b;
    } else if (!strcmp(key, "loglevel")) {
        if (parse_loglevel(value, &level) == 0) cfg->loglevel = level;
        else rj_log(RJ_LOG_WARN, "invalid loglevel '%s' ignored", value);
    } else if (!strcmp(key, "apply_latnice") || !strcmp(key, "apply_sched")) {
        /* recognized for compatibility with Ananicy configs, not supported */
        rj_log(RJ_LOG_DEBUG, "config key '%s' is not supported and is ignored", key);
    }
}

int config_load(RamjetConfig *cfg) {
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int applied = 0;
    char key[128], value[512];

    fp = fopen(cfg->conf_file, "r");
    if (!fp) return 0; /* no config file: defaults apply */
    while ((len = getline(&line, &cap, fp)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (parse_kv_line(line, key, sizeof(key), value, sizeof(value)) == 1) {
            apply_conf_key(cfg, key, value);
            applied++;
        }
    }
    free(line);
    fclose(fp);
    return applied;
}

/* ------------------------------------------------------------------ */
/* Loading (startup + SIGHUP)                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    const ProcTypeMap *types;
    int missing;
} TypeCheckCtx;

static void check_rule_type_cb(const Rule *r, void *user_data) {
    TypeCheckCtx *c = user_data;
    if (r->proc_type_name[0] && !proc_type_map_get(c->types, r->proc_type_name)) {
        rj_log(RJ_LOG_WARN, "rule '%s' references undefined type '%s'",
               r->name, r->proc_type_name);
        c->missing++;
    }
}

static void load_all(ProcTypeMap *types, RuleMap *rules, const RamjetConfig *cfg) {
    TypeCheckCtx check = { NULL, 0 };

    if (cfg->type_load) {
        if (build_types(types, cfg->config_dir) != 0)
            rj_log(RJ_LOG_WARN, "failed to load some types");
        rj_log(RJ_LOG_INFO, "%d types loaded", proc_type_map_count(types));
    } else {
        proc_type_map_init(types);
    }

    if (cfg->rule_load) {
        if (build_rules(rules, cfg->config_dir) != 0)
            rj_log(RJ_LOG_WARN, "failed to load some rules");
        rj_log(RJ_LOG_INFO, "%d rules loaded", rule_map_count(rules));
    } else {
        rule_map_init(rules);
    }

    check.types = types;
    rule_map_foreach(rules, check_rule_type_cb, &check);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(FILE *out) {
    fprintf(out,
        "ramjet %s - process priority daemon (CuerdOS)\n\n"
        "Usage: ramjet [OPTIONS]\n\n"
        "Options:\n"
        "  -c DIR      rules/types directory (default: /etc/ananicy.d)\n"
        "  -f FILE     key=value config file (default: /etc/ananicy.conf)\n"
        "  -t SECONDS  scan interval (default: 5, range 0.01..86400)\n"
        "  -T SECONDS  cache TTL, 0 disables the cache (default: 60)\n"
        "  -l LEVEL    log level: debug, info, warn, error (default: info)\n"
        "  -n          dry run: log what would be applied, change nothing\n"
        "  -d          shortcut for -l debug\n"
        "  -V          print version and exit\n"
        "  -h          show this help and exit\n\n"
        "Signals:\n"
        "  SIGINT/SIGTERM  stop\n"
        "  SIGHUP          reload rules and types\n",
        RAMJET_VERSION);
}

static int parse_seconds(const char *s, double *out) {
    char *end = NULL;
    double d = strtod(s, &end);
    if (!end || *end != '\0' || d < 0.0 || d > 86400.0) return -1;
    *out = d;
    return 0;
}

int main(int argc, char **argv) {
    RamjetConfig cfg;
    ProcTypeMap types;
    RuleMap rules;
    ProcCache cache;
    int opt;

    config_defaults(&cfg);
    config_load(&cfg); /* ananicy.conf first: CLI flags override it */

    while ((opt = getopt(argc, argv, "c:f:t:T:l:ndVh")) != -1) {
        double d;
        int level;
        switch (opt) {
            case 'c':
                snprintf(cfg.config_dir, sizeof(cfg.config_dir), "%s", optarg);
                break;
            case 'f':
                snprintf(cfg.conf_file, sizeof(cfg.conf_file), "%s", optarg);
                break;
            case 't':
                if (parse_seconds(optarg, &d) != 0 || d < 0.01) {
                    fprintf(stderr, "ramjet: invalid interval '%s'\n", optarg);
                    return 1;
                }
                cfg.check_freq = d;
                break;
            case 'T':
                if (parse_seconds(optarg, &d) != 0) {
                    fprintf(stderr, "ramjet: invalid cache TTL '%s'\n", optarg);
                    return 1;
                }
                cfg.cache_ttl = d;
                break;
            case 'l':
                if (parse_loglevel(optarg, &level) != 0) {
                    fprintf(stderr, "ramjet: invalid log level '%s'\n", optarg);
                    return 1;
                }
                cfg.loglevel = level;
                break;
            case 'n':
                cfg.dry_run = 1;
                break;
            case 'd':
                cfg.loglevel = RJ_LOG_DEBUG;
                break;
            case 'V':
                printf("ramjet %s\n", RAMJET_VERSION);
                return 0;
            case 'h':
                usage(stdout);
                return 0;
            default:
                usage(stderr);
                return 1;
        }
    }

    rj_set_loglevel(cfg.loglevel);

    if (setup_signals() != 0) {
        rj_log(RJ_LOG_ERROR, "failed to register signal handlers: %s", strerror(errno));
        return 1;
    }

    rj_log(RJ_LOG_INFO, "ramjet %s starting (config dir: %s, interval %.3gs, cache TTL %gs)",
           RAMJET_VERSION, cfg.config_dir, cfg.check_freq, cfg.cache_ttl);
    if (cfg.dry_run)
        rj_log(RJ_LOG_INFO, "dry-run mode: no changes will be applied");

    load_all(&types, &rules, &cfg);
    proc_cache_init(&cache);

    while (!g_terminated) {
        int errors = apply_all_rules(&rules, &types, &cfg, &cache);
        if (errors > 0)
            rj_log(RJ_LOG_WARN, "%d errors during rule application", errors);

        if (interruptible_sleep(cfg.check_freq) < 0) {
            rj_log(RJ_LOG_WARN, "sleep failed: %s", strerror(errno));
            break;
        }

        if (g_reload) {
            g_reload = 0;
            rj_log(RJ_LOG_INFO, "SIGHUP received, reloading configuration");
            rule_map_free(&rules);
            proc_type_map_free(&types);
            load_all(&types, &rules, &cfg);
            proc_cache_reset(&cache);
        }
    }

    rule_map_free(&rules);
    proc_type_map_free(&types);
    proc_cache_free(&cache);
    rj_log(RJ_LOG_INFO, "shutting down");
    return 0;
}
