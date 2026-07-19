# host-probe — the bench oracle lane

`au_instrument_probe` loads an installed Audio Unit instrument, renders it
offline, and writes a WAV. It is generic SDK tooling for measuring a licensed
or otherwise locally installed reference instrument as a black box.

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

## Feeding the measurement back

The probe renders; `test/support/interaction_residual.hpp` measures. The two
halves of the interaction-residual experiment — render a pair of hits, then
render each hit alone, then difference them — map onto `--hits "0:100,60:100"`
versus `--hits "0:100"` and `--hits "60:100"` at a fixed `--seconds`.

The metric itself is validated against synthetic voices in
`test/test_interaction_residual.cpp`, which run everywhere and need no licence.
That split is deliberate: the tool is proven correct by construction in CI, and
this lane only supplies the targets it is aimed at.
