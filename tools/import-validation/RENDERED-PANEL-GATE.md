# Scoring the artifact that ships

`verify_rendered_panel.py` scores an **emitted UI artifact** — the `ui.js` a
plugin actually loads — against the render its importer claimed to produce.

The failure it exists to catch: the import pipeline reports a high similarity
for the DesignIR render it built in memory, while the artifact that ships
renders to something else entirely. A panel measured at **0.13** was certified
at **0.98** by scoring the pipeline and calling it the product. So the subject
is always the artifact on disk, passed as a parameter, and the reference is the
importer's own render. Divergence between them is the bug.

The artifact path stays a parameter deliberately: the same gate scores a
generated panel and an installed one, and the installed one is the interesting
case — an asset whose path resolves during the run can be gone by the time the
plugin opens.

```bash
tools/import-validation/verify_rendered_panel.py \
  --artifact  <project>/build.ui.js \
  --reference <capture>/validation-proof/render/render.png \
  --tokens    <capture>/tokens.json \
  --width 900 --height 602 --scale 2
```

Exit codes distinguish "the panel is wrong" from "the harness could not measure
it": `0` pass, `2` bad input, `3` render failed, `4` size mismatch, `5` below
threshold, `6` a direct assertion failed, `7` the harness could not run a check
it was asked to run.

## Three checks, three different jobs

None subsumes another and no two overlap. A reviewer expecting the wrong one to
fire will read a green run as broader than it is.

| check | catches | blind to |
|---|---|---|
| **similarity** | the backdrop class — artwork missing or misplaced | palette regressions |
| **foreign colour** | the palette class — widgets painting outside the design's tokens | everything else |
| **`assert_image_sources`** | *no source emitted* for an image node | *source emitted and ignored* |

**Similarity catches the backdrop class and nothing finer.** A panel whose
capture backdrop never reaches the renderer scores **0.134**; the same panel
fixed scores **0.982**.

**Foreign colour catches the palette class, and nothing else does.** This is the
non-obvious one. An artifact that published its tokens under the design's own
names (`css/accent`) rather than the keys widgets resolve (`knob.arc`,
`accent.primary`) renders every control on the built-in default palette — blue
knobs on a cream faceplate — and **scores 0.940226 on similarity, a pass**. Its
foreign-colour ratio is **0.1931** against a correct panel's **0.0485**. No
similarity threshold separates 0.940 from 0.982 without firing on antialiasing.

**`assert_image_sources` reads the artifact's TEXT**, so it catches an image
node emitted with no source. It structurally *cannot* catch a source emitted and
then ignored downstream — the known-bad artifact above passes this check,
because its `src` is present and the consumer was what broke. Do not expect it
to catch a `src` regression; similarity covers that.

## The reference is the DesignIR render, never the browser capture

Compare against the importer's own `validation-proof/render/render.png`, not
Chrome's `browser.png`: an installed panel deliberately overlays live widgets on
the capture, so against the browser image it can never be pixel-equal and a
threshold there sits permanently red.

Dimensions are asserted **before** scoring, because a size-mismatched pair does
not error — it scores the intersection and returns a plausible number. Measured:
a 2560×1708 capture against an 1800×1204 render returns
`similarity=0.260396 … FAIL`. That is exactly the shape of comparing an import
against Chrome's raw capture, which can never match by construction.

## `PULP_SHOT_NO_RECONCILE=1` is mandatory

The script forces it. `pulp-screenshot` otherwise reconciles oversize
absolutely-positioned descendants to the capture viewport, and a faithful
capture backdrop is precisely that shape — a literal width with no
opposite-edge anchor, carrying bound controls positioned against it.
Reconciling rescales the artwork out from under the controls.

Nothing errors. The run simply scores a different image than the one on disk,
and **the error runs in both directions**: on one panel the clamp scored a
defective render 0.33 and its fix 0.44, where the unclamped truth was 0.13 and
0.98. It cannot be corrected for after the fact, which is why it is forced here
rather than left to the caller.

## The renderer must come from the same tree as the importer

A `pulp-screenshot` older than the artifact silently no-ops runtime calls it
does not implement, renders stock defaults, and scores near zero against a
styled design — indistinguishable from a catastrophic regression when it is a
build-order mistake. `assert_renderer_can_execute` refuses to score in that case
(exit 7), reading the binary's own string table rather than its build date: the
question is not how old it is but whether it implements what this artifact
calls.

Not hypothetical. A binary predating the fix under test was offered for a
validation run; using it would have rendered the *fixed* artifact without the
fix and produced a low score that looked exactly like the fix failing to work.

## Verify a harness's argv the way you verify its output

A wrapper that mangles arguments fails indistinguishably from the thing under
test. A shell helper passing `"--tokens /path"` through an unquoted `$2` sends
one argv entry rather than two under `zsh`, which does not word-split unquoted
parameter expansions, and the run exits 2 on a usage error that reads as a
broken script. Same family: `${PIPESTATUS[0]}` does not exist in `zsh` and
prints an empty exit code, and piping a command's output masks its exit status
entirely.

When a harness reports something surprising, confirm it ran what you think it
ran before believing what it says about the subject.

## Acceptance record

The gate's red has been demonstrated, not asserted. Every row is a real run
against a real artifact.

| case | artifact | similarity | foreign colour | exit |
|---|---|---|---|---|
| backdrop missing | known-bad | **0.134111** | — | 5 below threshold |
| palette wrong | known-bad | **0.940226** *(passes)* | **0.1931** | **6 assertion** |
| correct panel | fixed | **0.982288** | **0.0485** | **0** |

The middle row is the acceptance test that matters: similarity passes and the
run still fails, with output that says so —
`FAIL: similarity 0.9402 passed but 1 direct assertion(s) failed`.

Harness faults, each verified to exit **7** rather than pass:

| condition | why it is not a panel failure |
|---|---|
| no `--tokens` | the allow-list is absent, so the check cannot run |
| `--tokens` given, palette parses empty | the check would be vacuously green |
| PIL not importable | the check cannot run; system `python3` may be PEP-668 blocked |
| renderer missing runtime API the artifact calls | stale binary, not a panel regression |

`--skip-colour-check` exits **0** and still scores similarity — a deliberate
omission, stated explicitly and visible in the command line.

## Thresholds

Similarity floor defaults to `0.85`, foreign-colour budget to `0.10`. The budget
sits roughly 2x either side of the measured pair (correct ~0.05, broken ~0.19).
It reads higher on designs carrying more soft-edged chrome — antialiased edges
and gradient interpolation legitimately produce colours outside the token set,
so a blurred or photographic backdrop sits above a flat one. The ratio prints on
every run, pass or fail, so drift is visible before it trips.

## Relationship to the other scripts here

`diff_against_reference.py` and `diff_against_reference_regions.py` score two
PNGs that a caller has already produced, for the Spectr roundtrip. This script
owns the step before that: it *renders the artifact itself*, under the
conditions that make the render faithful, and refuses to score when it cannot.
