---
name: test-audit
description: >-
  Decide whether a Pulp test earns its place — before writing it, when reviewing
  one, or when sweeping a subsystem for low-value, duplicated, or mis-routed
  tests. Applies a four-question authoring gate, a junk-pattern list, a
  retention bar, and an evidence template for any deletion, and routes kept
  tests to the right lane (required gate, pr-fast, slow, nightly). Handles
  "should I add a test for this", "is this test useful", "audit these tests",
  "reduce test time", "which tests can go", and "move toward functional tests".
---

# Test Audit

The goal is **confidence per minute of gate time**, not a deletion count and
not a coverage number. Most of Pulp's suite is real tests; its waste is mostly
duplication across layers, oversized grids, and tests running on a lane that
does not need them. Audit with that prior.

Adapted from OpenClaw's `test-audit` skill
(<https://github.com/openclaw/openclaw/blob/main/.agents/skills/test-audit/SKILL.md>),
with Pulp's lanes, harnesses, and retention rules.

This complements, and does not relax, CLAUDE.md's "Tests ship with fixes": a
fix still lands with its test. This skill decides **which** test — one, at the
boundary that owns the behavior, functional by default.

## Modes

| Mode | When | Output |
|---|---|---|
| **Authoring** | before adding or changing any test | pass the gate below, or don't add it |
| **Audit** | reviewing a PR's tests, or a focused sweep | a short list of high-confidence candidates with evidence |
| **Campaign** | pruning one subsystem's suite | one owner-boundary PR per subsystem, evidence per deletion |

Audit and campaign start **read-only**: record evidence before editing anything.

## Authoring gate — answer all four before adding a test

1. **What behavior, invariant, or contract does it protect?** Name it in the
   test name as an observable outcome ("the tom's pitch dive finishes before the
   note does"), not as the function it calls.
2. **What realistic regression makes it fail?** If you cannot describe the bug,
   the test is decoration.
3. **Why doesn't existing coverage already catch that?** Each contract has one
   primary owner at the strongest boundary. A second layer needs its own
   distinct risk. Prefer adding a row to an existing table-driven case over a
   new case.
4. **Does it need a seam no real caller uses?** Then test at the real boundary
   instead of adding a test-only accessor.

Then prove it can fail: break the fix and watch the test go red with
`tools/scripts/confirm_failure.sh` (see CLAUDE.md, "Confirm the failure").

## Junk patterns

A match fails the gate unless the test names an independent contract the
retention bar keeps.

- No assertions, or assertions that cannot fail.
- Self-comparison: two identically configured objects compared to each other
  (the guard under test is never exercised).
- Expected values produced by the code under test (goldens regenerated from the
  renderer, impulse values snapshotted from the filter itself) — change
  detection, not correctness.
- A test-local subclass or mock that implements the behavior being asserted.
- Capability tests that restate declared flags or ID constants instead of
  exercising delivery.
- Exact string greps over source, docs, or generated code, where a
  behavior-level assertion (layout geometry, render output, round trip) exists.
- The same contract re-asserted at every layer (proof, authoring, control,
  runtime) with no layer-specific risk.
- Near-identical cases that differ by one input — merge into a table.
- Names that promise more than the inputs exercise.
- Per-element `REQUIRE` inside a sample loop — compare whole buffers once.
- Phase or ticket breadcrumbs in names or tags (already forbidden; see the
  `code-comments` skill).

A test that breaks under a behavior-preserving refactor is implementation-
coupled. In authoring, rewrite it at the owning boundary. In an audit it is
suspect, not automatically deletable.

## Retention bar — keep these

- Public API, plugin-format adapter, protocol, state serialization / migration,
  and file-format contracts.
- Real-time safety, threading and lock-free handoff invariants.
- Platform behavior (clipboard, dialogs, audio devices, screenshots).
- Credible regressions — a bug that actually shipped or nearly did.
- Functional end-to-end behavior (see templates below).
- Static drift / registry guards that are the cheapest independent check of a
  real invariant (two real sources agree). Being static or slow is not a reason
  to delete.

A retained test that fails on the base branch is a possible product bug: fix it
at the owner, don't delete it.

## Prefer functional tests

Drive the public path, observe the outcome a user or host would observe, and
assert a stated law with a derived tolerance and a built-in control.

| Area | Shape | Existing example |
|---|---|---|
| Format adapters | render directly and through the adapter over a sample-rate × block-size sweep; require equality | `test/test_vst3_audio_parity.cpp` |
| Widgets | `simulate_click` / `simulate_drag` → state change + callback | `test/test_widgets.cpp` |
| Design import | live source vs baked native layout snapshot | `test/test_design_import_native_materializer.cpp` |
| DSP | render through `process_block` or `RenderScenario`; measure with the shared analyzers; include a no-effect control | FDN reverb, drum-voice, and kick suites |
| CLI | shell out to the built binary; assert exit code + output | `test/test_control_sample_region_e2e.cpp` |

Load the `audio-harness` skill before choosing DSP acceptance gates.

## Grid economics

Most DSP runtime sits in a few cases with oversized grids. Before accepting a
slow test, ask whether a smaller grid catches the same regression:

- One point per behavior regime, not every sample rate × every setting.
- Short renders: reset leaks and block-partition dependence show up in seconds.
- Build expensive fixtures (baked graphs, baselines) once per case, not per probe.
- Keep an exhaustive sweep if it has value — tag it `[slow]` so it leaves the
  required gate.

## Routing: which lane a kept test belongs in

Labels decide lanes; `docs/guides/test-lanes.md` is the source of truth.

- **no label** — required `macos` gate (merge queue runs the full suite).
- **`pr-fast`** — deterministic static contracts that also run on the PR head.
- **`slow` / `performance` / `bench` / `quality-lab` / `validation`** — off the
  required gate. Before moving a test there, name what still enforces it: the
  nightly is informational, not a gate, so a test that must block needs an
  affected-path selector (see "Affected slow proofs" in `test-lanes.md`).

`RUN_SERIAL` and `PROCESSORS 8` tests run alone and set the floor on the test
step's wall-clock. Don't add either without a stated reason, and don't strip an
existing one without reading why it was set.

Measure against real gate timing, not a local Debug run. The `macos` job log
prints every test's time (`Test #N: <name> ... Passed <s> sec`); local Debug
costs can be an order of magnitude higher and misrank tests.

## Candidate evidence (required before any deletion)

Record for each candidate; a missing field blocks the deletion.

- Test name and `file:line`.
- The failure it can detect.
- Non-test callers of the covered code.
- The stronger remaining proof at the owner boundary (or why none is needed).
- Why the test exists — `git log -L` or the originating commit.
- What production or test-support code the deletion unlocks.
- Risk, and the focused validation command.

## Edit shape

- One coherent owner-boundary batch per PR.
- Delete test-only accessors and wrappers along with the tests that kept them
  alive.
- Move a retained regression to its canonical owner rather than keeping copies.
- Don't add replacement tests that restate the implementation, and don't
  delete uncertain candidates to inflate a count.
- Report test LOC and production LOC separately in the PR description.

## Validation

1. Build and run the owner's and siblings' test targets
   (`ctest --test-dir build -R <pattern>`; pair any "0 tests" result with a
   positive control — `ctest -R` exits 0 on no match).
2. For a removed string grep, run the tool or check that owns the real contract.
3. `tools/scripts/gates.sh origin/main`.
4. Run the inline architectural + adversarial review before the PR.
