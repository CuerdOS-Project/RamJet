#include "../src/class.h"
#include "../src/parse.h"
#include "../src/proc_type.h"
#include "../src/rule.h"
#include "../src/ramjet.h"
#include "../src/log.h"
#include <assert.h>
#include <linux/ioprio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

static void test_classes(void) {
    IoClass c;
    assert(io_class_from_string("idle", &c) == 0 && io_class_to_num(c) == 3);
    assert(io_class_from_string("best-effort", &c) == 0 && io_class_to_num(c) == 2);
    assert(io_class_from_string("realtime", &c) == 0 && io_class_to_num(c) == 1);
}

static void test_json(void) {
    const char *line = "{ \"name\": \"example\", \"nice\": -20, \"ioclass\": \"best-effort\", \"ionice\": 7 }";
    char name[32];
    long value;
    assert(json_get_string(line, "name", name, sizeof(name)) == 1);
    assert(strcmp(name, "example") == 0);
    assert(json_get_int(line, "nice", &value) == 1 && value == -20);
    assert(json_get_int(line, "ionice", &value) == 1 && value == 7);
    assert(json_get_int(line, "missing", &value) == 0);

    assert(json_get_string("{\"name\":\"a\\b\"}", "name", name, sizeof(name)) == 1);
    assert(strcmp(name, "a\b") == 0);

    /* values containing nested objects must be skipped correctly */
    const char *nested = "{ \"type\": \"Outer\", \"opts\": { \"type\": \"inner\" }, \"nice\": 3 }";
    char buf[32];
    assert(json_get_string(nested, "type", buf, sizeof(buf)) == 1);
    assert(strcmp(buf, "Outer") == 0);
    assert(json_get_int(nested, "nice", &value) == 1 && value == 3);
}

static void test_kv(void) {
    char key[64], value[128];
    assert(parse_kv_line("check_freq = 5", key, sizeof(key), value, sizeof(value)) == 1);
    assert(strcmp(key, "check_freq") == 0 && strcmp(value, "5") == 0);
    assert(parse_kv_line("# comment", key, sizeof(key), value, sizeof(value)) == 0);
    assert(parse_kv_line("  ## header ##  ", key, sizeof(key), value, sizeof(value)) == 0);
    assert(parse_kv_line("   ", key, sizeof(key), value, sizeof(value)) == 0);
    assert(parse_kv_line("apply_nice = true", key, sizeof(key), value, sizeof(value)) == 1);
    assert(strcmp(key, "apply_nice") == 0 && strcmp(value, "true") == 0);
    assert(parse_kv_line("no_equals_sign", key, sizeof(key), value, sizeof(value)) == -1);
    assert(parse_kv_line("  spaced_key   =   hello world  ", key, sizeof(key), value, sizeof(value)) == 1);
    assert(strcmp(key, "spaced_key") == 0 && strcmp(value, "hello world") == 0);
}

static void test_effective_values(void) {
    ProcTypeMap types;
    ProcType t;
    Rule r;
    int out;
    IoClass ic;

    proc_type_map_init(&types);
    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "Svc");
    t.has_nice = 1; t.nice = 7;
    t.has_ionice = 1; t.ionice = 5;
    t.has_ioclass = 1; t.ioclass = IO_CLASS_IDLE;
    t.has_oom_score_adj = 1; t.oom_score_adj = -500;
    proc_type_map_insert(&types, &t);

    /* rule without fields inherits everything from the type */
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "a");
    snprintf(r.proc_type_name, sizeof(r.proc_type_name), "Svc");
    assert(rule_effective_nice(&r, &types, &out) == 0 && out == 7);
    assert(rule_effective_ionice(&r, &types, &out) == 0 && out == 5);
    assert(rule_effective_ioclass(&r, &types, &ic) == 0 && ic == IO_CLASS_IDLE);
    assert(rule_effective_oom_score_adj(&r, &types, &out) == 0 && out == -500);

    /* regression: explicit nice = -1 must be honored (was eaten by the old
     * -1 sentinel) */
    r.has_nice = 1;
    r.nice = -1;
    assert(rule_effective_nice(&r, &types, &out) == 0 && out == -1);

    /* rule with unknown type: no inheritance, no crash, no value */
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "b");
    snprintf(r.proc_type_name, sizeof(r.proc_type_name), "Missing");
    assert(rule_effective_nice(&r, &types, &out) == -1);

    /* ionice-only rule resolves no ioclass (apply defaults to best-effort) */
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "c");
    r.has_ionice = 1;
    r.ionice = 7;
    assert(rule_effective_ioclass(&r, &types, &ic) == -1);
    assert(rule_effective_ionice(&r, &types, &out) == 0 && out == 7);

    proc_type_map_free(&types);
}

static void test_apply_self(void) {
    RamjetConfig cfg;
    ProcTypeMap types;
    Rule r;
    int io;

    config_defaults(&cfg);
    proc_type_map_init(&types);

    /* nice is applied to the calling process */
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "self");
    r.has_nice = 1;
    r.nice = 5;
    assert(rule_apply(&r, &types, &cfg, getpid(), 1) == 0);
    assert(getpriority(PRIO_PROCESS, 0) == 5);

    /* regression: ionice without ioclass must default to best-effort/7
     * (the old code silently skipped it) */
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "self2");
    r.has_ionice = 1;
    r.ionice = 7;
    assert(rule_apply(&r, &types, &cfg, getpid(), 1) == 0);
    io = (int)syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, (int)getpid());
    assert(io == IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 7));

    /* oom_score_adj is applied to the leader (self) */
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "selfoom");
    r.has_oom_score_adj = 1;
    r.oom_score_adj = 300;
    assert(rule_apply(&r, &types, &cfg, getpid(), 1) == 0);
    {
        char path[64], buf[32];
        FILE *fp;
        snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", (int)getpid());
        fp = fopen(path, "r");
        assert(fp != NULL);
        assert(fgets(buf, sizeof(buf), fp) != NULL);
        fclose(fp);
        assert(atoi(buf) == 300);
    }

    /* dry run must not touch anything */
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "self3");
    r.has_nice = 1;
    r.nice = 10;
    cfg.dry_run = 1;
    assert(rule_apply(&r, &types, &cfg, getpid(), 1) == 0);
    assert(getpriority(PRIO_PROCESS, 0) == 5); /* unchanged */
    cfg.dry_run = 0;

    /* regression: a vanished PID (ESRCH) is normal churn, not an error
     * (the old code counted it and warned on every cycle) */
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "gone");
    r.has_nice = 1;
    r.nice = 3;
    r.has_ionice = 1;
    r.ionice = 4;
    assert(rule_apply(&r, &types, &cfg, (pid_t)99999999, 1) == 0);

    proc_type_map_free(&types);
}

static void test_proc_helpers(void) {
    char exe[256];
    unsigned long long st = 0;
    int nth = 0;

    assert(ramjet_proc_exe(getpid(), exe, sizeof(exe)) == 0 && exe[0] != '\0');
    assert(ramjet_proc_stat(getpid(), &st, &nth) == 0);
    assert(st > 0 && nth >= 1);
    assert(ramjet_proc_stat((pid_t)99999999, &st, &nth) != 0);
}

static void test_cache(void) {
    ProcCache c;
    proc_cache_init(&c);

    proc_cache_put(&c, 123, 999, 4);
    assert(proc_cache_get(&c, 123, 999) != NULL);
    assert(proc_cache_get(&c, 123, 999)->num_threads == 4);
    /* PID reuse: same pid, different starttime -> miss */
    assert(proc_cache_get(&c, 123, 1000) == NULL);
    assert(proc_cache_get(&c, 124, 999) == NULL);

    /* replacing an entry drops the stale one */
    proc_cache_put(&c, 123, 1000, 6);
    assert(proc_cache_get(&c, 123, 999) == NULL);
    assert(proc_cache_get(&c, 123, 1000) != NULL);
    assert(proc_cache_get(&c, 123, 1000)->num_threads == 6);

    proc_cache_reset(&c);
    assert(proc_cache_get(&c, 123, 1000) == NULL);

    proc_cache_free(&c);
}

int main(void) {
    rj_set_loglevel(RJ_LOG_ERROR); /* silence warnings during tests */
    test_classes();
    test_json();
    test_kv();
    test_effective_values();
    test_apply_self();
    test_proc_helpers();
    test_cache();
    puts("RamJet core tests: OK");
    return 0;
}
