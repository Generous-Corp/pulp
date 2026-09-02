# A real run that kept a dead module for four attempts

Two attempts from one run of *"simple highly melodic patch using only CV funk
modules"*, kept exactly as the gate wrote them. `test_handover.py` reads them.

The patch is **correctly wired** by every jack name:

```
Hammer "Main Clock"        -> StepWave "Clock"
StepWave "Sequencer CV"    -> Zephyr "V/Oct (poly)"
StepWave "Sequencer Gate"  -> EnvelopeArray "Gate 1 CV"
EnvelopeArray "Env. 1"     -> PressedDuck "Chan. 1 VCA CV"
Zephyr "Audio (poly)"      -> PressedDuck "Chan. 1 L"
PressedDuck "Main Out L/R" -> AudioInterface
```

and it reads silent: `CVfunkSands/Zephyr` reports `out0=0.000 out1=0.000`, and
every module after it reads zero as a consequence.

## Why it read zero, which is not what this file used to say

Zephyr is not broken. The gate that wrote these two reports never resolved VCV
licence keys, so every licensed plugin it loaded constructed normally, ran its
DSP, and wrote zero to every output. See "A licensed module runs, writes zeros,
and logs nothing" in the `forge-modular` skill for the mechanism and the
ordering it depends on.

`tools/rack/licence-fix-replay.json` replays the whole corpus across the old
gate and the fixed one, and both of these patches are in it:

| sha256 prefix | pre-fix gates | fixed gate, no user dir | fixed gate |
|---|---|---|---|
| `320888380c9a` (attempt01) | silent | silent | audible |
| `d41dfbf51c74` (attempt03) | silent | silent | audible |

The middle column is the control that names the mechanism rather than the
rebuild: a fixed gate with no user directory to read keys from still reports
silent. Patches that never load a licensed module read the same in all four
lanes, so the lanes are not flipping everything.

So replacing the oscillator was never the repair, and attempt 5 escaped by
coincidence rather than by choosing a better module.

## What the fixture still proves

All of it, unchanged. A model reading this report cannot see the harness that
produced it, only the readings, and the retry has to name the FIRST module in
the chain reading zero rather than the module the FAIL line points at. Zephyr
is the correct answer for this report. The run kept it for four consecutive
attempts and adjusted its knobs instead, with all of its params in range and
none at a silencing zero, so retuning was never going to work.

Attempts **1 and 3**, not 1 and 2, on purpose: StepWave and EnvelopeArray read
differently between them while Zephyr is dead in both. A test that replayed one
report twice would pass on two identical strings and could not tell that the
repetition is being detected on the MODULE.

Why the files are here rather than regenerated: reproducing them costs five
model calls and a machine with CV funk installed, and the numbers are the
evidence. Do not hand-edit them; they are a transcript, not a fixture that can
be adjusted to suit a test. That includes correcting them now that the cause is
known. The reports are what the gate wrote.
