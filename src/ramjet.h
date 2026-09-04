/*
 * RamJet - Rice clone in C
 * Copyright (c) 2026 - Ported from Rust by Alecaishere/CuerdOS Dev. Team
 */
#ifndef RAMJET_H
#define RAMJET_H

#include "rule.h"
#include "cgroup.h"
#include "proc_type.h"
#include <sys/types.h>

typedef int (*proc_callback)(pid_t pid, const char *exe_name, void *user_data);
int iterate_procs(proc_callback cb, void *user_data);
int apply_all_rules(const RuleMap *rules, const ProcTypeMap *types, CgroupMap *cgroups);

#endif
