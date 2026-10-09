# SignalGraph lifecycle D1 current-main TSan receipt (2026-10-07)

This receipt refreshes the SignalGraph lifecycle and publication evidence on
current `origin/main`. It is validation evidence only: no production source,
public header, test source, or CTest manifest was changed for this packet.

## Provenance

- Protected base: `origin/main` at
  [`c4ed69529c9df393d65475ff9790d4eb4067fb38`](https://github.com/Generous-Corp/pulp/commit/c4ed69529c9df393d65475ff9790d4eb4067fb38)
- Worktree: `/Users/danielraffel/Code/pulp-signalgraph-next-20261007`
- Configure directory: `build-tsan-d1-current`
- Configuration: `Debug`, `PULP_SANITIZER=thread`, tests enabled, examples and
  GPU disabled.

## Build receipts

Both targets were built through the governed wrapper:

```bash
tools/ci/governed-build.sh cmake --build build-tsan-d1-current \
  --target pulp-test-host-signal-graph
tools/ci/governed-build.sh cmake --build build-tsan-d1-current \
  --target pulp-test-signal-graph-executor-parity
```

| Binary | Size | SHA-256 |
| --- | ---: | --- |
| `build-tsan-d1-current/test/pulp-test-host-signal-graph` | 32,603,848 bytes | `cda46121fe28caf1b654ce4399121563ff9c3c7e7c098d12040cd4fcee262c8d` |
| `build-tsan-d1-current/test/pulp-test-signal-graph-executor-parity` | 26,644,968 bytes | `aaea80e7c1255dc2a563005370ad2fc2678dbb85c52f9c1ad38574fb3a4c8bc7` |

## TSan runs

Each run used `TSAN_OPTIONS=halt_on_error=1:history_size=7:suppressions=$PWD/test/tsan.supp`.
No ThreadSanitizer report was emitted.

| Scope | Command/filter | Result |
| --- | --- | ---: |
| Host lifecycle/full graph | `pulp-test-host-signal-graph --reporter compact` | **11,935 assertions / 132 cases passed** |
| Executor/parity full suite | `pulp-test-signal-graph-executor-parity --reporter compact` | **48,378 assertions / 62 cases passed** |
| Host race-tagged | `pulp-test-host-signal-graph '[race]' --reporter compact` | **60 assertions / 8 cases passed** |
| Host live-swap | `pulp-test-host-signal-graph '[live-swap]' --reporter compact` | **107 assertions / 3 cases passed** |
| Executor reprepare thread cases | `pulp-test-signal-graph-executor-parity '[threads]' --reporter compact` | **52 assertions / 2 cases passed** |

The full host run includes the release/reader-drain, control-reader
retirement, node-field serialization, MIDI/parameter mailbox, and prepared
publication race cases. The parity run includes both serial and parallel
re-prepare-against-render cases.

## CTest enumeration

After both binaries were built:

```bash
ctest --test-dir build-tsan-d1-current -C Debug -N
ctest --test-dir build-tsan-d1-current -C Debug -N -R \
  'SignalGraph|Translated routing|Parallel routing|Standalone translated routing|Lowerable Custom|Routed PDC'
```

The focused CTest expression discovered **180 cases**. The narrower
`-R '^SignalGraph'` expression discovered **150 cases**. The Catch2 executables
reported 132 host cases and 62 parity cases, respectively; no test registration
was added or removed by this receipt.

## Interpretation

This is a current-main concurrency and parity refresh. It does not establish a
new production refactor or hardware/provider capability. The existing
publication/reader-pin and live-swap implementation remains TSan-clean under
the listed deterministic cases at the recorded protected-main SHA.

## Repository gate

`tools/scripts/gates.sh origin/main` completed **PASSED WITH 24 NOT CHECKED**:
75 diff-scoped source-selftests passed and all applicable static/format,
lineage, dependency, manifest, and policy checks passed. The local TSan build
uses Unix Makefiles, so the gate did not run the build-dependent pr-fast
members; those are listed explicitly by the gate and remain CI-owned. This
receipt therefore makes no full-gate claim.
