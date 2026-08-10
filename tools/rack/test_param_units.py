#!/usr/bin/env python3
"""Does a number from a book land on the right place on a real knob?

    python3 tools/rack/test_param_units.py

The coefficients below are not invented. Every one was MEASURED off the
installed library by CARTOG at scan 5, and the module it came from is named,
because the thing being tested is whether a conversion is right rather than
whether it is present -- and a fixture written by the same hand as the
converter agrees with it by construction.

Three real filters, three unit systems, one target of 40 Hz:

    Fundamental/VCF      8.56 .. 8000 Hz     exponential   -> 0.229, exact
    ALM018/ALM018        250 .. 12500 Hz     exponential   -> clamped, unreachable
    Ambivalent/Rain      0 .. 1              dimensionless -> refused

The third is the one that matters most. Converting into a knob that has no
unit produces 40, which is outside 0..1, which clamps to 1.0 -- a filter wide
open, in a patch that claims to be following the book. Silence there would be
worse than an error.

A live section at the end re-reads whatever this machine has actually measured
and round-trips every unit-bearing control in it. It skips cleanly where there
is no map, so this is runnable on a machine that has never seen Rack.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import param_units as pu                                    # noqa: E402


def check(ok: bool, label: str, detail: str = "") -> int:
    print(f"  {'ok    ' if ok else 'WRONG '} {label}"
          + (f" — {detail}" if detail and not ok else ""))
    return 0 if ok else 1


def close(a, b, tol=1e-4) -> bool:
    return a is not None and b is not None and abs(a - b) <= tol * max(1.0, abs(b))


# Measured, 2026-08-05, by CARTOG scan 5 on the installed library.
VCF = {"name": "Cutoff frequency", "minValue": 0.006558, "maxValue": 0.993442,
       "defaultValue": 0.5, "unit": " Hz", "displayBase": 1024.0,
       "displayMultiplier": 8.1758, "displayOffset": 0.0}          # Fundamental/VCF
ALM = {"name": "Cutoff Frequency", "minValue": -1.0, "maxValue": 0.0,
       "defaultValue": 0.0, "unit": "Hz", "displayBase": 50.0,
       "displayMultiplier": 12500.0, "displayOffset": 0.0}         # ALM018/ALM018
RAIN = {"name": "Filter Cutoff", "minValue": 0.0, "maxValue": 1.0,
        "defaultValue": 0.5}                                       # Ambivalent/Rain
PCT = {"name": "Level", "minValue": 0.0, "maxValue": 1.0, "unit": "%",
       "displayBase": 0.0, "displayMultiplier": 100.0,
       "displayOffset": 0.0}                                       # Fundamental/VCA-1
MAXIMIZE = {"name": "Maximize", "minValue": 0.0, "maxValue": 1.0, "unit": "%",
            "displayBase": 0.0, "displayMultiplier": 10.0,
            "displayOffset": 50.0}                                 # Battalion, has an offset


def test_a_knob_reads_as_a_physical_value() -> int:
    """Knob position -> what Rack shows next to it."""
    bad = 0
    # Exponential: 1024^v * 8.1758. The ends are the filter's real span.
    bad += check(close(pu.to_display(0.006558, VCF), 8.556, 1e-3),
                 "VCF at its minimum reads ~8.56 Hz",
                 f"got {pu.to_display(0.006558, VCF)}")
    bad += check(close(pu.to_display(0.993442, VCF), 7999.98, 1e-3),
                 "VCF at its maximum reads ~8 kHz",
                 f"got {pu.to_display(0.993442, VCF)}")
    # Linear with a multiplier, and one with an offset too.
    bad += check(close(pu.to_display(0.25, PCT), 25.0), "a 0..1 level reads as 25%",
                 f"got {pu.to_display(0.25, PCT)}")
    bad += check(close(pu.to_display(0.5, MAXIMIZE), 55.0),
                 "an offset control reads through its offset",
                 f"got {pu.to_display(0.5, MAXIMIZE)}")
    # Dimensionless: the identity, so the knob IS the value.
    bad += check(close(pu.to_display(0.42, RAIN), 0.42),
                 "a control with no display shape reads as itself")
    return bad


def test_the_conversion_round_trips() -> int:
    """from_display is the exact inverse of to_display, on real coefficients."""
    bad = 0
    for tag, param, positions in (
            ("VCF", VCF, (0.006558, 0.2, 0.5, 0.993442)),
            ("ALM018", ALM, (-1.0, -0.5, 0.0)),
            ("percent", PCT, (0.0, 0.25, 1.0)),
            ("offset", MAXIMIZE, (0.0, 0.5, 1.0))):
        worst = 0.0
        lost = []
        for v in positions:
            shown = pu.to_display(v, param)
            back = pu.from_display(shown, param) if shown is not None else None
            if back is None:
                # A broken inverse returns nothing rather than a wrong number,
                # so this has to be a named failure and not a crash on None.
                lost.append(v)
                continue
            worst = max(worst, abs(back - v))
        bad += check(not lost and worst < 1e-6,
                     f"{tag}: knob -> physical -> knob returns the knob",
                     f"worst drift {worst:g}" + (f", lost {lost}" if lost else ""))
    return bad


def test_forty_hertz_lands_on_three_different_filters() -> int:
    """The whole point: one number from a book, three unit systems."""
    bad = 0
    got = pu.place(VCF, 40.0, unit="Hz")
    bad += check(close(got.value, 0.22906) and not got.clamped,
                 "Fundamental/VCF can do 40 Hz, at knob 0.229",
                 f"got {got}")
    # And it really is 40 Hz once placed, not merely a number in range.
    bad += check(close(pu.to_display(got.value, VCF), 40.0),
                 "and that knob position reads back as 40 Hz",
                 f"got {pu.to_display(got.value, VCF)}")

    got = pu.place(ALM, 40.0, unit="Hz")
    bad += check(got.clamped and close(got.value, -1.0),
                 "ALM018 cannot reach 40 Hz and says so rather than pretending",
                 f"got {got}")
    bad += check("minimum" in got.reason and "250" in got.reason,
                 "and its refusal names the span it does have", f"got {got.reason!r}")

    got = pu.place(RAIN, 40.0, unit="Hz")
    bad += check(got.value is None and "dimensionless" in got.reason,
                 "a 0..1 knob REFUSES a Hz value instead of clamping wide open",
                 f"got {got}")

    got = pu.place(PCT, 40.0, unit="Hz")
    bad += check(got.value is None and "%" in got.reason,
                 "and a percent control refuses a Hz value too", f"got {got}")
    return bad


def test_the_awkward_shapes_do_not_produce_numbers() -> int:
    """A conversion that cannot be made returns nothing, never a NaN."""
    bad = 0
    log = {"minValue": 0.0, "maxValue": 1.0, "unit": "x",
           "displayBase": -2.0, "displayMultiplier": 1.0, "displayOffset": 0.0}
    bad += check(pu.to_display(0.0, log) is None,
                 "a logarithmic control has no reading at zero")
    bad += check(pu.to_display(-1.0, log) is None,
                 "nor below it")
    dead = {"minValue": 0.0, "maxValue": 1.0, "unit": "x",
            "displayBase": 0.0, "displayMultiplier": 0.0, "displayOffset": 5.0}
    bad += check(pu.from_display(5.0, dead) is None,
                 "a control whose multiplier is zero cannot be placed")
    bad += check(pu.from_display(-3.0, VCF) is None,
                 "an exponential control cannot be placed at a negative Hz")
    # Inverted range: sorted, or every value on it reads as out of range.
    inverted = {"minValue": 0.5, "maxValue": 0.0}     # Befaco/Octaves PWM, measured
    bad += check(pu.bounds(inverted) == (0.0, 0.5),
                 "an inverted range is read low end first",
                 f"got {pu.bounds(inverted)}")
    bad += check(not pu.place(inverted, 0.25).clamped,
                 "so a value inside it is not reported as out of range")
    return bad


def test_absence_means_unknown_below_scan_5() -> int:
    """A missing displayBase is linear only because the scanner looked."""
    bad = 0
    bad += check(pu.knows_units({"scan": 5}), "a scan-5 entry recorded units")
    bad += check(not pu.knows_units({"scan": 4}),
                 "a scan-4 entry did not, so absent means unknown there")
    bad += check(not pu.knows_units({}), "and an entry with no scan version did not")
    return bad


def test_against_whatever_this_machine_measured() -> int:
    """Round-trip every unit-bearing control in the real map, if there is one."""
    path = os.path.expanduser(
        "~/Library/Application Support/Rack2/forge-portmap.json")
    if not os.path.exists(path):
        print("  skip   no port map on this machine — nothing live to check")
        return 0
    with open(path) as f:
        doc = json.load(f)
    checked = worst = 0
    offenders = []
    for entry in (doc.get("modules") or []):
        if not pu.knows_units(entry):
            continue
        for p in (entry.get("params") or []):
            if "minValue" not in p:
                continue
            span = pu.bounds(p)
            lo, hi = span
            for v in (lo, (lo + hi) / 2.0, hi):
                shown = pu.to_display(v, p)
                if shown is None:
                    continue
                back = pu.from_display(shown, p)
                if back is None:
                    offenders.append((entry["model"], p.get("name"), v, shown))
                    continue
                checked += 1
                drift = abs(back - v)
                if drift > worst:
                    worst = drift
                if drift > 1e-3:
                    offenders.append((entry["model"], p.get("name"), v, drift))
    print(f"  ....   round-tripped {checked} live control positions")
    return check(not offenders and worst < 1e-3,
                 "every measured control on this machine round-trips",
                 f"worst drift {worst:g}, offenders {offenders[:4]}")


def main() -> int:
    bad = 0
    for fn in (test_a_knob_reads_as_a_physical_value,
               test_the_conversion_round_trips,
               test_forty_hertz_lands_on_three_different_filters,
               test_the_awkward_shapes_do_not_produce_numbers,
               test_absence_means_unknown_below_scan_5,
               test_against_whatever_this_machine_measured):
        print(f"{fn.__name__}:")
        bad += fn()
    print("\n" + ("all good" if bad == 0 else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
