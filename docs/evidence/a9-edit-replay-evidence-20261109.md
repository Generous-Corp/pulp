# A9 edit and replay evidence

Date: 2026-11-09

Worktree: `codex/a9-edit-replay-20261109`

Base source revision: `cc6430e25529e3c3f511c3f33178975ac58fc05d`

## Scope

`examples/graph-agent-loop-replay` is a standalone installed-SDK consumer of
the existing public `Pulp::host` API. It saves and reloads a stereo
Input → Gain → Gain → Output graph with plugin loading disabled, renders a
fixed input, applies one gain edit, and replays the saved baseline.

The fixture checks a topology-only semantic fingerprint and the resolved
prepared-description `content_hash()`. The semantic fingerprint remains equal
for the edited graph and the replayed baseline. The resolved plan changes for
the edit and returns to the baseline value on replay.

## Source-built SDK provenance

The source was configured as a Release build with examples and tests disabled,
then built and installed through the governed build wrapper:

```text
tools/ci/governed-build.sh cmake --build build-a9-edit --target pulp-host
tools/ci/governed-build.sh cmake --build build-a9-edit --target install
```

The installed package provenance is:

```text
source_git_sha: cc6430e25529e3c3f511c3f33178975ac58fc05d
source_git_dirty: false
platform: darwin-arm64
build_profile: dev
```

The standalone consumer was configured with that installed prefix and built
through the governed wrapper. The installed source-built SDK consumer is
**PASS**.

The direct source-tree consumer lane is **NOT CHECKED**. No source-tree CMake
hook was retained because the attempted hook caused recursive subproject
generation.

## Receipt

Positive run:

```text
graph-agent-loop-replay PASS checks=89
baseline_semantic=de2bdb2d0a7a156c
baseline_resolved_plan=b5503ff9c381e323e4a0a17232e2ac2f58e78b2340de52bf99bede804ff4a92c
edited_resolved_plan=525bd7fb6344a91447da0de98ea740e87b4cead288e41dd7908c6cfcf8bfbf12
replay_resolved_plan=b5503ff9c381e323e4a0a17232e2ac2f58e78b2340de52bf99bede804ff4a92c
```

CTest ran all three registered checks:

```text
100% tests passed, 0 tests failed out of 3
```

The planted-negative controls passed by rejecting the expected mutations:

```text
graph-agent-loop-replay-wrong-edit   PASS (expected refusal: wrong-edit-oracle)
graph-agent-loop-replay-wrong-replay PASS (expected refusal: wrong-replay-input)
```

## Repository gate status

`tools/scripts/gates.sh origin/main` reached the repository checks. The build
gate, source selftest, workflow lint, pr-fast CTest, and registered Python
lanes were **NOT CHECKED** because this host is marked
`TARTCI_ROLE=dedicated-builder`; the gate directs those suites to a light
host or disposable VM. The script-input drift lanes were also **NOT CHECKED**
locally because this standalone SDK build is not a source-tree gate configure.

The actionable local checks are clean: the changed C++ lines pass
`format_changed.sh`, and `examples_index_guard.py` reports every example
directory listed exactly once.

Artifact SHA-256 values from the installed-prefix consumer build:

```text
build/a9-edit-replay/pulp-graph-agent-loop-replay
7c86839b5156e373755b140149026b45b90e9be834d7bafe8abad738eedcce0b

examples/graph-agent-loop-replay/fixtures/stereo-semantic.json
61ae73c1280ad2fcf8a929fd37854af7060502bccf4a45f411445b28934f51ac

examples/graph-agent-loop-replay/fixtures/replay-plan.json
13a43a176664f9e4701a257417da9c9a8d58be9845a771638538f9c4ea0d4df5
```

## Boundaries

This evidence covers the admitted prepared I/O + Gain profile and the
save/edit/replay fixture. It does not establish whole-graph identity, A9
roadmap completion, model execution, CLI or MCP parity, arbitrary plugin or
node-family support, realtime behavior, or browser/native parity. Those lanes
remain **NOT CHECKED** here.

## Current-main reconciliation

The branch now contains a non-rebased merge of `origin/main`
(`04a6852f08e68c252a890c4eba0ded73c73adedd`). The installed-SDK receipt above
remains historical evidence from source revision
`cc6430e25529e3c3f511c3f33178975ac58fc05d`; it was not rerun after this merge.
A current-main Release build of `pulp-host` completed, but SDK installation
stopped at the missing optional `inspect/pulp-control-broker` artifact. The
current-main standalone consumer lane is therefore **NOT CHECKED**.
