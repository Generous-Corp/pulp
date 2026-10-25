# Importer differential lab: frozen holdout panels

Three real panels, frozen as a holdout for the importer differential lab. They
are never used to tune the importer: a rule written while looking at one of
them would make its score say nothing about panels nobody has looked at. Use
the lab's other fixtures to develop a fix, then read these to see whether it
generalises.

Each panel has an entry in `../manifest.json` with `split: "holdout"`. The
entry records the source SHA-256, the viewport the lab must use, the dynamic
blockers the lab is expected to report, the gaps the panel is known to show,
and any region whose pixels change from run to run. Each panel also has a
`<name>.receipt.json` holding the digests and scores of a three-run lab
campaign. A frozen panel is replaced, never edited: changing one byte changes
its SHA-256 and voids its receipt.

The lab does not read `expected_blockers`, `expected_gaps`,
`volatile_regions` or `split` yet. They are declarations, so a result can be
checked against them by hand until the lab compares them itself.

| Panel | Bytes | What it exercises |
| --- | ---: | --- |
| `spectr-standalone.html` | 1,860,417 | A React app compiled in the page by Babel: header and toolbar over a full-window animated spectrum canvas, letter-spaced JetBrains Mono and Inter, opacity-dimmed groups, inline SVG icons, a disabled `<input type=range>` with `accent-color`, and low-alpha hairlines over gradients. |
| `lattice-panel.html` | 833,090 | A MIDI step sequencer: a 96-cell grid of near-identical shadowed siblings, conic gradients, `mix-blend-mode`, `backdrop-filter`, `filter: blur()` and `color-mix()` in oklab. No script. |
| `kelvin-panel.html` | 494,407 | A synth voice: knobs drawn with `repeating-conic-gradient`, an output meter, and a CSS animation. No script. |

## Provenance

`spectr-standalone.html` is the owner's Spectr product UI, exported from
Claude as a standalone bundle and committed byte for byte. Its `__bundler`
manifest carries 16 assets: React and React DOM 18.3.1 development builds,
`@babel/standalone` 7.29.0, seven Inter 4.001 WOFF2 subsets and six JetBrains
Mono 2.211 WOFF2 subsets. It makes no network request; the Google Fonts URLs in
it are preconnect hints only.

`lattice-panel.html` and `kelvin-panel.html` are
`test/fixtures/agent-panels/lattice/panel.html` and
`test/fixtures/agent-panels/kelvin/panel.html` (see each `PROVENANCE.md`) made
self-contained. The one change is that the
`<link rel="stylesheet" href="styles.css">` line is replaced by a `<style>`
element holding Forge's `tape-machine` design pack:

- source: `Generous-Corp/forge` commit `bbbab33ff8a1d9cfce40e92a8cce2fd9e08b5927`,
  path `data/design_systems/tape-machine/` (last changed at `b53e8b53`)
- `styles.css` flattened in place: each `@import url("...")` line is replaced
  by the imported file, in order, under a `/* <path> */` comment
- each `@font-face` keeps its declaration, with `url('fonts/<file>')` replaced
  by a `data:font/ttf;base64,` URL of that file; every other face is dropped
- lattice keeps `JetBrainsMono-400/500/700.ttf` and `Jost-500/600/800.ttf`;
  kelvin keeps `JetBrainsMono-400/500.ttf` and `Jost-800.ttf`. These are the
  faces Chromium used when every pack face was present (the capture's
  `platform-fonts.json`), so dropping the rest cannot change font matching.
  Kelvin's browser screenshot, native render and native layout digests are
  identical with every face and with only the kept ones.

Removing the `<style>` element and restoring the link line gives back the
committed `panel.html` byte for byte.

Glyphs missing from the embedded faces fall back to the host's system fonts:
Helvetica Neue and Menlo for lattice, and Helvetica Neue and Lucida Grande for
kelvin. These panels were captured on macOS, so a receipt from another OS can
differ for that reason alone.

## Licences

- `OFL.txt`: SIL Open Font License 1.1, for Inter, JetBrains Mono and Jost.
- `LICENSE-React.txt`: MIT, for React and React DOM.
- `LICENSE-Babel.txt`: MIT, for `@babel/standalone`.

These are also listed in the repository's `NOTICE.md`, `DEPENDENCIES.md` and
`docs/reference/licensing.md`. The tape-machine pack's CSS belongs to the
Pulp project owner.

## Running them

```bash
python3 tools/import-validation/importer_differential_lab.py analyze-corpus \
  --importer build/tools/import-design/pulp-import-design \
  --observer build/tools/import-design/pulp-design-ir-observe \
  --manifest test/fixtures/import-differential/manifest.json \
  --timeout-seconds 180 --output /tmp/importer-differential/corpus
```

The lab needs Pillow in the interpreter that runs it. It checks once before
running and names `tools/ci/install_visual_python_deps.sh` when Pillow is
missing. Spectr's import takes about 35 s, so the default 60 s timeout is too
tight on a loaded host.

All three score. Spectr's screenshot and native render digests change on
every run inside its `volatile_regions`, so its determinism verdict is `fail`
while its DOM snapshot and native layout digests hold. Lattice and kelvin are
deterministic: every digest holds over three runs. None of the three is
threshold-eligible; the gaps each one shows are recorded in the manifest.

Pass `--manifest`, as above, so each panel is scored at the viewport its entry
declares. `--input` with `--include-holdout` also scores them, but at the
default 1280x800 viewport. `benchmark` and `compare` take
`--viewport WIDTHxHEIGHT` for a single panel.
