# RamJet

RamJet is a small C daemon inspired by Rice/Ananicy. It scans `/proc` and applies
Ananicy-style `.rules` and `.types` definitions to process/thread priorities,
I/O priorities and OOM scores.

## What's new in 1.3 (optimization pass)

- **PID cache**: priorities are re-applied only when a process is new, gained
  threads or the cache TTL expired (default 60 s), removing the bulk of
  `setpriority`/`ioprio_set`/`oom_score_adj` syscalls from every scan.
- **Bug fixes**:
  - rules with `nice: -1` are applied (`-1` was used as an internal sentinel);
  - rules with only `ionice` now default to the `best-effort` class, like Ananicy;
  - executables replaced on disk (` (deleted)` suffix) still match their rules;
  - dead processes (`ESRCH`) no longer count as errors (no more warn spam);
  - repeated failures (e.g. `EPERM` without `CAP_SYS_NICE`) are logged once, not
    on every cycle;
  - `oom_score_adj` uses raw `write(2)` and no longer leaks a `FILE*`.
- **Deterministic loading**: config files are parsed in sorted path order, so
  `00-...` files are reliably overridden by later files. Redefined rules/types
  and rules referencing undefined types are reported.
- **Runtime configuration**: `/etc/ananicy.conf` is now actually read
  (`check_freq`, `type_load`, `rule_load`, `apply_nice`, `apply_ionice`,
  `apply_oom_score_adj`, `log_applied_rule`, `loglevel`).
- **SIGHUP** reloads rules and types without restarting the daemon.
- **CLI**: `-c DIR`, `-f FILE`, `-t SECONDS`, `-T SECONDS`, `-l LEVEL`, `-n`
  (dry run), `-d` (debug), `-V`, `-h`.
- Dead code removed: `src/cJSON.c`/`src/cJSON.h` were never compiled.

## Build

Dependencies are only the C toolchain and Linux headers:

```bash
make
make test
```

Install the binary **and** the default configuration:

```bash
sudo make install            # /usr/local/bin/ramjet + /etc/ananicy.d + /etc/ananicy.conf
sudo make install-bin        # binary only
```

`make install` never overwrites existing rules or types. To install under
another prefix (e.g. `/usr`), use `make install PREFIX=/usr` and adjust the
service files accordingly.

## Usage

```
ramjet [OPTIONS]
  -c DIR      rules/types directory (default: /etc/ananicy.d)
  -f FILE     key=value config file (default: /etc/ananicy.conf)
  -t SECONDS  scan interval (default: 5)
  -T SECONDS  cache TTL, 0 disables the cache (default: 60)
  -l LEVEL    log level: debug, info, warn, error
  -n          dry run: log what would be applied, change nothing
  -d          shortcut for -l debug
```

Signals: `SIGINT`/`SIGTERM` stop the daemon, `SIGHUP` reloads the
configuration and flushes the PID cache.

Tip: `ramjet -n -d -c /etc/ananicy.d` shows exactly which rules would be
applied to which processes, which is handy to validate new rules.

## Configuration

`/etc/ananicy.conf` (key = value):

| key | default | meaning |
| --- | --- | --- |
| `check_freq` | `5` | seconds between scans (0.01..86400) |
| `type_load` / `rule_load` | `true` | load `.types` / `.rules` files |
| `apply_nice` | `true` | apply nice values |
| `apply_ionice` | `true` | apply I/O priorities |
| `apply_oom_score_adj` | `true` | apply OOM score adjustments |
| `log_applied_rule` | `false` | log every applied rule (debug) |
| `loglevel` | `info` | trace, debug, info, warn, error, critical |

`apply_latnice`, `apply_sched` and `cgroup` keys are recognized but ignored
(RamJet does not manage scheduler policies nor cgroups).

Rules (`/etc/ananicy.d/**/*.rules`, one JSON object per line):

- `name` — executable name (`/proc/<pid>/exe` basename)
- `type` — inherits fields from a type definition
- `nice` (`-20` to `19`)
- `io-class` or `ioclass` (`realtime`, `best-effort`, `idle`)
- `ionice` (`0` to `7`; without a class it defaults to `best-effort`)
- `oom_score_adj` (`-1000` to `1000`, applied per process)

Files are loaded in sorted order; when a rule or type name is defined several
times, the last definition wins and a warning is logged. Invalid field values
are ignored with a warning instead of dropping the whole rule.

## How the cache works

Each scan, a matched process is skipped when its `pid` + `starttime` are
already cached, it has not gained threads, and the cache has not expired.
Every `cache_ttl` seconds (default 60, `-T 0` disables) the cache is rebuilt:
dead PIDs are dropped and every priority is re-applied, so external changes
(e.g. a manual `renice`) are corrected within the TTL.

## Service files

Systemd:

```bash
sudo install -m 644 systemd/ramjet.service /etc/systemd/system/ramjet.service
sudo systemctl daemon-reload
sudo systemctl enable --now ramjet.service
```

For runit, place the `runit/` directory under the appropriate service directory.

## License

GPL-3.0, see [LICENSE](LICENSE).
