# host-probe — the bench oracle lane

Two probes, same lane and same rules:

- `au_instrument_probe` loads an installed Audio Unit **instrument**, renders it
  offline, and writes a WAV.
- `au_effect_ab` renders one deterministic source through either an installed
  Audio Unit **effect** or one of Pulp's own `CharacterDelay` engines, and writes
  a WAV. Running it once per engine with the same source and `--seconds` yields
  renders the audio quality lab can compare directly.

Both are generic SDK tooling for measuring a licensed or otherwise locally
installed reference as a black box.

Rendering is driven from the calling thread through `AudioUnitRender`. No
output device is ever opened, so a probe run is silent and safe to invoke from
an SSH session or a background agent.

## What this lane is, and what it is not

Published specifications, equations, and independently authored models remain
the implementation authority. This lane is a **bench oracle**: it supplies
real-world measurements to compare with those implementations. The distinction
matters in two directions.

**It is not a gate.** The reference instruments are commercial plugins licensed
to one machine (see below). Every test that depends on them must skip — loudly,
with a named reason — when they are absent, and a skip is never a pass. A gate
that silently passes on every machine except one is worse than no gate.

**It is not a source of design.** Measuring a reference to learn *what* it does
is fine. Deriving an implementation from it is not, and that includes fitting
parameters to its output: an optimizer pointed at a reference render is still
reverse-engineering, just automated. When a measurement and the documented
model disagree, investigate the model and experiment instead of tuning until
the curves overlap.

**Rendered audio from a commercial plugin is never committed.** What lives in
the repo is the recipe and the measured numbers. Anyone holding their own
licence regenerates the audio locally and should get the same numbers; anyone
without a licence still gets the numbers, the method, and every CI gate.

## Usage

```bash
./build/tools/audio/pulp-au-instrument-probe \
    --name "Reference Instrument" \
    --note 36 \
    --seconds 2 \
    --hits "0:100,60:100" \
    --set-param 1234=0.75 \
    --out /tmp/x.wav
```

`--hits` takes `MS:VEL` pairs — **milliseconds**, not samples and not beats. The
example above is a pair of velocity-100 hits 60 ms apart.

By default, a final render whose peak does not exceed `1e-6` exits with status
5 and prints `RESULT: SILENT render`. This is the normal validation behavior:
an accidentally silent reference render must not look like a successful
measurement. Use `--allow-silent` only when silence is the expected subject of
the experiment, such as a zero-velocity or muted-control baseline. The tool
will then write the silent WAV, exit successfully, and print
`RESULT: SILENT (allowed)` so the exception remains visible in logs.

`--allow-silent` does not make an undiscovered instrument or failed load pass,
and it does not bypass the automatic note sweep. Supply `--note` when the
experiment intentionally targets a silent note or trigger condition.

`--list-params` dumps the loaded instrument's parameter IDs. Capture this output
with the experiment recipe because Audio Unit parameter IDs are chosen by the
instrument vendor and cannot be inferred from display names.

## Usage — `au_effect_ab` (effects)

```bash
# Our engine, and a reference effect, through the identical source.
./build/tools/audio/pulp-au-effect-ab --pulp bbd --time-ms 375 --feedback 0.45 \
    --out /tmp/pulp-bbd.wav
./build/tools/audio/pulp-au-effect-ab --au TYPE:SUBT:MANU --out /tmp/ref.wav
```

A plugin that models several devices usually exposes each one as a **factory
preset** rather than as a host parameter, so a fresh headless instance comes up
with nothing engaged and no parameter set will engage it. Use `--list-presets`
to enumerate and `--preset N` to select before rendering. If a plugin reports
zero factory presets, its selection lives in opaque state and the only headless
route is a saved `.aupreset` restored through `PluginSlot::restore_state()`.

An AU is addressed by **identity** (`TYPE:SUBT:MANU`) rather than by scanning.
The identity is the complete loader descriptor, so this never walks every
installed bundle and never instantiates unrelated third-party plugins. Read the
codes off a bundle with:

```bash
/usr/libexec/PlistBuddy -c "Print :AudioComponents:0:type" \
  -c "Print :AudioComponents:0:subtype" -c "Print :AudioComponents:0:manufacturer" \
  "/Library/Audio/Plug-Ins/Components/<Name>.component/Contents/Info.plist"
```

`--src-gen impulse|pluck|tone` picks a deterministic in-tool source (default
`pluck`), so an A/B needs no asset and is bit-identical across machines;
`--src FILE` uses a WAV instead. `--pulp` accepts `clean`, `tape`,
`tape-physical`, `bbd`, `vintage-digital`, `diffusion`.

**`--list-params` prepares the unit first, on purpose.** An Audio Unit generally
does not publish its parameter list until it is initialised, so enumerating
before `prepare()` reports *zero* parameters for every plugin — including
Apple's own `aufx:dely:appl`, which obviously has four. That reads as "this
plugin exposes nothing" when it means "we asked too early". If a probe ever
reports no parameters, check it against a known-good control such as
`aufx:dely:appl` before concluding anything about the plugin.

### Matching controls before comparing — required, and not the same as fitting

Two delays at their factory defaults are not comparable. Observed on real
plugins: a reference delay shipped in **tempo-sync** mode, which in an offline
render with no transport leaves its delay time undefined; its blend was 50%
dry while the Pulp render was wet-dominant; and its feedback differed. A
spectral difference measured across that is a difference of *settings*, not of
character, and must not be reported as one.

So before comparing, match what you can — delay time, feedback, wet/dry blend,
sync off — and verify the match from the render itself rather than trusting the
control values: an impulse through the engine shows the true echo spacing (Pulp
at `--time-ms 375` measures 372.5 ms between taps).

This is measurement hygiene and is explicitly **not** the parameter-fitting this
lane forbids. Matching two engines' controls so an A/B is apples-to-apples is
legitimate. Sweeping *our* coefficients to minimise error against a reference
render is reverse-engineering, just automated — see the rule above.

Note also that **peak is the wrong metric for a band-limited engine**. An
impulse through a bucket-brigade path is smeared across time by its
reconstruction filtering, collapsing peak while preserving energy: Pulp's `bbd`
peaks ~28 dB below `clean` on an impulse but only ~5 dB below it in RMS on
musical material. Compare energy, not peaks.

## Feeding the measurement back

The probe renders; `test/support/interaction_residual.hpp` measures. The two
halves of the interaction-residual experiment — render a pair of hits, then
render each hit alone, then difference them — map onto `--hits "0:100,60:100"`
versus `--hits "0:100"` and `--hits "60:100"` at a fixed `--seconds`.

The metric itself is validated against synthetic voices in
`test/test_interaction_residual.cpp`, which run everywhere and need no licence.
That split is deliberate: the tool is proven correct by construction in CI, and
this lane only supplies the targets it is aimed at.
