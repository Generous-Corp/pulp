# Repo Readability Cleanup — Small Wins Plan

**Date:** 2026-05-14  **Branch:** `chore/repo-readability-small-wins`
**Scope:** Behavior-preserving cleanup of comments, generated/reference docs, agent skills, and doc-update infrastructure to reduce transient issue/PR/wave/handoff noise that has leaked into long-lived documentation.

## Goal

Land a small, mergeable cleanup pass that: (1) removes the most visible workflow archaeology from public docs and source comments, (2) preserves stable technical rationale and legitimate upstream/vendor references, (3) adds a lightweight lint that prevents the pattern from re-emerging in the same files going forward. No architecture rewrites, no public API changes, no broad formatting churn, no runtime behavior changes.

## Background

### Where the noise lives — verified hits on `chore/repo-readability-small-wins`

**Public reference docs (highest-density, public-facing):**
- `docs/reference/compat/canvas2d.md` — multiple `## Wave N` sections with dates, `pulp #1525`/`#1521`/`#1526`/`#1527`, "Wave 1 paperwork (2026-05-07)", "PR #1348", `sub-agent #24`. Hand-written markdown, NOT generated from `compat.json`.
- `docs/reference/compat/css.md` — dated cleanup headings ("2026-05-07 (Wave 4 css extensive…)", "Wave 1 backgroundAttachment precedent", "Wave 2 css.2 fontSize precedent"), several inline `pulp #15xx` references.
- `docs/reference/cli.md` — "Validator-discovery preflight (#743)", "Headless / screenshot flags (#914)", "#499 Slice 1", "Slice 3 (#548)", "issue #940".
- `docs/reference/imports/index.md` — `pulp #1031`, `pulp #995`.
- `docs/status/cli-commands.yaml` — "CI auto-validation (#914)", "#547 Slice 2", "Slice 3, deferred — issue #947".
- `docs/status/support-matrix.yaml` — "removed on 2026-04-18 during parity-audit", "PR #283".

**Guides (medium-density, mostly hand-written long-form):**
- `docs/guides/platforms/android.md` — `issue #337`, `See issue #487`.
- `docs/guides/compat-sync.md` — `issue #1029`, `ships in #1029`.
- `docs/guides/coverage.md` — `#641`, `Issue #641`, link to a planning audit-dated path.
- `docs/guides/docs-site.md` — "Before #577 (completed 2026-04-21)".
- `docs/guides/focus-mode.md` — "Slice 1 of #940", "On 2026-04-28 … #924", "shape that worked on 2026-04-28".
- `docs/guides/iwyu.md` — "Three real incidents on 2026-04-21".
- `docs/guides/release-watchdog.md` — "2026-04-20 … PR #501", "2026-04-30 … merged via".
- `docs/guides/versioning.md` — "PR #1008, 2026-04-30".

**Agent-facing skills (medium, agent-readable — sets the tone for what agents emit next):**
- `.agents/skills/android/SKILL.md` — "Slice 6 (#551)", "Tested 2026-04-18", "spike #355 follow-up".
- `.agents/skills/auv2/SKILL.md` — "Current Gaps (2026-04-22)".
- `.agents/skills/ci/SKILL.md` — "cloud-handoff recipe", "shipyard cloud handoff run --apply", "need a manual handoff".
- `.agents/skills/cli-maintenance/SKILL.md` — "Design decision (2026-04-21…)", "bump-undo-2026-04-21…", "Codex 2026-04-21 wave 2 P1".
- `.agents/skills/engine/SKILL.md` — "gap matrix in pulp #468".
- `.agents/skills/hosting/SKILL.md` — "fixed in PR #1873".
- `.agents/skills/import-design/SKILL.md` — "learned 2026-05-03", "learned 2026-05-14".
- `.agents/skills/prototype-loop/SKILL.md` — "Slice status (as of 2026-04-27)", "shape that worked on 2026-04-28".

**Source comments (lower density per file, broader spread):**
Representative `REMOVE`/`REWRITE` targets surfaced by the source-comment probe (full inventory in the file:line list; relevant subset):
- `core/canvas/include/pulp/canvas/canvas.hpp:173` — "pulp Wave 2 canvas2d cheap wiring …" → `REMOVE` planning prefix, keep API description.
- `core/canvas/include/pulp/canvas/canvas.hpp:179` — "pulp #1434 — added `justify` for CSS / RN `text-align: justify`" → `REWRITE` to drop issue ref, keep the CSS/RN parity statement.
- `core/canvas/include/pulp/canvas/canvas.hpp:616` — "Workstream 07 slice B4 follow-up (#255)" → `REMOVE`.
- `core/canvas/include/pulp/canvas/skia_canvas.hpp:457` — "per-View writing-direction lookup lands (#1506)" → `REWRITE` (future-work issue ref in long-lived header).
- `core/canvas/src/skia_canvas.cpp:8` — "pulp #1737 (Codex P2 sweep on #1791) — Skia headers MUST be included …" → `REWRITE` (keep ordering invariant, drop sweep provenance).
- `core/audio/platform/linux/alsa_device.cpp:161` — "fill cycle. See #438 P1 Codex review on #387." → `REWRITE`.
- `core/audio/platform/win/wasapi_device.cpp:62` — "See #438 P1 Codex review on #386." → `REWRITE`.
- `core/host/include/pulp/host/scanner.hpp:78` — "Codex 2026-04-21 review on #545 …" → `REWRITE` keep hermetic-tool rationale.
- `core/host/src/scanner_clap.cpp:124` — `runtime::log_warn("CLAP scan: entry->init threw for '{}': {} (#812 guard)", …)` → `REWRITE` (issue ref leaks into runtime user-visible log line).
- `core/render/src/gpu_compute.cpp:226` and `gpu_compute_pool.hpp:172` — `REWRITE` "Codex 2026-04-21 review on #560 (wave 2) P1 …" entries.
- `core/runtime/src/zip.cpp:104` — "Codex P2 on PR #747 — required for …" → `REWRITE`.
- `core/view/js/web-compat-element.js:249` — "TODO: add test for #1917 P1 regression …" → `REMOVE` (issue-only TODO).
- `core/view/js/web-compat-element.js:990` — "URLs and http(s) fetch are deferred follow-ups (see #1658)" → `REWRITE` (state the constraint, not the issue).
- `examples/design-tool/design-tool.js:572`, `:778` — "#50:" / "#55:" used as section labels → `REMOVE`.
- `examples/threejs-native-demo/main.cpp:1326` — "Slice 0.5 of #516" header → `REMOVE`.
- `examples/threejs-native-demo/main.cpp:1781` — "Codex 2026-04-21 review on #553 …" → `REWRITE`.
- `packages/pulp-import-ir/src/types.ts:9` — "Spec: /tmp/pulp-ir-spec-spike.md (sub-agent #25 draft)" → `REMOVE` (`/tmp` ref is dead).
- `packages/pulp-import-ir/test/anchors.test.ts:142`, `test/lower.test.ts:1` — "pulp #1499 follow-up — Codex P1: …" → `REWRITE` to describe the regression scenario.
- `ship/src/appcast.cpp:169` — "Real Ed25519 signing is a follow-up to this P0 fix (#295). Until …" → `REWRITE` (state current capability, not P0 history).
- `tools/coordination/health-check.sh:115`, `:197` — "Agent A/B/C status doc" coordination artifact → `REMOVE` (or move into `planning/` if still used; verify before deleting).
- `tools/screenshot/pulp_screenshot.cpp:137` — "TODO: pin in #1919 test …" → `REMOVE`.

**`KEEP` examples (real upstream/regression workarounds — NOT cleanup targets):**
- `core/audio/include/pulp/audio/device.hpp:110` — MSVC C2248 / WASAPI hotplug context.
- `core/audio/platform/android/oboe_device.cpp:75`, `:110` — Oboe stream cleanup ordering.
- `core/canvas/include/pulp/canvas/canvas.hpp:572` — `clearRect` regression rationale.
- `core/canvas/src/sdf_atlas.cpp:215` — Skia macro compatibility workaround.
- `core/events/include/pulp/events/timer.hpp:13` — concurrency/lifetime design block.
- `core/format/src/clap_adapter.cpp:27` — alignment workaround.
- `core/host/src/scanner_clap.cpp:63` — defensive scan failure mode.
- `core/midi/platform/win/winmidi_device.cpp:181` — WinMM shutdown hazard.
- `core/platform/include/pulp/platform/win32_sane.hpp:20` — UIAutomationCore.h header conflict.
- `core/view/src/widget_bridge.cpp:626` — concrete pointer-event interaction workaround.
- `examples/threejs-native-demo/CMakeLists.txt:20` — `--capture` regression test rationale.
- `ship/include/pulp/ship/appcast.hpp:57` — explains the API contract that previously failed silently.

These show the rewrite policy in practice: **keep what describes a real invariant or upstream quirk; drop what only describes who/when/which-PR.**

### Documentation pipeline — what generates what

| Surface | Source of truth | Generator | Mode |
|---|---|---|---|
| Site (`build/site/**`) | `docs/**/*.md`, `mkdocs.yml` | `mkdocs build` (+ `tools/mkdocs_hooks.py` for URL flattening) | hand-written prose, no template substitution into content |
| API reference (`/api/`) | `core/*/include/**` headers | `tools/build-api-docs.sh` → Doxygen via `docs/doxygen/Doxyfile` | Doxygen extracts header comments verbatim |
| `docs/reference/capabilities.md` | `docs/status/support-matrix.yaml` `limitations:` block | `tools/docs_generate.py` + `tools/list_limitations.py` (region between `<!-- generated:start id=limitations --> … <!-- generated:end -->`) | `generate` writes; `check` verifies on PR via `tools/check-docs.sh` |
| `docs/reference/compat/*.md` | (no generator) | hand-written markdown | the hits in `canvas2d.md` / `css.md` are author-written, not generated |
| `compat.json` | hand-edited matrix; `tools/scripts/compat_sync_check.py --mode=apply` can stub-write entries | — | enforced sync gate via `compat_sync_check.py` |

**Implication for this plan:** cleaning the compat reference docs and the guides is a **direct edit** on hand-written markdown — there is no upstream template to fix first. Only `docs/reference/capabilities.md` has a generated region, and the generator is well-behaved (no noise).

### Existing lint/check infrastructure — the pattern to follow

All gates under `tools/scripts/` follow a uniform shape:
- Single-file Python, zero deps.
- Diff-aware via `--base origin/main --head HEAD`.
- Three modes: `--mode=hint` (PostToolUse, always exits 0), `--mode=report` (CI/pre-push, exits 1 on violation), `--mode=apply` where applicable.
- Exit conventions: `0` clean, `1` violation, `2` invocation/config error.
- Bypass via commit trailer with `reason="…"` (e.g. `Skill-Update: skip skill=ci reason="docs-only"`).
- Wired in three layers: `hooks/scripts/cli-plugin-sync.sh` (hint), `.githooks/pre-push` (report), `.github/workflows/version-skill-check.yml` (report, authoritative).

Existing scripts: `skill_sync_check.py`, `version_bump_check.py`, `compat_sync_check.py`, `docs_sync_check.py`, `source_tree_pollution_check.py`, `host_pump_lint.py`, `check_cli_mcp_parity.py`, `cli_sync_check.py`. A new `docs_noise_lint.py` would slot in naturally.

### Prior art

None. No prior plan, completed report, or recent commit on `origin/main` covers a comments/docs noise cleanup pass. The closest related effort is `docs/reports/wave5-css-audit.md`, which is a CSS-compat correctness audit — different goal, different files.

## Approach

**Three-layer strategy, in order of risk and value:**

1. **Hand-written public reference docs first.** The compat docs and `docs/reference/cli.md` are the most user-visible surfaces and the densest concentration of noise. Direct edits are low-risk (no generators), trivially reviewable in a diff.
2. **Agent-facing skills next.** Skills set the tone for what Claude/Codex emit *next time*. Cleaning these reduces ongoing noise generation. Each `SKILL.md` has its own version-bump/skill-sync gate but is text-only — bypass via `Skill-Update: skip skill=<name> reason="docs-only readability cleanup"` if a touched skill has no functional change.
3. **Add a forward-looking lint** — `tools/scripts/docs_noise_lint.py` — that flags the same patterns this PR removes, scoped to `docs/reference/**` and `.agents/skills/**` only. Wire it as advisory `--mode=hint` in `cli-plugin-sync.sh` and as `--mode=report` in `.githooks/pre-push`. Defer CI-enforcing wiring to a follow-up after the first month of false-positive triage.

**Defer source-comment edits.** The probe surfaced ~40 hits across `core/`, `examples/`, `packages/`, `ship/`, `tools/` — many are `KEEP` (real workarounds), many are `REWRITE` (need careful per-file judgment), and the touched files trigger compat-sync, version-bump, and skill-sync gates per-subsystem. Doing this in the same PR risks scope creep and a noisy diff. Land a separate, smaller follow-up PR per subsystem cluster.

**Keep all changes behavior-preserving.** No source code semantics change. No YAML schema fields removed (status/yaml fields stay; only inline comment text and cell content gets rewritten). No CMake or build logic touched. Public API headers get comment edits only — no signature changes. The one runtime-touching candidate (`scanner_clap.cpp:124` log message containing `(#812 guard)`) is in scope for the source-comment follow-up PR, NOT this one.

## Work Items

### Item 1 — Clean public reference docs
**Classification:** REMOVE (planning headings, agent labels, dated cleanup notes) + REWRITE (issue-number cites kept-with-rationale → stable technical statements).
**Risk:** low. Hand-written markdown + YAML description fields only; no schema changes; no generators between source and rendered docs.
**Goal:** Remove wave/agent/handoff/dated-cleanup language and naked issue-number breadcrumbs from the highest-traffic reference pages, replacing them with stable technical statements.
**Done when:**
- `docs/reference/compat/canvas2d.md`: no `## Wave N` headings, no `Wave N paperwork`, no `pulp #15xx` inline cites. For headings whose substance is in scope (e.g. `## Recently expanded (#1348)`), reframe to a substantive title; if the substance isn't recoverable from the surrounding section, **dropping the heading is acceptable** — do not require PR archaeology.
- `docs/reference/compat/css.md`: no `2026-05-NN (Wave N …)` cleanup headings; precedent references rewritten to state the actual policy ("backgroundAttachment precedent" → describe what the precedent *is*) or dropped where the precedent is no longer load-bearing; `pulp #15xx` inline cites removed.
- `docs/reference/cli.md`: all `(#NNN)` parentheticals after feature names dropped or replaced with one-line technical descriptions; `#499 Slice 1` / `Slice 3 (#548)` removed.
- `docs/reference/imports/index.md`: `pulp #1031`, `pulp #995` removed or rewritten.
- `docs/status/cli-commands.yaml` and `docs/status/support-matrix.yaml`: inline issue-number tails on description fields cleaned; YAML keys/structure unchanged.
- `tools/check-docs.sh` passes.
- `mkdocs build` succeeds (smoke build via `pulp docs build-site` if available).
- **Verify before committing**: run `python3 tools/scripts/compat_sync_check.py --mode=report --base origin/main` and `python3 tools/scripts/docs_sync_check.py --mode=report --base origin/main` against the diff. Description-field text edits in `docs/status/*.yaml` and prose edits in `docs/reference/compat/*.md` should not trip these gates; if they do, narrow the diff or add the documented bypass trailer.
**Key files:** `docs/reference/compat/canvas2d.md`, `docs/reference/compat/css.md`, `docs/reference/cli.md`, `docs/reference/imports/index.md`, `docs/status/cli-commands.yaml`, `docs/status/support-matrix.yaml`.
**Dependencies:** none.
**Size:** medium.

### Item 2 — Clean agent-facing skills
**Classification:** REMOVE (dated cleanup notes, agent/wave labels) + REWRITE (PR/issue breadcrumbs → technical claims).
**Risk:** low. Text-only edits to documentation files; no behavior change.
**Goal:** Remove dated cleanup notes and PR/issue breadcrumbs from `.agents/skills/**/SKILL.md` so the in-context examples agents read no longer model the pattern back.
**Preflight (do this before editing):** `skill_sync_check.py` is keyed off **mapped source paths** in `tools/scripts/skill_path_map.json`, not `SKILL.md` content. Confirm whether text-only `SKILL.md` edits trigger the gate by inspecting that JSON and (optionally) running the script against a one-line probe edit. If the gate does **not** fire on SKILL-only edits, drop the per-skill `Skill-Update:` trailers from the commit. If it **does** fire, also confirm that multiple `Skill-Update:` trailers in one commit are honored — the existing examples in `CLAUDE.md` show only one. If unconfirmed, split into per-skill commits.
**Done when:**
- `.agents/skills/android/SKILL.md`: "Slice 6 (#551)" reframed, "Tested 2026-04-18" / "spike #355 follow-up" removed.
- `.agents/skills/auv2/SKILL.md`: "Current Gaps (2026-04-22)" → "Current Gaps" (the date adds nothing — the file itself is git-dated).
- `.agents/skills/cli-maintenance/SKILL.md`: "Design decision (2026-04-21…)" → "Design decision"; "Codex 2026-04-21 wave 2 P1" rewritten as the technical claim.
- `.agents/skills/engine/SKILL.md`: "gap matrix in pulp #468" → "engine capability comparison" (or whatever the matrix actually shows).
- `.agents/skills/hosting/SKILL.md`: "fixed in PR #1873" → state the fixed behavior.
- `.agents/skills/import-design/SKILL.md`: "learned 2026-05-03" / "learned 2026-05-14" prefixes dropped.
- `.agents/skills/prototype-loop/SKILL.md`: "Slice status (as of 2026-04-27)" / "shape that worked on 2026-04-28" rewritten to describe the current shape.
- Apply per-skill `Skill-Update: skip skill=<name> reason="readability cleanup, no functional change"` trailers **only if the preflight confirmed the gate fires**.
- `python3 tools/scripts/skill_sync_check.py --mode=report --base origin/main` passes.
**Key files:** the seven skills enumerated above. **Defer** `.agents/skills/ci/SKILL.md` — its "handoff" mentions describe an actual Shipyard CLI verb (`shipyard cloud handoff run`) and are not workflow archaeology. (Item 3's lint must path-allowlist this file; see Item 3.)
**Dependencies:** preflight above. Can land in same PR as Item 1.
**Size:** small-to-medium.

### Item 3 — Add `tools/scripts/docs_noise_lint.py`
**Classification:** REWRITE (new guardrail script — adds a check, no docs/source content moved).
**Risk:** low for the script itself; medium for the wiring (a noisy first run on `.githooks/pre-push` could surprise contributors). Mitigated by advisory-only mode in v1.
**Goal:** Forward-looking lint that flags the same patterns this PR removes, so the noise doesn't return to the cleaned files.
**Done when:**
- New script `tools/scripts/docs_noise_lint.py` exists, single-file, zero-dep Python, follows the established `--mode=hint/report` + exit `0/1/2` shape.
- Default scan paths: `docs/reference/**/*.md`, `docs/reference/**/*.yaml`, `.agents/skills/**/SKILL.md`. Whether to expose the path/pattern config as inline constants vs. a sidecar JSON is the implementer's call — both fit the existing convention.
- **Pattern intent (categories), not exact regex set** — the implementer picks final patterns after a triage pass on the cleaned tree. Categories that MUST be caught:
  - planning/wave/agent labels (`Wave N`, `Agent A/B/C`, `slice N of …`)
  - dated cleanup tags (`audit-YYYY-MM-DD`, parenthetical `(YYYY-MM-DD)` next to headings)
  - issue/PR cite phrases (`see #N`, `added in #N`, `fixed in #N`, `via #N`, `pulp #N`)
  - **bare `(#NNN)` parentheticals in prose and headings** — required to actually catch what Item 1 cleans (the obvious shape `^#+ .*\(#\d+\)$` is necessary but not sufficient; mid-sentence `(#499)` must also fire)
  - issue-only TODOs (`TODO …#N` with no other content)
  - workflow artifact phrases (`planning artifact`, `markdown artifact`, `compat pass`)
- Allowlist: external spec/vendor refs (`WHATWG`, `W3C`, `WebGPU`, `Skia`, `Dawn`, `Yoga`, `ICU`, `HarfBuzz`, `CSSWG`, `MDN`, `CVE-\d{4}-\d+`, `RFC \d+`); per-line opt-out marker `<!-- docs-noise-lint: skip — <reason> -->` for the rare legitimate internal cite.
- **File-level path allowlist** (no scan): `docs/migrations/**`, `docs/reports/**`, `docs/policies/**`, `docs/contracts/**`, `CHANGELOG*`, `planning/**`, `.github/**`, **and `.agents/skills/ci/SKILL.md`** (the `handoff` term there refers to the live `shipyard cloud handoff` verb, not workflow archaeology).
- Scan semantics for v1: line-based; case-sensitive; **fenced code blocks (```` ``` ````) and inline backtick spans are skipped**; YAML files scanned as plain text against description-shaped lines.
- Bypass trailer: skip a new convention in v1 — the per-line skip marker is enough while the lint is advisory. Add `Docs-Noise: skip …` only if the lint is later promoted to a hard gate.
- Wired into `hooks/scripts/cli-plugin-sync.sh` as `--mode=hint`. Wired into `.githooks/pre-push` as `--mode=report` behind the same `PULP_DISABLE_PREPUSH_GATES=1` advisory toggle the other gates use. **Defer** wiring into `.github/workflows/version-skill-check.yml` and into `tools/check-docs.sh` until after one cycle of false-positive triage; pick **one** of those two surfaces when promoting (avoid two parallel "docs is clean" gates).
- Test fixture: `tools/scripts/test_docs_noise_lint.py` covering at least: a positive hit per denied category, an allowlisted external-spec line that does NOT trigger, a fenced-code-block line that does NOT trigger, the per-line skip marker, the `ci/SKILL.md` path-allowlist, exit codes for hint/report/error.
**Key files:** new `tools/scripts/docs_noise_lint.py`, new `tools/scripts/test_docs_noise_lint.py`, optional `tools/scripts/docs_noise_lint_config.json`, edits to `hooks/scripts/cli-plugin-sync.sh` and `.githooks/pre-push`.
**Dependencies:** Items 1 and 2 land first (or in the same commit) so the lint is green on a clean tree.
**Size:** medium.

### Item 4 — Add a short pointer in `CLAUDE.md`
**Classification:** REWRITE (one new paragraph; no removals).
**Risk:** low.
**Goal:** Make the new lint and its policy discoverable to future agents and humans without committing to a full prose policy section before the lint has bedded in.
**Done when:**
- New 3–5 line note in `CLAUDE.md` under "Repo Standards" pointing at `tools/scripts/docs_noise_lint.py` and stating the one-sentence policy: *long-lived docs and source comments explain current behavior, invariants, and upstream/vendor quirks — not workflow history; transient issue/PR/wave/handoff references belong in `planning/`, `docs/migrations/`, `docs/reports/`, or the changelog.*
- Defer the longer "Comments and Docs Voice" section (with bad/good examples and full allowlist) to a follow-up after one cycle of lint false-positive triage.
**Key files:** `CLAUDE.md`.
**Dependencies:** Item 3 must exist (the pointer must point at a real script).
**Size:** small.

### Deferred work (explicitly out of scope for this PR)

| Item | Why deferred |
|---|---|
| Source-comment cleanup across `core/`, `examples/`, `packages/`, `ship/`, `tools/` | Cross-subsystem touches trigger version-bump, compat-sync, and skill-sync gates in many directions; needs per-subsystem judgment and per-cluster PRs. Land as follow-ups, e.g. `chore/clean-canvas-comments`, `chore/clean-host-comments`. |
| Removing the `(#812 guard)` runtime log token in `scanner_clap.cpp:124` | Touches a user-visible runtime string — small but technically a behavior change in log output. Bundle with the host source-comment follow-up. |
| Cleaning `docs/guides/**` issue references | Guides are long-form prose; rewrites are subjective and high-touch. Address after the lint has bedded in and the policy is settled. |
| `CLAUDE.md` lines 445 (dated incident note), 813 ("25 skills as of …"), 850 ("Shipyard #106") | The incident note is genuinely load-bearing rationale for the "tests ship with fixes" rule; the skill count is auto-stale and should be removed (or auto-generated) but that's a separate small change; the Shipyard #106 ref points at a real upstream issue. Triage individually in a follow-up. |
| `tools/coordination/health-check.sh` "Agent A/B/C" labels | Need to verify whether the coordination harness is still in active use. If yes, the labels are functional, not archaeological. Confirm before touching. |
| Wiring `docs_noise_lint.py` as a hard CI gate | Wait one cycle of false-positive triage before promoting. |

## Recommended subset for THIS PR

**Land:** Items 1 + 2 + 3 + 4 together. They are tightly related, easy to review as a single diff, and the lint guards what the cleanup achieved. Total surface: ~10–15 reference doc and skill files edited (text-only), three new files in `tools/scripts/`, two small wiring edits in `hooks/scripts/cli-plugin-sync.sh` and `.githooks/pre-push`, one `CLAUDE.md` section.

**Verification commands** (run before pushing):
```bash
# Docs consistency
tools/check-docs.sh

# Existing gates still pass
python3 tools/scripts/skill_sync_check.py --mode=report --base origin/main
python3 tools/scripts/compat_sync_check.py --mode=report --base origin/main
python3 tools/scripts/version_bump_check.py --mode=report --base origin/main
python3 tools/scripts/docs_sync_check.py --mode=report --base origin/main

# New lint passes on the cleaned tree
python3 tools/scripts/docs_noise_lint.py --mode=report

# New lint test fixture
python3 tools/scripts/test_docs_noise_lint.py

# Optional: site smoke build
pulp docs build-site   # or: mkdocs build --site-dir build/site
```

**Commit trailers required:**
- Skills touched in Item 2: `Skill-Update: skip skill=<name> reason="readability cleanup, no functional change"` per affected skill.
- If Item 4's `CLAUDE.md` edit is bundled, verify `version_bump_check.py` doesn't ask for a bump (CLAUDE.md is unmapped); if it does, `Version-Bump: skip reason="docs-only repo-standards addition"`.
- The PR itself uses `shipyard pr` per CLAUDE.md; do NOT bypass the gates by hand.

## Open Questions

1. **`tools/coordination/health-check.sh`** — Are the `Agent A/B/C` labels still wired to anything functional, or are they an archaeology relic? If functional, leave them. If not, drop them in the deferred source-comment follow-up. (Confirm before touching that file in any PR.)
2. **`docs_noise_lint.py` enforcement scope** — Should the lint default to scanning `docs/guides/**` too, with the per-line skip marker as the escape hatch? Or keep it narrow to `docs/reference/**` + `.agents/skills/**` for now? Recommendation: **narrow** for v1; expand once the false-positive shape is understood.

## References

- Existing lint pattern: `tools/scripts/skill_sync_check.py`, `tools/scripts/compat_sync_check.py`, `tools/scripts/source_tree_pollution_check.py`.
- Hook wiring: `.githooks/pre-push`, `.github/workflows/version-skill-check.yml`, `hooks/scripts/cli-plugin-sync.sh`, `hooks/hooks.json`.
- Docs pipeline entrypoints: `tools/check-docs.sh`, `tools/docs_generate.py`, `tools/list_limitations.py`, `mkdocs.yml`, `docs/doxygen/Doxyfile`.
- Compat surface: `compat.json`, `tools/scripts/compat_path_map.json`, `docs/reference/compat/*.md`.
- Repo policy: `CLAUDE.md` "Repo Standards", "Verify Against Code, Not Planning Docs", "Skill Maintenance Rule".
