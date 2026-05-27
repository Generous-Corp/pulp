# Linux Real-Time Thread Strategy

Status: **stable contract** (documented 2026-05-26 against
`core/audio/include/pulp/audio/workgroup.hpp`).

## Summary

On Linux, Pulp's `AudioWorkgroup::set_high_priority()` / `set_realtime_priority()`
deliberately use **`SCHED_OTHER` with elevated `pthread_setschedparam` priority**
rather than `SCHED_FIFO` or `SCHED_RR`. The decision is documented inline
in `core/audio/include/pulp/audio/workgroup.hpp` (line 127):

```cpp
// Don't use SCHED_FIFO as it requires root — use elevated normal priority
int policy = SCHED_OTHER;
```

This document records *why* that is the default, when the trade-off bites,
and the opt-in path for users who want hard real-time scheduling.

## Why SCHED_OTHER is the default

| Concern                | SCHED_OTHER (today)                 | SCHED_FIFO (alternative)            |
|------------------------|-------------------------------------|-------------------------------------|
| Capability required    | None                                | `CAP_SYS_NICE` or root              |
| Distro / install path  | Works out-of-box on every distro    | Requires `/etc/security/limits.conf` edits + `rtirq` / `rtkit` |
| Plugin host model      | Host owns the priority decision     | Plugin attempts to elevate above host |
| Worst-case behavior    | Audio dropout under heavy load      | Process starvation / unkillable RT loop on a bug |
| CI portability         | Same code path in CI and prod        | CI containers reject RT scheduling   |

Pulp targets the **MIT-licensed, no-install** experience first. A plugin
that calls `sched_setscheduler(SCHED_FIFO)` and fails opens a confusing
errno path; a plugin that asks for `RLIMIT_RTPRIO` it doesn't have is
worse than one that runs at normal priority. `SCHED_OTHER` is the
"works on every laptop" contract.

The trade-off: under heavy CPU contention (kernel build, browser tabs,
video encode), a Pulp plugin running on `SCHED_OTHER` can produce
xruns. Users who care about hard real-time on Linux are expected to
run a low-latency kernel (`linux-lowlatency`, `linux-rt`) and pair it
with the opt-in path below.

## Opt-in: SCHED_FIFO / RtKit for power users

There is no public API today. The opt-in lane is the user's
responsibility — Pulp will join whatever priority class the audio
callback thread already has.

Recommended approaches (in increasing order of effort):

1. **JACK / PipeWire**: launch Pulp's standalone host or your DAW
   under a session that already gives the audio thread `SCHED_FIFO`
   via the daemon. Pulp's `set_high_priority()` becomes a no-op
   because the thread is already RT.
2. **RtKit (DBus)**: bind to `org.freedesktop.RealtimeKit1` and call
   `MakeThreadRealtime` from the audio callback. Requires the
   `rtkit-daemon` package and a desktop session. Pulp does not bundle
   an RtKit client — wire it from the application layer if you need it.
3. **CAP_SYS_NICE on the binary**: `sudo setcap cap_sys_nice+ep /path/to/pulp`
   then call `pthread_setschedparam(..., SCHED_FIFO, &param)` from your
   own application code before `AudioWorkgroup::join_from_audio_thread()`.

If a future Pulp release adds an opt-in surface, it will land as a
constructor argument on `AudioWorkgroup` (`AudioWorkgroup(LinuxRtPolicy)`)
or a free function in `pulp::audio::linux_rt`. The default will remain
`SCHED_OTHER` — opt-in not opt-out.

## What this means for plugin authors

- On Linux, **assume `SCHED_OTHER`**. Do not allocate, take locks, or
  block on file I/O in `process()` — the same RT-safety rules apply
  whether the kernel is honouring an FIFO priority or not.
- The macOS / iOS path (`os_workgroup_join` + Mach time-constraint
  policy) and the Linux path are deliberately asymmetric. macOS gives
  you a real workgroup; Linux gives you a polite request.
- If your plugin is missing deadlines on Linux but holding up fine on
  macOS, the answer is probably "the user needs RtKit or a low-latency
  kernel", not "Pulp needs a code change".

## Gap-doc cross-reference

This file closes the docs-only deliverable for the gap-doc row:

> **Document Pulp's Linux RT-thread strategy** —
> `core/audio/include/pulp/audio/workgroup.hpp` explicitly elects
> **SCHED_OTHER** ("Don't use SCHED_FIFO as it requires root") with
> elevated nice. Decide whether to add an opt-in `SCHED_FIFO` /
> RtKit path for users running with `CAP_SYS_NICE` or rtirq, or
> keep SCHED_OTHER as the only Linux RT story.

Decision: **keep `SCHED_OTHER` as the default**, document the opt-in
recipe, and revisit only if a real Pulp-on-Linux user files an issue
with a reproducible xrun trace that an RtKit upgrade would fix.

See also: `core/audio/include/pulp/audio/workgroup.hpp` (the contract),
`apple/audio/workgroup.swift` (the Apple counterpart),
`planning/2026-05-24-reference-framework-gap-analysis.md` (the row this
doc closes).
