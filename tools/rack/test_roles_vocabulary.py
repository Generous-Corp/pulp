#!/usr/bin/env python3
"""What the role vocabulary must accept, and must still refuse.

Ten generated patches were rejected as "not a sequenced-voice patch" while
every one of them had a sequencer's pitch reaching an oscillator and something
clocking the sequencer. Two were played through the fidelity harness: four and
five distinct pitches, each note within 0.07 semitones of the pitch its CV
asked for. The pairing the check ACCEPTED played identically to one it
rejected -- same four pitches, same tracking. The patches were right and the
vocabulary was wrong.

Widening a role is the change that can quietly turn a gate into a rubber
stamp, so every case here comes in two halves: the correct patch that must now
pass, and the broken one that must still fail. A file of accepting tests would
pass just as well against a check that accepts everything.
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import idiom_check  # noqa: E402

ROLES = idiom_check.load_roles()
FAILED = 0


def ok(msg: str) -> None:
    print(f"  ok     {msg}")


def wrong(msg: str) -> None:
    global FAILED
    FAILED += 1
    print(f"  WRONG  {msg}")


def check(cond: bool, msg: str) -> None:
    ok(msg) if cond else wrong(msg)


def port(kind: str, role, label: str | None) -> bool:
    return idiom_check._port_matches(kind, role, label, ROLES)


def module(role: str, tags: list) -> bool:
    return idiom_check._module_matches(role, {"tags": tags}, ROLES)


def main() -> int:
    # A clock that publishes the bare tag. 11 of the 78 clock-looking modules
    # installed spell it this way, and a model asked for a melody reached for
    # one of them in 3 of 5 attempts.
    check(module("clock", ["Clock"]),
          "a module tagged Clock is a clock")
    check(module("clock", ["Clock generator"]),
          "the two-word spellings still count")
    check(not module("clock", ["Reverb"]),
          "a reverb is still not a clock")
    check(not module("clock", []),
          "a module with no tags at all is not a clock")

    # A pitch CV is a control voltage. The best-named pitch output on the
    # machine -- "Pitch CV (1V/oct)" -- failed the requirement that a
    # sequencer's pitch reach an oscillator, while a jack called "Step CV"
    # passed, so the vocabulary rewarded the vaguer name.
    check(port("cv_out", "Pitch", "Pitch CV (1V/oct)"),
          "a Pitch output satisfies cv_out")
    check(port("cv_out", "Cv", "Step CV"),
          "an ordinary Cv output still satisfies cv_out")
    # cv_out deliberately does NOT refuse an audio-rate source, unlike
    # clock_out. In a modular a control voltage and an audio signal are the
    # same voltage and only where it is patched differs, which is exactly what
    # amplitude modulation relies on: fixed-modulator-am and
    # tracking-modulator-am both send an audio-rate source through cv_out into
    # an oscillator. Ruling out Audio here would make AM unsatisfiable.
    check(port("cv_out", "Audio", "OUT"),
          "an audio-rate source may be a CV, which is what AM patches need")

    # Our own LFO publishes "Square" and our manifest gives that jack an
    # explicit role of Cv, which overrides inference -- so cartographing the
    # module made it worse off than a vendor LFO nobody cartographed, whose
    # Square infers as Cv+Clock+Gate+Trigger and satisfied this outright.
    check(port("clock_out", "Cv", "Square"),
          "a square output labelled in full can clock something")
    check(port("clock_out", ["Cv", "Clock", "Gate", "Trigger"], "Square"),
          "an uncartographed square still clocks, as it always did")
    check(port("clock_out", "Clock", "CLK"),
          "an explicit clock output still satisfies clock_out")

    # THE GUARD. Widening clock_out's labels to the spelled-out forms is
    # exactly what let an oscillator's audio-rate pulse read as a clock once
    # before: same label, but it would retrigger an envelope thousands of
    # times a second. gate_out carries not_ports for this reason and
    # clock_out did not.
    check(not port("clock_out", "Audio", "Square"),
          "an OSCILLATOR's audio-rate square is refused as a clock")
    check(not port("clock_out", "Audio", "Pulse"),
          "an oscillator's audio-rate pulse is refused as a clock")
    check(not port("clock_out", "Audio", "PLS"),
          "the abbreviation is refused at audio rate too")

    # Nothing above may make an unpatched jack acceptable.
    check(not port("clock_out", None, None),
          "a jack with neither role nor label clocks nothing")
    check(not port("cv_out", None, None),
          "a jack with neither role nor label carries no CV")
    check(not port("pitch_in", "Audio", "IN"),
          "an audio input is still not a pitch input")

    print()
    print("all good" if not FAILED else f"{FAILED} wrong")
    return 1 if FAILED else 0


if __name__ == "__main__":
    raise SystemExit(main())
