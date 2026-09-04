# RamJet

RamJet is a small C daemon inspired by Rice/Ananicy. It scans `/proc` and applies Ananicy-style `.rules`, `.types` and `.cgroups` definitions.

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
- `cgroup`
- `oom_score_adj` (`-1000` to `1000`)

Types support the same scheduling fields and are inherited by rules when a rule does not define the corresponding value itself.

## Cgroups

Cgroup handling remains intentionally limited to the legacy v1 CPU controller at `/sys/fs/cgroup/cpu`. On systems using cgroup v2 only, RamJet starts normally but reports cgroups as unavailable.

## Service files

Systemd:

```bash
sudo install -m 644 systemd/ramjet.service /etc/systemd/system/ramjet.service
sudo systemctl daemon-reload
sudo systemctl enable --now ramjet.service
```

For runit, place the `runit/` directory under the appropriate service directory.

## Changes in this version

- Fixed Linux `ionice` class numbers.
- Fixed the valid `nice` range.
- Stopped processing the main thread twice.
- Hardened `/proc` PID parsing against malformed/overflowing directory names.
- Propagated scheduling/application errors instead of always returning success.
- Added `oom_score_adj` application and inheritance.
- Replaced shell-based `ionice` invocation with direct `execvp`.
- Removed the bundled cJSON dependency in favor of a small parser for the flat Ananicy JSON objects RamJet consumes.
- Made duplicate rule/type/cgroup definitions replace previous entries deterministically.
- Prevented shutdown from deleting pre-existing cgroups.
- Rejected unsafe cgroup names containing path separators.
- Fixed stale systemd/runit paths and arguments.
