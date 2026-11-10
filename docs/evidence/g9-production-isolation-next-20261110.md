# G-9 production isolation qualification — 2026-11-10

Status: **PARTIAL / HOLD**.

This audit is bound to exact `origin/main` source `b8a8278c9b3c876370fe921ddbbf7434030335ae`.
It deliberately adds no hosted-plugin runtime or speculative isolation API.

## Fresh hostile controls

The existing Darwin hostile fixture was run from this exact checkout:

```text
python3 tools/validation/g9_hostile_fixture.py --output /tmp/g9-next-20261110-1794302696 --timeout 0.5
python3 tools/validation/test_g9_hostile_fixture.py
```

The fixture reported all eight controls passing: crash, hang, deadline,
descendant-parent timeout, descendant survival, fixture cleanup, clean
negative, and slow-within-budget negative. The four-test Python regression
suite also passed. Receipt SHA-256:
`5453b23ef06863fdd02bd9bb802b8e981d2bfd644ffe24068741333e7dfbe415`.

The result remains a hostile-process observation. The receipt reports
`isolation_implemented=false`; the fixture itself terminates the surviving
child during cleanup. It is not evidence that a production hosted plugin is
isolated.

## Current implementation audit

| Requirement | Verified implementation | Disposition |
| --- | --- | --- |
| Hosted audio worker ownership | `PluginSlot::load()` remains in the caller process; `IsolatedPluginScanner` isolates discovery only. | HOLD |
| Load/process crash, hang, and deadline containment | No hosted audio worker equivalent to `pulp-scan-worker` exists. | HOLD |
| Callback IPC and realtime allocation contract | No production hosted-plugin callback transport or measured allocation/lock/I/O matrix exists. | NOT CHECKED |
| Restart, state migration, and recovery | No hosted-worker restart/state contract or fixture exists. | NOT CHECKED |
| Sibling continuity after worker failure | No graph fixture proves surviving hosted siblings continue safely. | NOT CHECKED |
| POSIX descendant cleanup | `ChildProcess` private process-group cleanup has positive helper tests, but this is helper evidence only. | PARTIAL |
| Windows descendant cleanup | Windows `ChildProcess` uses direct `TerminateProcess`; no Job Object/tree contract is present. | HOLD |
| Cross-platform hosted-plugin parity | No Linux/Windows production hosted-plugin lifecycle fixture exists. | NOT CHECKED |

## Decision and next action

No production implementation is justified by this evidence. The smallest
next design step is a separately reviewed hosted-worker contract defining
ownership, callback transport, deadlines, crash reporting, restart/state
semantics, sibling continuity, and platform-specific descendant cleanup. Each
contract item needs a real hosted-plugin fixture with a positive control and an
independently planted negative. Until those gates exist, G-9 remains
**PARTIAL / HOLD** and must not be promoted to production isolation.
