# Goal — finish Forge Modular

Paste the block below as a prompt. It assumes nothing about the session that
wrote it.

---

Finish building **Forge Modular** — a sibling to Forge for VCV Rack that turns
a prompt into a Eurorack module or a whole patch.

Work in `/Volumes/Workshop/Code/pulp-modular-rack` on branch
`explore/modular-rack`. Nothing is on `main` and nothing should go there
without being asked.

**Read first, in this order:**

1. `planning/2026-07-29-forge-modular-build-status.md` (in the pulp-planning
   repo) — what is proven, what remains, and the findings that cost time.
2. `DECISIONS.md` — the arguable calls and what would change our mind about
   each. Do not silently re-decide any of them; if you think one is wrong, say
   so and why.
3. `planning-draft-forge-modular-ux.md` — the full spec. §11 is measured
   capabilities, §12 is agent settings, §13 is the DAW plugin.

**Keep the status document current.** Update it as work lands, including
anything that turns out to be wrong. It is the handoff.

## The standing rule, learned expensively

Every gate written for this pipeline has been wrong the first time it met real
material. Two manifest rules rejected correct modules. The behavioural gate
failed six of eleven working ones. The capability preflight read "hat" out of
"that" and refused an ambient drone. The patch explainer described a correct
cross-modulation patch as an oscillator modulating itself. Every one was found
by running it; none by reading it.

So: **anything that checks, rejects or explains ships with a corpus it must
pass and a negative control it must fail.** A check that rejects everything
scores perfectly on negative cases alone, which is why failing for the *wrong
reason* must also count as a failure. This is not optional polish — it is the
only thing that has reliably worked here.

Prefer running something over reasoning about it. Where a claim can be
measured, measure it.

## Order of work

**Independent of anything else:**

1. Put a small data adapter between `tools/rack/export_design_data.py` and
   whatever consumes it, so the browser-capture envelope can change without
   dragging the preview with it.
2. Make "used no Pulp DSP" a failure rather than a warning, with a
   `{type, reason}` waiver whose reason names the specific blocker — not "not
   supported yet" but what is actually hard.
3. Land `tools/dsp_vocabulary.py` and `tools/test_dsp_vocabulary.py` on Pulp
   `main` as their own small PR. Forge's exporter consumes the extractor and
   should not depend on an unmerged, off-by-default branch.
4. Fetch the Rack SDK silently during install, with exactly one licence
   checkbox. No wizard, no first-run panel. All three platform URLs are live,
   unauthenticated, ~40 MB.
5. Per-patch settings — module preference, module-creation opt-in (off by
   default), explanation depth (standard by default) — persisted so reopening
   a project restores how it was built.

**Once the HTML-import work lands** (ask before assuming it has):

6. The app shell — two tabs, chat, a preview compositing real panel images at
   true widths with cables drawn between recorded jack coordinates, the
   mention picker, and the model picker and agent settings inherited from
   Forge. The imported design system is in `design/prototype/`.
7. Our own Eurorack knobs, jacks, switches and sliders, replacing Rack's
   component graphics — which are CC BY-NC and put a non-commercial obligation
   on the artwork of every module someone builds.

**After the shell exists:**

8. The DAW plugin — audio effect, AU + VST3 + CLAP, as a thin client on the
   shared engine. Verify whether Logic's AU sandbox blocks spawning a compiler
   before committing to the shape.
9. Report collisions: `lsof` on the installed `.vcvplugin` names every process
   holding it, so regenerating while Rack has it open says so rather than
   appearing to do nothing.

## Things that are true and easy to get wrong

- **A new module needs a Rack restart.** `plugin::init()` runs once and the
  plugin API is read-only. A patch, by contrast, loads instantly. This
  asymmetry shapes both flows; do not design around a hot-reload that does not
  exist.
- **Rack does not silently drop modules it cannot find.** It names them, offers
  to open the VCV Library, and keeps the modules and their cables so installing
  later completes the patch. Our checks are a courtesy while building, not a
  rescue.
- **Rack unpacks a `.vcvplugin` only when it loads it**, so a plugin installed
  since the last Rack run is still an archive and will look entirely
  uninstantiable.
- **Nothing on disk describes a module's ports.** Index, name and jack position
  exist only in compiled widget code. The MAP module records them from inside a
  running rack. Index order is *not* visual order.
- **Model slugs are not unique across the library.** Fundamental also ships
  VCO, VCF, VCA and LFO.
- **No plugin can instantiate another plugin or tell its host to open a file.**
  Standalone Rack can be launched and handed a patch; a Rack Pro instance in a
  DAW can be neither.

## Etiquette

Launching Rack opens an audio device. Say so in the message that dispatches it,
cap the run, and quit gracefully rather than killing it — a hard kill truncates
Rack's log and triggers a crash-recovery modal that swallows the next patch
argument.

Do not run a generation that reinstalls the plugin while Rack is reading it.
That crashed Rack mid-screenshot once and looked like a Rack bug for a while.
