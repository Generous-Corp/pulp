# Project C result — proceed, with an L6 gate

Date: 2026-07-28
Worktree base: Pulp `origin/main` at `c9305061cbd16fd66d354ff86585277e0b98ed27`
Forge authority inspected read-only:
`/Volumes/Workshop/Code/forge-sdk-bump-20260727` at `e9999a6`

## Verdict

The primary bet is viable, but not as “any frontier model, profile-only,
single-shot.”

- Claude Fable 5 authored 6/6 artifacts that passed the experiment's L0-L4
  mirror single-shot.
- GPT-5.5 authored 1/6 that passed single-shot. Its other five all made the
  same repairable field error: putting `linear-gradient(...)` in
  `style.background` instead of `style.gradient`.
- All five GPT-5.5 failures converged after one diagnostic repair. None needed
  round 2.
- Blind visual judging found 6/12 better than the current template floor, but
  0/12 at the Halo ceiling.
- Arm 2 (profile + craft rules + Ink & Signal base) won all three concepts.
- Several L0-L4-valid panels still clipped after Yoga layout. The static
  geometry checks are not a sufficient visible-quality gate.

Proceed with the C++ composer/ladder and prompt v2, behind total fallback. Make
post-Yoga overflow detection a blocking L6 gate for the frontier-provider path;
do not defer it to report-only if the designed panel can become user-visible.

## What ran

Three concepts:

1. Atlas Bus Compressor — hero-led, 6 macros.
2. Lattice Poly Synth — four peer sections, 10 macros.
3. Vector Utility — compact hero-less utility, 5 macros.

For each concept:

- Claude Fable 5 × arm 1 and arm 2.
- GPT-5.5 × arm 1 and arm 2.
- One initial call, with a shared maximum of two repair rounds.

This produced 12 initial artifacts, 5 repair calls, 12 final DesignIR
translations, and 12 Skia PNGs. Provider metadata is in `calls.jsonl`; raw
Claude response envelopes and Codex JSONL event streams are retained under
`artifacts/raw/`.

Claude calls used `claude -p --model fable --tools ""`; the provider returned
canonical model `claude-fable-5` for every call. GPT calls used
`codex exec --model gpt-5.5 --sandbox read-only --ephemeral`; their Codex thread
IDs and token usage are retained.

## Ladder results

| Layer | Single-shot | After repair | Evidence |
|---|---:|---:|---|
| L0 envelope | 12/12 | 12/12 | Python mirror of live Forge envelope |
| L1 profile | 7/12 | 12/12 | Python mirror of exact live Forge accepted fields/limits |
| L2 DESIGN.md/tokens | 7/12 reached; 7/7 pass | 12/12 | Python reference audit plus live Pulp DESIGN.md parse for all finals |
| L3 binding | 7/12 reached; 7/7 pass | 12/12 | exact concept macro diff in Python |
| L4 declared geometry | 7/12 reached; 7/7 pass | 12/12 | contract numeric checks in Python |
| L5 translation/DesignIR parse | n/a before repair | 12/12 partial | Python translation + live Pulp canonical DesignIR parse/serialize |
| L5 Forge link/probe | unavailable | unavailable | requires the held C++ composer; not claimed as passed |
| L6 rendered pixels | 12 renders | mixed | blind pixel judging found overflow and contrast/craft defects |

The live Forge checkout currently implements L0/L1 only. The experiment
therefore mirrors those exact fields in Python, runs later model-actionable
layers in Python, then proves each translated final through Pulp's real
DesignIR parser. It deliberately does not claim the unavailable
`prepare_designer_link`/probe half of L5.

## Diagnostic clustering and repair

Every one of the 18 initial diagnostics was the same semantic error:

```text
style.background must be #RRGGBB, #RRGGBBAA or a token reference
```

GPT-5.5 treated `background` as CSS shorthand and placed gradients there. One
repair prompt repeated the original task, named the paths, and stated:

> `background` is a solid color/token only; put `linear-gradient(...)` in the
> sibling `gradient` field.

All five artifacts then passed L0-L4 without new diagnostics. This is clean
convergence, not wandering.

Prompt v2 should put this field distinction directly in the profile table and
worked example. The named field diagnostic is worth preserving verbatim.

## Blind visual result

The picker received A-D images plus explicitly identified FLOOR and CEILING
images. A-D were deterministically shuffled; provider and arm were absent from
the prompt and filenames. The key was decoded only after picks were written.

| Concept | Blind ranking | Winner | Beat floor | Reached Halo |
|---|---|---|---:|---:|
| Compressor | D > A > C > B | Claude arm 2 (80) | 2/4 | 0/4 |
| Synth | B > A > D > C | GPT-5.5 arm 2 (74) | 3/4 | 0/4 |
| Utility | B > A > D > C | Claude arm 2 (76) | 1/4 | 0/4 |

Across all concepts, arm 2 won 3/3. This is stronger than a mild average lift:
craft rules plus a concrete language base changed the selected winner every
time.

The main visible failure class was overflow after layout:

- compressor: Claude arm 1 and GPT arm 1 had clipped bottom controls;
- synth: every candidate had at least one left-edge control/label clipping
  defect;
- utility: both GPT candidates clipped, one severely at top and bottom.

Those artifacts passed declared-size L4. This is direct evidence that L6's
post-Yoga box audit closes a real hole.

## Composition variance

Pixel mean-absolute-difference after common 256x256 normalization:

| Concept | Mean pairwise MAD | Root sizes | Node-count range | Read |
|---|---:|---:|---:|---|
| Compressor | 0.2603 | 4/4 distinct | 30-42 | high, inflated by one broken light/card composition |
| Synth | 0.0566 | 4/4 distinct | 35-96 | low visual variance despite large structural range |
| Utility | 0.0621 | 4/4 distinct | 25-46 | low visual variance |

The profile is not structurally identical—every concept produced four root
sizes and node counts varied widely—but the hero-less archetypes converge to
similar dark card/section compositions. Keep the constrained profile for
safety, but do not expect it alone to create language-level diversity. Variance
should come from stronger language bases/examples, not by reopening absolute
positioning or the full IR.

## Recommendations for the held implementation

1. Build the composer and L0-L5 ladder now; the compact artifact is viable.
2. Use the arm-2 prompt as the frontier-provider default. The profile-only arm
   is an ablation, not the product prompt.
3. Add an explicit `background` versus `gradient` sentence and show both fields
   in the worked example.
4. Block visible admission on post-Yoga overflow. A one-repair-then-fallback L6
   policy matches the observed failures.
5. Retain exact-path, exact-field diagnostics and the shared two-round budget.
   One round repaired every actionable failure here.
6. Keep total template fallback and retain parsed DESIGN.md tokens on terminal
   failure.
7. Treat Halo as aspirational: the constrained profile clears the existing
   template floor inconsistently and does not yet produce Halo-level
   illustration, bespoke controls, or visual storytelling.

## Reproduction and evidence map

- `profile_to_design_ir.py` — small profile-to-DesignIR translator/spec.
- `validate_emission.py` — experiment ladder mirror.
- `prompts/`, `concepts/` — frozen inputs.
- `artifacts/<provider>/<case>/` — rounds, diagnostics, final, DesignIR,
  canonical Pulp parse, generated bridge JS, Skia render and logs.
- `references/` — template floor and local Halo ceiling render.
- `blind/<concept>/` — anonymous A-D images, picker output, and decode key.
- `metrics/summary.json` — machine-readable rates, rankings, variance.
- `calls.jsonl` — provider/model/session/thread/token provenance.

Build proof:

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ...
cmake --build build --target pulp-import-design pulp-screenshot -j 6
12/12 translated DesignIR documents parsed and canonically serialized by Pulp.
12/12 final panels rendered with pulp-screenshot --backend skia.
```
