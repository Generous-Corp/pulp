# A9 edit and replay evidence — BLOCKED

Status: **BLOCKED**. This is an evidence-only checkpoint from
`origin/main` at `a2e153a89d4c3884fa0e40160b4e392fb4c0f8f4`. No runtime, API,
source, or fixture changes were made.

## Fixture discovery

The current checkout contains two bounded public-host consumers:

* `examples/graph-agent-loop` — the existing author/save/reload/edit/replay
  consumer with planted network, plugin, and missing-action controls.
* `examples/graph-agent-loop-replay` — the separate stereo semantic fixture
  with positive, wrong-edit, and wrong-replay controls.

The replay fixture is present and readable. Its immutable input and plan
digests are:

| file | bytes | SHA-256 |
| --- | ---: | --- |
| `examples/graph-agent-loop-replay/fixtures/stereo-semantic.json` | 3179 | `61ae73c1280ad2fcf8a929fd37854af7060502bccf4a45f411445b28934f51ac` |
| `examples/graph-agent-loop-replay/fixtures/replay-plan.json` | 172 | `13a43a176664f9e4701a257417da9c9a8d58be9845a771638538f9c4ea0d4df5` |
| `examples/graph-agent-loop-replay/fixtures/replay-plan-wrong-edit.json` | 174 | `0d04249f6480a5b03fe91186a5aceb65634a682d84e8a8b06c217b8a4b4fbf63` |
| `examples/graph-agent-loop-replay/fixtures/replay-plan-wrong-replay.json` | 176 | `22af76935417e1c55a961acbba510ed76bd064d26cc5e1798d3a891e91f30708` |

These are source fixture digests; the source fixture itself is not a CLI/MCP
publication receipt.

## Blocking evidence

The required two-consumer execution could not start because this fresh
worktree has no build tree or exact-head installed SDK. The source project is
version `1.119.0` (`CMakeLists.txt:10`), while the available user SDK config
reports `PULP_VERSION "0.305.0"`.

Commands and results:

```text
cmake -S examples/graph-agent-loop-replay -B build/a9-edit-replay-2 \
  -DCMAKE_BUILD_TYPE=Release \
  -DPulp_DIR=/Users/danielraffel/.pulp/lib/cmake/Pulp
...
Found package configuration file .../PulpConfig.cmake
but it set Pulp_FOUND to FALSE

cmake -S examples/graph-agent-loop-replay -B build/a9-edit-replay-823 \
  -DCMAKE_BUILD_TYPE=Release \
  -DPulp_DIR=/Users/danielraffel/.pulp/sdk-build/darwin-arm64-0.823.0
...
include could not find requested file: PulpAAX.cmake,
PulpSdkProvenance.cmake, PulpPlatformConfig.cmake, PulpMinOs.cmake,
PulpPkgConfigImports.cmake, PulpLinkFontconfig.cmake, PulpSdkGuards.cmake,
PulpWebGpuImportedTarget.cmake, PulpRuntimeStaging.cmake,
PulpBundleRelocatable.cmake, PulpTargets.cmake, and PulpUtils.cmake
...
but it set Pulp_FOUND to FALSE
```

Because `find_package(Pulp CONFIG REQUIRED COMPONENTS host)` does not resolve,
the executable was not compiled and CTest did not run. Therefore there are no
semantic/target-bound receipts, render outputs, output hashes, or planted
positive/negative control results to report. The checked-in fixture alone does
not establish CLI/MCP publication, installed-SDK provenance, or two-consumer
parity.

## Next admissible step

Provide a complete exact-head SDK (including its companion CMake modules) or a
source Release build installed from this commit, then configure both consumers
and run their complete CTest suites. Until that owner/provenance is available,
do not infer A9 completion or add a second scheduler/runtime abstraction.
