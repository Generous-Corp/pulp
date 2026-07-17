# host-probe — the bench oracle lane

`au_instrument_probe` loads an installed Audio Unit instrument, renders it
offline, and writes a WAV. It exists so that claims about how a real drum
machine behaves can be *measured* instead of remembered.

Rendering is driven from the calling thread through `AudioUnitRender`. No
output device is ever opened, so a probe run is silent and safe to invoke from
an SSH session or a background agent.

## What this lane is, and what it is not

SPICE and the published circuit equations are what gate CI. This lane is a
**bench oracle**: it supplies real-world numbers to aim at and to check
ourselves against. The distinction matters in two directions.

**It is not a gate.** The reference instruments are commercial plugins licensed
to one machine (see below). Every test that depends on them must skip — loudly,
with a named reason — when they are absent, and a skip is never a pass. A gate
that silently passes on every machine except one is worse than no gate.

**It is not a source of design.** Measuring a reference to learn *what* it does
is fine. Deriving our implementation from it is not, and that includes fitting
our parameters to its output: an optimizer pointed at a reference render is
still reverse-engineering, just automated. Our lineage is Werner's published
equations, the published schematic, and SPICE. When a measurement and the
circuit disagree, the answer is to go re-read the circuit, not to tune until the
curves overlap.

**Rendered audio from a commercial plugin is never committed.** What lives in
the repo is the recipe and the measured numbers. Anyone holding their own
licence regenerates the audio locally and should get the same numbers; anyone
without a licence still gets the numbers, the method, and every CI gate.

## Availability

The reference instruments (TR-808/909/707/727/606, TB-303) are installed at
`/Library/Audio/Plug-Ins/Components/` on **m3 only** — nowhere else in the
fleet. Treat any other host as not having them.

Working renders and loop scripts live in `~/pulp-808-refs/` on that machine.
That directory is scratch, outside the repo: read from it, never copy audio out
of it into version control.

## Usage

```bash
./build/tools/audio/pulp-au-instrument-probe \
    --name "TR-808" \
    --note 36 \
    --seconds 2 \
    --hits "0:100,60:100" \
    --set-param 6293508=128 \
    --out /tmp/x.wav
```

`--hits` takes `MS:VEL` pairs — **milliseconds**, not samples and not beats. The
example above is a pair of velocity-100 hits 60 ms apart.

`--list-params` dumps the loaded instrument's parameter IDs, which is how the
IDs below were found. TR-808 bass drum:

| Parameter | ID |
|-----------|-----|
| BD TUNE   | 6293508 |
| BD DECAY  | 6293510 |
| BD LEVEL  | 6293512 |
| BD TONE   | 234864640 |

## Feeding the measurement back

The probe renders; `test/support/interaction_residual.hpp` measures. The two
halves of the interaction-residual experiment — render a pair of hits, then
render each hit alone, then difference them — map onto `--hits "0:100,60:100"`
versus `--hits "0:100"` and `--hits "60:100"` at a fixed `--seconds`.

The metric itself is validated against synthetic voices in
`test/test_interaction_residual.cpp`, which run everywhere and need no licence.
That split is deliberate: the tool is proven correct by construction in CI, and
this lane only supplies the targets it is aimed at.
