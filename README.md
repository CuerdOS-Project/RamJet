# RamJet

RamJet is a small C daemon inspired by Rice/Ananicy. It scans `/proc` and applies Ananicy-style `.rules`, `.types` definitions.

## Build

Dependencies are only the C toolchain and Linux headers:

```bash
make
make test
```

Install:

```bash
sudo make install
```

The daemon reads configuration from `/etc/ananicy.d` and runs in the foreground, which makes it suitable for systemd and runit.

## Supported rule fields

Rules support:

- `name`
- `type`
- `nice` (`-20` to `19`)
- `io-class` or `ioclass` (`realtime`, `best-effort`, `idle`)
- `ionice` (`0` to `7`)
- `oom_score_adj` (`-1000` to `1000`)

Types support the same scheduling fields and are inherited by rules when a rule does not define the corresponding value itself.

RamJet only adjusts process/thread scheduling priorities and OOM score. It does not create, configure or migrate cgroups; `cgroup` fields in external Ananicy rules/types are ignored.

## Service files

Systemd:

```bash
sudo install -m 644 systemd/ramjet.service /etc/systemd/system/ramjet.service
sudo systemctl daemon-reload
sudo systemctl enable --now ramjet.service
```

For runit, place the `runit/` directory under the appropriate service directory.

## Changes and optimizations in this new version:

1. Cgroup Management Removal
- Removed the cgroup management implementation and its associated source files.
- Removed cgroup-related structures, functions, initialization and cleanup routines.
- Removed cgroup dependencies from the build system.
- Removed bundled cgroup configuration files and obsolete cgroup fields from default rules and types.
- External Ananicy rules remain supported for other scheduling fields; `cgroup` entries are ignored.

2. I/O Priority Optimization
- Replaced external `ionice` process execution with a direct `ioprio_set` system call.
- Eliminated the need to spawn and wait for an external process when applying I/O priorities.
- Corrected the mapping between RamJet's internal I/O classes and Linux I/O priority class identifiers:
  - `realtime`: 1
  - `best-effort`: 2
  - `idle`: 3

3. Process Scanning Optimization
- Skip thread enumeration when a process executable does not match any rule.
- Reduced unnecessary `/proc` traversal for processes that RamJet does not need to manage.

4. OOM Score Handling
- Apply `oom_score_adj` once per thread group through its leader, avoiding redundant writes for individual threads.
- Keep `nice` and I/O priority handling at the task/thread level.

5. Rule Loading Simplification
- Removed the unused process-type parameter from `build_rules()`.
- Simplified the rule-loading context and removed obsolete cgroup-related parsing logic.

6. Reduced Internal Complexity
- Removed cgroup-specific state and callbacks from the main execution path.
- Simplified rule application and process management.
- Focused RamJet on process scheduling priorities, I/O priorities and OOM score adjustment rather than resource-control group management.
