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

and it is silent, because `CVfunkSands/Zephyr` produces `out0=0.000
out1=0.000`. Everything after it is silent as a consequence.

The model kept Zephyr for four consecutive attempts and adjusted its knobs
instead of replacing it. Zephyr's params were all in range and none at a
silencing zero, so retuning was never going to work. It escaped only on
attempt 5, by happening to choose a different oscillator.

Attempts **1 and 3**, not 1 and 2, on purpose: StepWave and EnvelopeArray read
differently between them while Zephyr is dead in both. A test that replayed one
report twice would pass on two identical strings and could not tell that the
repetition is being detected on the MODULE.

Why the files are here rather than regenerated: reproducing them costs five
model calls and a machine with CV funk installed, and the numbers are the
evidence. Do not hand-edit them; they are a transcript, not a fixture that can
be adjusted to suit a test.
