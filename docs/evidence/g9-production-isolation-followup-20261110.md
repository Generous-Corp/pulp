# G-9 production isolation follow-up — 2026-11-10

Status: **PARTIAL / HOLD — production hosted-plugin containment remains unproven**.

This follow-up is bound to `origin/main` at
`f4022ec16f273d90c232faaedef596505661b707` and records a fresh audit of the
remaining G-9 gates. It does not add a hosted-plugin runtime, callback IPC,
restart protocol, or speculative platform abstraction.

## Verified current baseline

The hostile fixture was run from this exact source tree on Darwin with a
0.5-second budget:

```text
python3 tools/validation/g9_hostile_fixture.py \
  --output /tmp/g9-audit2-20261109203034 --timeout 0.5
python3 tools/validation/test_g9_hostile_fixture.py
```

The fixture passed all eight controls (crash, hang, deadline, descendant
survival, fixture cleanup, clean negative, and slow-within-budget negative).
The receipt is local run output and is retained here by its digest only:

```text
receipt: /tmp/g9-audit2-20261109203034/receipt.json
sha256: f9c3c72d840b0752b2dc564585255bb969c6c34a820548543b471edd620fd6df
```

The positive observation is still that the hostile parent timed out and exited
while its child remained in Darwin state `S`; the fixture then sent SIGTERM and
verified that the child disappeared. The receipt reports
`isolation_implemented=false`. This is a hostile process observation, not a
production hosted-plugin containment proof.

The Python regression suite ran four tests and passed. The existing C++ POSIX
process-group test at `test/test_runtime_utils.cpp:1194-1232` proves that the
Pulp `ChildProcess` timeout path removes a real shell descendant on macOS and
Linux. That helper cleanup is useful infrastructure, but it does not turn
`PluginSlot::load()` into a worker process or provide plugin callback transport.

## Remaining production gaps

| Gate | Current evidence | Disposition |
| --- | --- | --- |
| Graph-native hosted-plugin worker ownership | `PluginSlot::load()` remains in the caller process; the hosting guide explicitly says discovery isolation does not cover load or audio processing (`docs/guides/hosting.md:66-78`). | **HOLD** |
| Crash/hang/deadline containment during load/process | `IsolatedPluginScanner` uses `ChildProcess` only for `pulp-scan-worker` discovery (`core/host/src/isolated_scanner.cpp:243-313`). No equivalent hosted audio worker exists. | **HOLD** |
| Callback IPC and allocation behavior | No production hosted-plugin callback channel or bounded audio IPC contract is present. | **NOT CHECKED** |
| Restart and state recovery | No hosted-worker restart/state migration contract or positive/negative fixture exists. | **NOT CHECKED** |
| Sibling continuity | No multi-node hosted-worker failure fixture proves surviving graph siblings continue safely. | **NOT CHECKED** |
| Windows descendant cleanup | `core/platform/platform/win/child_process_win.cpp:526-547` and `616-627` call `TerminateProcess` on the direct process handle. No Job Object or descendant-tree contract is present. | **HOLD** |
| Linux/Darwin helper cleanup | POSIX process groups are implemented and the descendant test is positive, but this is helper-level evidence only. | **PARTIAL** |
| Cross-platform hosted-plugin parity | No Linux/Windows production hosted-plugin lifecycle fixture exists. | **NOT CHECKED** |

## Decision

No production implementation is justified by the current evidence. The
smallest defensible next slice is a separately reviewed hosted-worker contract
that names ownership, callback transport, deadline semantics, crash reporting,
restart/state behavior, sibling continuity, and platform-specific descendant
cleanup. Each item needs a real hosted-plugin fixture with a positive control and
an independently planted negative. Until that contract and fixtures exist,
G-9 must remain **PARTIAL / HOLD** and must not be promoted from the hostile
process fixture to a graph isolation claim.
