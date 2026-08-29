# GPU clean-agent acceptance

Horizon-A A5 is a two-party acceptance gate. A deterministic driver, a replay,
an implementing agent correcting its own option, or an unsigned handcrafted
session does not satisfy it.

The checked-in `m3-a5-clean-agent-20260828.json` remains the unchanged
`superseded-nonterminal` disposition for the former self-run receipt. Its
historical replacement field names the earlier v2 design; it cannot satisfy
A5. The hardened terminal pair uses a new filename, a
`pulp.gpu-clean-agent-verification.v3` receipt, and a
`pulp.gpu-clean-agent-audit-bundle.v1` bundle.

## State and trust boundary

`gpu_clean_agent_journey.py` has three separate subcommands:

1. `prepare` derives the exact source and plan revisions from clean Git
   repositories with their canonical GitHub origins; plan HEAD must equal
   `origin/main`. It binds the committed plan document blob, Release CMake home
   and generated build stamp, a fresh replay of the CLI install script, the
   installed `pulp`/`pulp-cpp` bytes, and Git-blob-identical installed catalog
   and documentation. Caller-supplied revision labels are not accepted. It
   creates the planted workspace, private reference, fresh nonce, and one-use
   record-signing key. Its state is `awaiting-independent-agent`.
2. `record` accepts only the native hardened-runtime `codex` signed by OpenAI
   team `2DC432GLL2`. It launches one non-ephemeral session with a disposable
   HOME/CODEX_HOME, closed JSON output schema, and sanitized environment. One
   outer macOS Seatbelt profile is the OS authority: source, build, plan, and
   private-case reads are denied; writes are allowed only in the workspace and
   disposable runtime. Direct, `../` relative, symlink read traversal, and
   non-allowlisted writes are planted controls. `/usr/local` and site-wide
   `/Library/Application Support` are explicit ambient-read negatives. Codex receives
   `--sandbox danger-full-access` only because macOS refuses a nested Seatbelt
   policy; the outer profile is already active before Codex starts and remains
   the filesystem boundary. Reads default-deny outside the exact workspace,
   install, disposable runtime, exact signed Codex executable, and narrowly named macOS
   system roots, so unrelated checkouts are not ambiently visible. `record`
   places the minimum compact auth payload in a random-CODEX_HOME-scoped macOS
   Keychain item, removes creator access, ACLs retrieval to the verified Codex
   executable, launches with the direct `keyring` backend, and deletes the item
   before publishing the session. No readable `auth.json` is created. Shell
   tools inherit a closed environment that excludes CODEX_HOME, authentication,
   and proxy variables. Direct outbound sockets are denied by Seatbelt; Codex
   reaches only an auth-mode-specific host allowlist through a bounded
   recorder-owned loopback CONNECT proxy. The proxy rejects non-CONNECT,
   non-443, unlisted-host, connection-count, idle, and byte-cap violations and
   records host/byte outcomes without TLS payloads. `record` joins the stdout
   thread UUID to Codex's persisted session JSONL, signs both the complete
   private session core and its deterministic placeholder-redacted form,
   destroys the one-use private key, and remains nonterminal.
3. `verify` re-derives every provenance and byte identity, verifies the record
   signature, official Codex identity, saved-session join, nonce, Seatbelt
   profile/controls, installed catalog/docs reads, exact negative/edit/repaired
   sequence, evidence IDs, adapter, tree, and artifact digests. It writes the
   terminal receipt and a bounded redacted audit bundle transaction in this
   directory. The receipt is the commit marker: an exact bundle left staged by
   interruption can be resumed, a mismatched bundle is rejected, and an
   ordinary receipt-write failure rolls back a newly staged bundle. Absolute source, plan, build, private-case, workspace, install,
   runtime, and Codex paths are replaced with stable placeholders.

`record` and `verify` additionally require both source and plan HEADs to carry
Daniel's valid authorized SSH commit-signing fingerprint. The nonterminal CTest
plan fixture deliberately cannot cross that terminal authority gate.

The durable bundle is deliberately nonterminal until the terminal receipt
binds its digest. It contains the bounded stdout JSONL events, structured final
message, redacted case/session records, public Codex rollout metadata, command
records, exact tree/diff, and evidence summaries. It intentionally excludes
authentication and private-key bytes. The local prepare/record orchestrator is
still a trust root: Codex sessions are not remotely signed by the service. The
bundle includes the public key and the second signature, so its redacted
session remains independently verifiable after private paths are discarded;
the original signature still protects the private forensic record. These
one-use signatures prevent later file substitution; neither is claimed as
server-side attestation.

The exact-host proxy is stronger than the plan's “do not use network
documentation” requirement, but it is not a process-identity firewall: Codex
and its descendants inherit one Seatbelt policy. A descendant could open the
same loopback proxy, but only toward the recorded Codex transport/auth hosts;
it cannot make a direct or arbitrary-host connection and it cannot read the
keychain credential. The transcript verifier separately rejects web-search
events and observable network client commands. This is the explicit residual
boundary, not a claim that macOS offers process-qualified descendant rules.

Subprocess stdout, stderr, persistent session JSONL, workspace, artifacts,
event counts, nesting, strings, and durable output bytes are capped. Timeout,
symlink, special-file, hard-link, path escape, dirty Git state, altered install
bytes, unavailable evidence, replay, undocumented discovery, network-doc use,
extra edits, or cap breaches fail closed.

## Terminal independent run

Run this only after the harness commit is the shared source HEAD and the
canonical plan is committed. The person orchestrating the proof prepares and
verifies it; `record` launches the fresh no-context agent. This hardening change
does not run or self-certify that terminal proof.

```sh
SOURCE_ROOT=/absolute/path/to/the-shared-clean-pulp-worktree
BUILD_ROOT="$SOURCE_ROOT/build"
PLAN_ROOT=/absolute/path/to/the-clean-pulp-planning-worktree
PLAN_DOCUMENT=research/2026-08-27-vgpu-gpu-ux-inspiration-audit-and-plan.md
PROOF_ROOT="$(mktemp -d /private/tmp/pulp-gpu-a5-independent.XXXXXX)"

cmake --build "$BUILD_ROOT" --target pulp-cli
cmake -DCMAKE_INSTALL_PREFIX="$PROOF_ROOT/installed" \
  -P "$BUILD_ROOT/tools/cli/cmake_install.cmake"
V8_RUNTIME_LIBRARY="$(sed -n 's/^V8_RUNTIME_LIBRARY:[^=]*=//p' "$BUILD_ROOT/CMakeCache.txt")"
WEBGPU_RUNTIME_LIB="$(sed -n 's/^WEBGPU_RUNTIME_LIB:[^=]*=//p' "$BUILD_ROOT/CMakeCache.txt")"
mkdir -p "$PROOF_ROOT/installed/lib"
install -m 0644 "$V8_RUNTIME_LIBRARY" "$WEBGPU_RUNTIME_LIB" "$PROOF_ROOT/installed/lib/"
install -m 0755 "$PROOF_ROOT/installed/bin/pulp-cpp" "$PROOF_ROOT/installed/bin/pulp"

HARNESS="$SOURCE_ROOT/tools/scripts/gpu_clean_agent_journey.py"
python3 "$HARNESS" prepare \
  --pulp "$PROOF_ROOT/installed/bin/pulp" \
  --symptom compute-readback-mismatch \
  --workspace "$PROOF_ROOT/workspace" \
  --case-dir "$PROOF_ROOT/private-case" \
  --source-root "$SOURCE_ROOT" \
  --build-root "$BUILD_ROOT" \
  --cli-install-script "$BUILD_ROOT/tools/cli/cmake_install.cmake" \
  --installed-prefix "$PROOF_ROOT/installed" \
  --plan-root "$PLAN_ROOT" \
  --plan-document "$PLAN_DOCUMENT"

python3 "$HARNESS" record \
  --case "$PROOF_ROOT/private-case/case.json" \
  --agent-bin "$(command -v codex)" \
  --model gpt-5.6-sol

python3 "$HARNESS" verify \
  --case "$PROOF_ROOT/private-case/case.json" \
  --session "$PROOF_ROOT/private-case/agent-session.json" \
  --receipt "$SOURCE_ROOT/docs/validation/gpu-clean-agent/m3-a5-clean-agent-independent-20260829.json" \
  --bundle "$SOURCE_ROOT/docs/validation/gpu-clean-agent/m3-a5-clean-agent-independent-20260829.bundle.json"
```

Review and commit both new durable artifacts. Preserve the private case and
workspace only as a local forensic source until the redacted pair is reviewed;
they are not required for checked-in replay. CTest exercises the nonterminal
`prepare` process contract and planted trust primitives only. It cannot claim
A5 acceptance.
