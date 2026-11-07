# A9 installed Release-profile SDK candidate qualification

Status: **QUALIFIED CANDIDATE / NOT AN OFFICIAL RELEASE**

This receipt is for PR 11728 exact head `d7171772851c6486a5645ddb31c6f6788b0a9ad6`.
The receipt is intentionally uncommitted so the PR head and SDK source binding
remain exact; committing it requires a new SDK source binding and rerun.

## Source and candidate SDK

- Consumer source: immutable external copy of `examples/graph-agent-loop/**` from
  `d7171772851c6486a5645ddb31c6f6788b0a9ad6`.
- SDK source: m1 worktree
  `/Users/danielraffel/agent-worktrees/pulp-a9-sdk-qualification-20261107`,
  exact `HEAD=d7171772851c6486a5645ddb31c6f6788b0a9ad6`, clean.
- SDK build: `build-a9-sdk-inspector`, `CMAKE_BUILD_TYPE=Release`,
  `PULP_PROFILE=release`, `PULP_ENABLE_INSPECTOR=ON`, GPU disabled for this
  host candidate (`PULP_ENABLE_GPU=OFF`, `PULP_REQUIRE_GPU_FOR_SDK=OFF`).
- Installed prefix: `sdk-staging-a9-inspector`; `version.txt` is `1.83.0`,
  `sdk_build_type.txt` is `Release`.
- Candidate marker: `kind=source-install`, `build_profile=release`,
  `distribution_eligible=false`, source SHA exact. Marker SHA-256:
  `fad1bdac110e9e6183aa4aa0563b1d01bfced7cf5a0ca93580afa86c02edc48b`.
- Archive: `pulp-sdk-a9-d717177-darwin-arm64.tar.gz`, 53,303,743 bytes,
  SHA-256 `192730fc704519a71a31284841311ecf07ceadf1a24819289c22c1c14f13cba2`.
- Installed build header SHA-256:
  `b8115c6398166a760160e259ea232731dd56e7f97b94c160f2f460954be89634`.
- Installed `PulpConfig.cmake` SHA-256:
  `2d039e796fcf39fc6691cdc3ef5a6fb45da847eded5690ae56e999599d0ccb8d`.

The supported `sdk_development_provenance.py check` passed and recorded the
candidate metadata, cache SHA-256 `d9ac088c2d3dc718088a5e67d7cc4d2f77057bff71303c4bda461f70ab62a12b`,
and GPU archive identity SHA-256
`8e6050456784f562a1c031b78bd2db904bbe1b0c67c412cdb4050e4d6c203161`.

## External consumer proof

External consumer build directory:
`/Users/danielraffel/agent-worktrees/pulp-a9-sdk-consumer-20261107`.

Configure, governed build, and test commands:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/Users/danielraffel/agent-worktrees/pulp-a9-sdk-qualification-20261107/sdk-staging-a9-inspector \
  -DCMAKE_BUILD_TYPE=Release
PULP_BUILD_JOBS=3 /Users/danielraffel/agent-worktrees/pulp-a9-sdk-qualification-20261107/tools/ci/governed-build.sh \
  cmake --build build
ctest --test-dir build --output-on-failure
```

CTest result: **4/4 passed** — positive consumer plus the three `WILL_FAIL`
controls (`network-allowed`, `plugin-allowed`, `missing-action`). The direct
controls returned nonzero and printed `graph-agent-loop FAIL rule=plan-schema`
for network/plugin opt-in and `graph-agent-loop FAIL rule=model-pin` for the
missing-action fixture. No control passed because of an unrelated crash or
missing executable.

Positive receipt:

```text
graph-agent-loop PASS checks=66 catalog_nodes=89 baseline_blocks=3 edited_gain=0.25 plan_digest=ea6f2355694122f0 semantic_fingerprint=7368e020052b8aa2 network=denied ambient_plugins=denied
```

The produced baseline and edited `.pulpgraph` files had SHA-256
`179f37462d0c05b90c5b80af4e51093bdca5182ee6911ceb30d8690cfb9154bc` and
`d2318eb570640bed246bcebb49eb521d4387f16cd6214a0e32f7f38aa915481d`, with
only `gain: 0.5` → `gain: 0.25` changing. The action log SHA-256 is
`ab9c21937433a7e14a1005bbc9dc44886b5a9119d3fae375099d00be01ff4949`.

Consumer executable SHA-256:
`4963147b39fa66a7a038509403e36b2767e530a2964d2306c4e9211333e9ffb6`.
The generated Release rules contain `-O3 -DNDEBUG`; the link line names the
installed SDK libraries under `sdk-staging-a9-inspector/lib`.

## Provenance correction and limits

The official `sdk_provenance.py stamp` path was not used. It requires a real
`v1.83.0` tag resolving to the candidate SHA. Upstream `v1.83.0` is annotated
object `c3b086b504a14aa6d51ae717d1dc5c28f7f84e13`, peeled to
`87a6adc4c45167bf444afa16d891186deb8dbd99`, so it cannot identify this PR.
An accidental local synthetic tag was deleted; no tag was pushed and no
official release marker was generated. The candidate worktree is linked to
common Git dir `/Users/danielraffel/Code/pulp/.git`; the verified upstream tag
is retained only in the separate fresh clone
`/Users/danielraffel/agent-worktrees/pulp-a9-upstream-tag-verification-20261107`.

This proves the bounded installed Release-profile host consumer and its planted
denials/mutation oracles. It does not prove an official published SDK,
GPU-provider execution, model execution, network isolation outside the
consumer, realtime behavior, CLI/MCP parity, or full A9/A10 completion.
