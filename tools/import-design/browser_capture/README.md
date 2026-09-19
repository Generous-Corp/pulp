# Browser capture helper

This helper evaluates an authorized HTML document in an isolated Chromium
process and writes a replayable `pulp-browser-capture-v1` envelope. It is an
import-time tool; generated Pulp plugins do not embed or require Chromium.

The C++ launcher in `browser_capture_backend.{hpp,cpp}` is the supported entry
point. It discovers and probes a compatible browser, creates a fresh temporary
profile, and invokes `capture.mjs` through Pulp's argv-safe `ChildProcess`.
`capture.mjs` uses only Node.js built-ins and requires Node.js 22 or newer.
The public CLI opt-in is `--allow-browser-network`; the helper also accepts that
spelling when invoked directly by importer developers, while the launcher uses
its internal `--allow-network` spelling.

The caller supplies an `input_file` inside an authorized `staged_root`. The
helper serves that root from a random tokenized loopback URL, so relative
scripts, styles, fonts, and images continue to work. Each served path is
realpath-checked against the root, including symlinks. External requests are
blocked through CDP unless the caller explicitly enables them.

Successful output is:

```text
<output>/
  capture.json
  browser.png
  dom-snapshot.json
  semantic-report.json
  tokens.json
```

The viewport starts at `--initial-width` and grows once, by both clipped
margins, when the settled layout centres content past its left or top edge.
`--width` replaces that correction with a width the caller already knows the
layout resolves at, which is the lever for a responsive shell whose own width
changes at a breakpoint the single bounded step lands inside. The two are
mutually exclusive, and `provenance.viewport.width_pinned` records which
decided the capture. No width recovers content anchored left of the document
origin (an absolutely positioned element at a negative left, `position: fixed`,
or a negative margin on `<html>`), and a capture refused for that reason says
so rather than naming a flag that cannot help. Raw bounds from a descendant
clipped by an overflow-hidden ancestor are another non-width cause and are
named explicitly rather than misdiagnosed as responsive layout. The supported `pulp
import-design` entry point maps an explicit `--render-size WxH` to this pinned
width; its default viewport remains an automatically correctable initial width.

`--interactions <plan.json>` optionally reaches one deterministic secondary
state before the same-frame evidence capture. Plans use
`pulp-browser-interactions-v1` and contain only bounded `click`,
`context-click`, `type`, `wait-for`, and `wait-ms` actions. `context-click`
uses a real secondary-button press/release. The helper records selectors and typed-text
length in `interaction-report.json`; it persists neither typed plaintext nor a
per-action text hash. The published plan identity hashes a canonical redacted
plan in which typed text is replaced by its length, so short private values
cannot be recovered by hashing candidate plans. Same-document history and
fragment routing are allowed; loading another document or opening a popup
remains forbidden. Typed text still becomes live rendered page state and may
therefore appear in screenshots, DOM/semantic evidence, or tokens. Never put a
password, credential, private draft, or other secret in an interaction plan.
Sources with a distinct asynchronous boundary after the last action may expose
`globalThis.__pulpInteractionReady` as a Promise or one-shot function; the
initial `__pulpCaptureReady` contract is never invoked twice.
Action timeouts remain inside the capture-wide
`--timeout-ms` deadline and cannot extend it. See
`interaction_plan_protocol.json` for the exact schema. With no plan, capture
retains its initial-state behavior and output set.

For a reproducible Forge Modular secondary-state proof, use
`test/fixtures/browser_capture_forge_modular_mentions.json` with the source
export and pixel validation:

```bash
pulp import-design \
  --file /path/to/ForgeModular.dc.html \
  --browser-interactions test/fixtures/browser_capture_forge_modular_mentions.json \
  --allow-browser-network \
  --emit ir-json \
  --output /tmp/forge-modular-proof.json \
  --validate \
  --screenshot-backend skia
```

The captured and Skia-rendered frames must both show the module mention picker,
and validation must report zero differing pixels.

`capture.json` conforms to `capture_protocol.json`. The DOM snapshot is kept as
a sidecar because it can be large; the envelope references it by relative path.
`tokens.json` preserves active light / no-preference computed CSS custom
properties as colors, true
pixel dimensions, or strings. Relative units and expressions are never coerced
to pixels. Selector-specific theme overrides are not promoted into the current
single-mode DesignIR token map; the authored HTML/CSS remains the source for
that follow-up rather than pretending a computed default is multi-theme.
Loopback ports, random tokens, browser executable paths, and host filesystem
paths are not recorded in the envelope.

With explicit network access, the launcher admits only public HTTPS origins
declared by the bounded staged source graph. Provider-owned secondary origins
must also be present in the audited dependency registry; currently this covers
Google Fonts stylesheet responses loading font bytes from `fonts.gstatic.com`.
All admitted hosts are resolved to public addresses and pinned before Chromium
launches, and every fetched response is content-hashed in the capture envelope.

Failures are nonzero and write `capture-error.json` when an output directory is
available. The helper never selects a lower-fidelity importer.

The resolved browser build is written to stderr as a `[browser-capture]` line
before any page work, so a capture that never produces an envelope still says
which Chromium ran. Chromium's screenshot and virtual-time behaviour differs
between releases, and a failure report that does not name the browser cannot be
triaged. A capture that reaches its deadline reports the phase it died in, the
last browser call that completed, and the calls still in flight; the same
detail is written to `capture-error.json`.

Arbitrary JavaScript evaluation is intentionally not a CLI escape hatch: it
would weaken the source/evidence boundary and make captures difficult to
reproduce or audit.

## Offline text mismatch evidence

From a source checkout, build an optional diagnostic sidecar from an existing
capture (no browser launch, capture instrumentation, or DesignIR change):

```sh
node tools/import-design/browser_capture/text_diagnostics.mjs \
  /path/to/capture/dom-snapshot.json /path/to/capture/platform-fonts.json \
  > /path/to/text-diagnostics.json
```

`pulp-text-diagnostics-v1` records the requested font, resolved owner-element
face census and glyph counts, snapshot text-fragment rectangles and UTF-16
ranges, and computed CSS line-height. Face counts are deliberately not summed
across runs: sibling text runs can repeat the same owner census. Multiple faces
show participation, but do not identify which character used which face or
prove that a CSS family alias failed. The report preserves the census rather
than declaring a guessed fallback. `normal` remains a CSS value, not a guessed
pixel height.

Every metric has `status: observed` with `source` and `value`, or
`status: unavailable` with `reason`. Current CDP snapshots do not provide
per-glyph ink bounds/advances, laid-out baselines, face ascent/descent, or full
CSS inline line boxes. These fields remain explicitly unavailable. Snapshot
text-fragment rectangles are not glyph ink bounds or full CSS line boxes;
fragment width is not an advance. Do not infer a baseline from font size or
clamp negative half-leading to manufacture measured evidence.

The report covers the primary document and preserves the font census's
truncation summary. Run IDs are capture-local `document:0/layout:N`; rectangles
remain in primary-document snapshot CSS pixels, independent of screenshot DPR.
`capture_basis` is SHA-256 of the concatenated ASCII SHA-256 digests of the two
input files, snapshot first. It identifies retained inputs, not a native render
or proof that independently supplied inputs were captured together. Supply
both files from the same retained capture directory.

`compareTextDiagnostics(reference, candidate, tolerance = 0.25)` is an exported
adapter seam. A future native producer must explicitly join reference run IDs,
text, capture basis and coordinate space, and report the same metric meanings.
Use arrays for glyph metrics and baselines, numbers for ascent/descent, and
`{bounds: [x,y,width,height], start, length}` records for fragment/line boxes;
keep run-relative UTF-16 ranges exact. Metrics requiring additional conventions
(such as glyph order and font-metric units) need an agreed producer contract
before comparison. This helper does not collect native metrics or establish
native parity. It returns per-metric `match`, `mismatch`, or `unavailable`, and
`incomparable` for missing/extra runs or changed text. Different capture bases
or coordinates are rejected. Geometry uses CSS-pixel tolerance; ranges, font
counts, identities, and CSS strings compare exactly. Missing metrics never
count as matches, including in a self-comparison. Coverage accompanies findings;
there is no overall pass that could hide truncation or missing instrumentation.

The diagnostic module is a source-checkout analysis tool, not a shipped capture
runtime dependency. Its tests join the existing `pulp-browser-capture-node-unit`
CTest glob and can also run with `node --test` directly.

### Native observation and the bounded coordinate join

`pulp-design-ir-observe --text-diagnostics <json>` enables selectable geometry
on its disposable imported tree before the normal Skia render, then emits
`pulp-native-selection-diagnostics-v1` after paint. With the option absent the
observer does not enable selection or emit this sidecar. The data comes from
`Label::selectable_layout()`: UTF-8 ranges, shaped caret X positions, selection
band top/height, anchor, and the effective font request. It never calls a font
request a resolved face. Unpainted layouts, transformed/hidden ancestors,
child paint offsets, transformed text, attributed text, and non-LTR labels do
not produce comparable coordinates.

`compareNativeSelection` in `native_text_diagnostics.mjs` requires an explicit
one-to-one browser-run/native-anchor join, exact source text, capture basis,
native input path, and native root origin in browser CSS pixels. Correspondence
is caller-owned; matching strings alone is not an identity or transform proof.
The caller must establish an untransformed horizontal LTR single-line fixture.
The initial adapter accepts only complete printable ASCII ranges without
leading/trailing spaces, with a single CDP fragment and a single painted native
selection line. UTF-8 and UTF-16 ranges therefore coincide in this bounded lane.
It rejects partial, unpainted, reversed, ambiguous, or unsupported ranges.

Only full-range horizontal endpoints are compared. Native selection bands and
CDP fragment heights have different meanings, so their vertical coordinates
are never scored against each other. The font diagnostic compares the native
paint request to exact names in the browser's resolved census. A name mismatch
is actionable request evidence, but aliases can differ and it does not prove
a native fallback. This report does not establish arbitrary import parity.

Run the retained browser/native experiment from the source checkout:

```sh
node tools/import-design/browser_capture/text_diagnostics_native_proof.mjs \
  /path/to/pulp-design-ir-observe /path/to/chromium /path/to/new-evidence-dir
```

It captures a known HTML fixture, builds explicitly paired DesignIR fixtures,
then uses the observer for three actual native Skia renders: reference, a font
intervention, and a +12 logical-pixel translation. The proof requires the font
intervention to change measured native geometry and the translation to shift
both range endpoints by 12 relative to the native reference. An existing
browser/native reference mismatch is retained rather than asserted away.
Artifacts include source HTML, IR inputs, screenshots, raw observations, joins,
comparisons, and a receipt with executable/input hashes. A receipt is emitted
only after all three native renders and sensitivity assertions succeed.

The fixture also retains an independent browser Canvas2D `measureText` probe:
whole-string advance/ink bounds and font-box ascent/descent, with missing fields
represented by null. The font-ready probe uses the fixture's computed font.
These are **probe metrics**, not a DOM baseline, per-glyph ink, or the metrics of
every fallback face. Measuring isolated characters would alter shaping and
cannot manufacture per-glyph evidence for the laid-out string.

Producer limits requiring separate work:

- The Label selection seam does not expose painted baselines, resolved native
  faces, or per-glyph ink. `baseline_y()` is a layout baseline and can differ
  from paint after vertical alignment; it is not a substitute.
- `TextShaper::PreparedText` exposes ascent/descent with a real-metric flag, but
  a new prepare call is an independent probe, not the Label's painted run.
- `SkiaCanvas::measure_text_full` can return estimates without a validity flag;
  its whole-string font bounds also do not identify fallback glyphs. Calling it
  separately must not be promoted to an observed painted-glyph census.
- CDP text boxes and owner face counts do not expose actual DOM baselines or
  character-to-face attribution. A DOM range rectangle remains layout geometry.
- General joins need producer-owned DOM-to-native identity and full transform,
  scroll, bidi, wrapping, shaping, and UTF-8/UTF-16 correspondence. The bounded
  fixture adapter refuses to stand in for those contracts.

The generic implementations owning any new painted-run instrumentation are
outside this observer change. The observer consumes existing APIs only.
