# tools/import-validation/

Scripts for the Spectr import-validation harness — the closed loop that
re-runs `pulp import-design` against `spectr/resources/editor.html`,
relaunches the native Spectr build, captures the live render, and compares
it against the canonical browser screenshot.

See `planning/spectr-validated-runtime-import-product-spec.md` (private
submodule) for the full design.

## What's in here

| Script | Purpose |
|--------|---------|
| `spectr-roundtrip.sh` | The full A→D loop: re-import editor.html → rebuild Spectr → launch → capture → diff. Top-level entry point for "did my Pulp fix narrow the gap?" |
| `diff_against_reference.py` | Single-score histogram + pixel-distance diff between two PNGs. Used by `spectr-roundtrip.sh` step 5. |
| `diff_against_reference_regions.py` | (PR #1871) Per-region masked diff — fails on the FIRST broken sub-region instead of averaging the whole frame. Recommended over the single-score variant once landed. |
| `semantic_probes.sh` | **Semantic-probe vector** — pixel-diff complement. Asserts no soft runtime-import error, lifecycle reached `mounted`+`settled`, and the canvas region actually painted. See below. |
| `check_label_coverage.sh` | Structural label-coverage check — string-match expected reference labels against the imported IR. |
| `reference-labels-spectr.txt` | Ground-truth list of UI labels that must appear in any successful Spectr import. |

## Semantic probes

`semantic_probes.sh` is the codex-required addition to the pixel-diff
pipeline (planning spec, 2026-05-12). Pixel diff alone has documented
blind spots — a render that "looks right" can still have silent runtime
failures the histogram never sees. The probes catch those:

| Probe | Pixel diff misses... | What the probe asserts |
|-------|----------------------|-------------------------|
| `runtime_import_err` | A Babel-transform or payload-eval failure that left React partially mounted. The chrome paints, the histogram is close, but `__pulpRuntimeImportErr__` is set and `onError` was never wired. | `__pulpRuntimeImportErr__` in the runtime log is `''` (or absent), not a non-empty error string. |
| `runtime_import_trace` | A render that bailed before `useEffect` queues drained, or a stale paint from a previous mount. The pixels are fine; the lifecycle never finished. | The runtime log contains both `phase=mounted` and `phase=settled` markers (or their JSON-shaped equivalents). |
| `canvas_non_blank` | A blank canvas with intact chrome — the chrome dominates the histogram, the overall similarity score passes, but the canvas region (the actual product surface) is empty. | `diff_against_reference_regions.py`'s `central_canvas.blank_candidate` is `false`. |

### Usage

```bash
# After a spectr-roundtrip.sh run, probe the artifacts:
tools/import-validation/semantic_probes.sh \
    --log /tmp/spectr-rt-runtime.log \
    --screenshot planning/screenshots/spectr-native-latest.png

# JSON output for downstream consumers:
tools/import-validation/semantic_probes.sh \
    --log /tmp/spectr-rt-runtime.log \
    --screenshot planning/screenshots/spectr-native-latest.png \
    --json

# Enforce trace presence (turn the trace probe from advisory → hard gate):
tools/import-validation/semantic_probes.sh ... --require-trace
```

The script is intentionally decoupled from `spectr-roundtrip.sh` — it
operates on artifacts produced by an earlier run, so the same probe can
score a screenshot collected anywhere (CI run, hand-captured PNG, frame
extracted from a video). The roundtrip script's contract — write
`/tmp/spectr-rt-runtime.log` and `planning/screenshots/spectr-native-latest.png`
— is the integration surface.

### Exit codes

- `0` — every probe that ran passed (skipped probes do not fail).
- `1` — at least one probe failed.
- `2` — invalid arguments / artifacts unusable.

### Dependencies

- `python3` + `Pillow` (already required by `diff_against_reference.py`).
- `diff_against_reference_regions.py` (PR #1871) for the canvas probe —
  falls back to whole-frame blank detection from `diff_against_reference.py`
  if the per-region script isn't yet present.
- The trace probe expects the Spectr bridge to emit `phase=<name>` lines
  to the runtime log via `__spectrLog`. While that instrumentation is
  still landing, the probe is advisory by default; pass `--require-trace`
  to enforce.
