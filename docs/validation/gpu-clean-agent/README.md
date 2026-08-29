# GPU clean-agent acceptance

Horizon-A A5 is a two-party acceptance gate. A deterministic driver, a replay,
or an implementing agent correcting its own option does not satisfy it.

The checked-in `m3-a5-clean-agent-20260828.json` is only a schema-validated
`superseded-nonterminal` disposition for the former self-run receipt. It
preserves that file's historical provenance but explicitly cannot satisfy A5.
Only a separately named `pulp.gpu-clean-agent-verification.v2` receipt with
`status: independent-agent-accepted` and `acceptance_gate_satisfied: true` is
terminal evidence.

## State boundary

`gpu_clean_agent_journey.py` has three separate subcommands:

1. `prepare` creates an isolated public workspace and a private verifier case.
   The workspace contains `TASK.md` and an executable `run-probe.sh` with
   exactly one planted error: `seeded_option="--negative-control"`. Preparation
   privately selects the recipe from the symptom and records an authentic
   unmutated reference run. Its state is `awaiting-independent-agent`; it never
   emits acceptance.
2. `record` verifies that the workspace is pristine and launches one fresh
   Codex session. It uses `workspace-write`, a disposable HOME/config root,
   `--ephemeral`, `--ignore-user-config`, `--ignore-rules`, and a sanitized
   environment/PATH. The agent sees only the installed CLI, exact symptom,
   workspace, and public documentation. It receives neither a recipe ID nor a
   source checkout/private-plan path. The session remains nonterminal.
3. `verify` is external to the fresh agent. It checks the full prompt/tool/
   command transcript, model and session identity, cwd/PATH, installed binary
   and source/plan revisions, before/after tree hashes and exact one-line diff,
   unique negative/repaired/reference evidence IDs, typed diagnosis, adapter
   identity, and every artifact digest. Only this subcommand can create the
   terminal receipt.

Subprocess stdout, stderr, workspace, and artifact growth are capped while a
producer is running. A cap or timeout terminates the whole process group.
Symlink, special-file, path-escape, undeclared-artifact, unavailable,
unverified, untyped, replayed, or extra-edit evidence fails closed.

## Terminal independent run

Run this only after the harness commit is the shared source HEAD and the
canonical plan is committed. The person orchestrating the proof prepares and
verifies it; the `record` command itself launches the fresh no-context agent.
Do not give that agent this source checkout, the private case, a recipe ID, or
prior task context.

```sh
SOURCE_ROOT=/absolute/path/to/the/shared-pulp-worktree
PLAN_ROOT=/absolute/path/to/the/canonical-pulp-planning-worktree
PROOF_ROOT="$(mktemp -d /private/tmp/pulp-gpu-a5-independent.XXXXXX)"

cmake --build "$SOURCE_ROOT/build" --target pulp-cli
cmake -DCMAKE_INSTALL_PREFIX="$PROOF_ROOT/installed" \
  -P "$SOURCE_ROOT/build/tools/cli/cmake_install.cmake"
V8_RUNTIME_LIBRARY="$(sed -n 's/^V8_RUNTIME_LIBRARY:[^=]*=//p' "$SOURCE_ROOT/build/CMakeCache.txt")"
WEBGPU_RUNTIME_LIB="$(sed -n 's/^WEBGPU_RUNTIME_LIB:[^=]*=//p' "$SOURCE_ROOT/build/CMakeCache.txt")"
mkdir -p "$PROOF_ROOT/installed/lib"
install -m 0644 "$V8_RUNTIME_LIBRARY" "$WEBGPU_RUNTIME_LIB" "$PROOF_ROOT/installed/lib/"
install -m 0755 "$PROOF_ROOT/installed/bin/pulp-cpp" "$PROOF_ROOT/installed/bin/pulp"

SOURCE_REVISION="$(git -C "$SOURCE_ROOT" rev-parse HEAD)"
PLAN_REVISION="$(git -C "$PLAN_ROOT" rev-parse HEAD)"
HARNESS="$SOURCE_ROOT/tools/scripts/gpu_clean_agent_journey.py"

python3 "$HARNESS" prepare \
  --pulp "$PROOF_ROOT/installed/bin/pulp" \
  --symptom compute-readback-mismatch \
  --workspace "$PROOF_ROOT/workspace" \
  --case-dir "$PROOF_ROOT/private-case" \
  --source-revision "$SOURCE_REVISION" \
  --plan-revision "$PLAN_REVISION" \
  --forbidden-root "$SOURCE_ROOT" \
  --forbidden-root "$PLAN_ROOT"

python3 "$HARNESS" record \
  --case "$PROOF_ROOT/private-case/case.json" \
  --agent-bin "$(command -v codex)" \
  --model gpt-5.6-sol

python3 "$HARNESS" verify \
  --case "$PROOF_ROOT/private-case/case.json" \
  --session "$PROOF_ROOT/private-case/agent-session.json" \
  --receipt "$SOURCE_ROOT/docs/validation/gpu-clean-agent/m3-a5-clean-agent-independent-20260828.json"
```

Preserve the private case, agent transcript, and workspace alongside the
terminal receipt until their hashes have been reviewed. CTest exercises only
the nonterminal `prepare` process contract under
`cli-gpu-clean-agent-preparer-contract`; it is intentionally incapable of
claiming A5 acceptance.
