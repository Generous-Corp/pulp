#!/usr/bin/env python3
"""Turn a physical value into a knob position, and back.

A book says "low-pass cutoff 40 Hz" and means one thing. The installed library
expresses that one thing four ways -- measured across its filters:

    ALM018/ALM018            Cutoff Frequency   -1.000 ..     0.000
    Ambivalent/XFMN01        Filter cutoff      20.000 .. 12000.000   Hz
    Ambivalent/Rain          Filter Cutoff       0.000 ..     1.000   normalised
    Battalion/BattalionTone  Cutoff             -5.000 ..     5.000   volts

Against those bounds alone, 40 is past one module's maximum, near another's
floor, and meaningless on a third. What reconciles them is what Rack keeps
beside the bounds and CARTOG records from scan 5: the unit, and the three
numbers that map a knob position to a physical value.

    displayValue = f(value) * displayMultiplier + displayOffset
        f(value) = value                      for displayBase == 0   linear
        f(value) = log_{-displayBase}(value)  for displayBase < 0    logarithmic
        f(value) = displayBase ** value       for displayBase > 0    exponential

So `to_display` reads a knob, `from_display` places one, and `place` is the
one to reach for: it converts, then clamps into the control's own range and
says whether it had to.
"""

from __future__ import annotations

import math
from typing import NamedTuple

# Below this a scan did not look for units at all, so a param with no
# `displayBase` is UNKNOWN rather than linear. At or above it, absent means
# the identity -- the scanner looked and there was nothing to record.
SCAN_WITH_UNITS = 5


def _shape(param: dict) -> tuple[float, float, float]:
    return (float(param.get("displayBase", 0.0)),
            float(param.get("displayMultiplier", 1.0)),
            float(param.get("displayOffset", 0.0)))


def to_display(value: float, param: dict) -> float | None:
    """Knob position -> physical value. None where it cannot be expressed.

    A logarithmic control has no display value at or below zero, and an
    exponential one can overflow; both return None rather than a NaN that
    would travel quietly into a patch.
    """
    base, mult, offset = _shape(param)
    try:
        if base == 0.0:
            f = value
        elif base < 0.0:
            if value <= 0.0:
                return None
            f = math.log(value) / math.log(-base)
        else:
            f = math.pow(base, value)
    except (ValueError, OverflowError):
        return None
    out = f * mult + offset
    return out if math.isfinite(out) else None


def from_display(display: float, param: dict) -> float | None:
    """Physical value -> knob position. The exact inverse of `to_display`."""
    base, mult, offset = _shape(param)
    if mult == 0.0:
        return None                       # not invertible; every knob is offset
    try:
        f = (display - offset) / mult
        if base == 0.0:
            out = f
        elif base < 0.0:
            out = math.pow(-base, f)
        else:
            if f <= 0.0:
                return None
            out = math.log(f) / math.log(base)
    except (ValueError, OverflowError, ZeroDivisionError):
        return None
    return out if math.isfinite(out) else None


def bounds(param: dict) -> tuple[float, float] | None:
    """The control's range, low end first.

    Sorted, because `configParam` does not require min < max: a reversed knob
    is a legal configuration and this library contains ten of them. Comparing
    against an unsorted pair silently rejects every value on such a control.
    """
    lo, hi = param.get("minValue"), param.get("maxValue")
    if not isinstance(lo, (int, float)) or not isinstance(hi, (int, float)):
        return None
    return (float(lo), float(hi)) if lo <= hi else (float(hi), float(lo))


def knows_units(entry: dict) -> bool:
    """Whether this module's entry came from a scanner that recorded units."""
    return isinstance(entry.get("scan"), int) and entry["scan"] >= SCAN_WITH_UNITS


def unit_of(param: dict) -> str:
    """The control's unit, normalised. Empty means dimensionless.

    Rack's convention puts a space before a word unit (" Hz", " ms") and none
    before a symbol ("%"), so the raw strings do not compare equal even when
    they mean the same thing.
    """
    return str(param.get("unit", "")).strip().casefold()


class Placement(NamedTuple):
    #: knob position, or None where the value cannot be placed at all
    value: float | None
    #: pinned to the control's range rather than placed exactly
    clamped: bool
    #: why it is not exact; empty when it is
    reason: str


def place(param: dict, physical: float, unit: str | None = None) -> Placement:
    """Put a physical value on a control, or refuse and say why.

    Pass the unit the number came WITH. A dimensionless 0..1 knob has no
    opinion about Hz, and converting anyway is how "cutoff 40 Hz" becomes a
    filter wide open: the number is out of the knob's 0..1 range, so it clamps
    to 1.0 and the patch plays something the book did not describe. A caller
    that ignores a `clamped` flag gets that silently, so a unit it cannot read
    is a refusal rather than a flag.

    Clamping stays a flag, because it is a different situation: a 250..12500 Hz
    filter asked for 40 Hz genuinely understood the request and cannot reach
    it. That is worth reporting and sometimes worth accepting.
    """
    if unit is not None:
        want, have = unit.strip().casefold(), unit_of(param)
        if not have:
            return Placement(None, False,
                             f"control is dimensionless; cannot place {unit}")
        if want != have:
            return Placement(None, False,
                             f"control reads in {param.get('unit','').strip()}"
                             f", not {unit}")
    value = from_display(physical, param)
    if value is None:
        return Placement(None, False, "value is not representable on this control")
    # Prove the inverse produced the target we were asked for before the raw
    # value is allowed into a patch. This is deliberately checked through the
    # forward transform rather than trusted from shared arithmetic: the
    # contract is physical target -> device-domain value -> physical target.
    shown = to_display(value, param)
    if shown is None or not math.isclose(shown, physical,
                                         rel_tol=1e-6, abs_tol=1e-6):
        return Placement(None, False,
                         "converted knob value does not round-trip to the target")
    span = bounds(param)
    if span is None:
        return Placement(value, False, "")
    lo, hi = span
    if value < lo:
        return Placement(lo, True, f"below the control's minimum "
                                   f"({describe(param)})")
    if value > hi:
        return Placement(hi, True, f"above the control's maximum "
                                   f"({describe(param)})")
    return Placement(value, False, "")


def describe(param: dict) -> str:
    """The control's physical span, for a human or a prompt."""
    span = bounds(param)
    if span is None:
        return "unbounded"
    lo, hi = span
    dlo, dhi = to_display(lo, param), to_display(hi, param)
    unit = param.get("unit", "")
    if dlo is None or dhi is None:
        return f"{lo:g}..{hi:g} (raw)"
    return f"{dlo:g}..{dhi:g}{unit}"
