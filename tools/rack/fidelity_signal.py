#!/usr/bin/env python3
"""What a recording of a patch actually contains: pitch, level, held values.

Split from the harness that produces the recording because these are two
different jobs with two different failure modes. Everything here is pure
arithmetic over a list of samples -- no Rack, no subprocess, no files -- which
is what lets the awkward cases be tested directly: silence, noise, a drone, a
pluck too short to hold, a sawtooth at the top of the range.

That matters more here than anywhere else in the harness, because a pitch
reader that always answers is worse than no pitch reader at all. It turns
silence into a note and makes a broken patch look like a playing one. Every
function below can say "I could not read this", and the harness reports that
as an unproven claim rather than a pass.

Rack's convention throughout: a volt is an octave and zero volts is middle C.

WHAT THIS CANNOT READ, so that a limit is never reported as a finding: a note
has to hold a couple of cycles inside one window. A bass line two octaves down
that a short envelope cuts to thirty milliseconds is under two cycles of its
own pitch, and no window settles -- measured on a real generated patch, whose
notes were 30 ms bursts of a 58 Hz tone. `heard_notes` reports nothing for it,
and the harness calls the audible claim UNTESTED rather than failed. Reading a
pitch out of that needs a different method (counting crossings over a whole
burst), not a wider tolerance on this one.
"""

from __future__ import annotations

import math

# How well a window has to correlate with itself a period later before the
# reading is called a pitch. Measured on real oscillator output, a held note
# reaches 0.95 and noise stays under 0.5; two thirds sits clear of both.
CONFIDENCE = 0.66

# Rack's pitch convention: 0 V is middle C, and a volt is an octave.
C4_HZ = 261.6255653

# Below this, in volts RMS, a recording is silence rather than a quiet signal.
# Rack runs at +-5 V, so this is some eighty decibels down.
SILENCE = 5e-4

# How far two voltage readings may sit apart and still be the same one. A
# hundredth of a volt is an eighth of a semitone -- wide enough for a DC
# reading averaged over a window, and nowhere near wide enough to let a knob
# sitting at its default pass for one the patch set.
STEP_TOLERANCE = 0.01


def volts_to_semitones(v: float) -> float:
    return v * 12.0


def hz_to_volts(hz: float) -> float:
    return math.log2(hz / C4_HZ)


def estimate_f0(samples: list[float], rate: float,
                lo_hz: float = 40.0, hi_hz: float = 3000.0) -> float | None:
    """The fundamental of one window, by autocorrelation. None when unpitched.

    Decimated first, because the lag search is the expensive part and pitch
    does not need audio bandwidth: at 8 kHz the whole musical range fits in
    lags a plain loop can walk.

    The search starts AFTER the correlation first goes negative, which is the
    part that has to be right. Autocorrelation is near 1 at a lag of nothing
    and falls away from there, so the largest value in the whole curve is
    usually the smallest lag rather than the period -- and the smallest lag
    the search can see is the top of its frequency range. That produced a
    confident 8.9 kHz on two unrelated patches, identical to four decimals,
    which is the shape of a number that came from the instrument. Skipping the
    initial descent leaves only real peaks to choose between.

    Among those, the EARLIEST peak within a hair of the best one wins, not the
    tallest. A waveform correlates with itself at two periods as well as at
    one, and picking the taller of the two reports an octave down.

    Returns None rather than a number when the window is silent or no peak is
    convincing. A pitch detector that always answers turns silence into a note
    and makes a broken patch look like a playing one -- which is exactly the
    failure this whole exercise exists to catch.
    """
    if not samples or rate <= 0:
        return None
    # Averaged down, not sampled down. Taking every fifth sample of a
    # sawtooth folds everything above the new Nyquist back into the range the
    # search covers, and the folded copy of a 1 kHz saw correlates better an
    # octave below it than at its own period -- a plain decimation reported
    # 1046 Hz as 521. A box average is the cheapest filter that stops it.
    step = max(1, int(rate / 8000.0))
    x = ([sum(samples[i:i + step]) / step
          for i in range(0, len(samples) - step + 1, step)]
         if step > 1 else list(samples))
    r = rate / step
    n = len(x)
    if n < 64:
        return None
    mean = sum(x) / n
    x = [v - mean for v in x]
    energy = sum(v * v for v in x)
    # Rack runs at +-5 V; a window whose RMS is under a millivolt is silence,
    # not a quiet note.
    if math.sqrt(energy / n) < 1e-3:
        return None

    min_lag = max(2, int(r / min(hi_hz, r / 2.0)))
    max_lag = min(n // 2, int(r / lo_hz))
    if max_lag <= min_lag + 2:
        return None
    corr = [sum(x[i] * x[i + lag] for i in range(n - lag)) / energy
            for lag in range(0, max_lag + 1)]

    first = next((lag for lag in range(min_lag, max_lag + 1)
                  if corr[lag] < 0.0), None)
    if first is None:
        return None                      # never turns over: no period in reach
    peak = max(range(first, max_lag + 1), key=lambda lag: corr[lag])
    if corr[peak] < CONFIDENCE:
        return None
    best = next(lag for lag in range(first, peak + 1)
                if corr[lag] >= corr[peak] * 0.9
                and corr[lag] >= corr[lag - 1]
                and (lag == max_lag or corr[lag] >= corr[lag + 1]))

    # Parabolic interpolation around the peak, so a pitch between two integer
    # lags is not reported a quarter-tone out.
    y0, y1, y2 = corr[best - 1], corr[best], corr[min(best + 1, max_lag)]
    denom = y0 - 2 * y1 + y2
    shift = 0.5 * (y0 - y2) / denom if abs(denom) > 1e-12 else 0.0
    return r / (best + max(-1.0, min(1.0, shift)))


# How much signal one reading covers. Small enough that a note half a second
# long is read many times over, large enough to hold two cycles of anything
# down to a low bass note.
WINDOW = 0.06


def segment_pitches(samples: list[float], rate: float,
                    window: float = WINDOW) -> list[float | None]:
    """The fundamental of each fixed window, in order. None where unpitched."""
    n = max(1, int(window * rate))
    return [estimate_f0(samples[i:i + n], rate)
            for i in range(0, max(0, len(samples) - n + 1), n)]


def runs(values: list[float | None], tol: float = 0.4
         ) -> list[tuple[float | None, int]]:
    """(value, how many windows in a row read it), in order.

    `tol` is in semitones. Grouping rather than collapsing keeps the LENGTH of
    each reading, which is what separates a note from an artefact: a window
    straddling a step boundary reads somewhere between the two notes and does
    so exactly once, where a note held for half a second reads the same thing
    eight windows running.

    A run's value is the MEDIAN of its windows, not its first. The first window
    of a note is the one that contains the change, so reporting it puts every
    note a fraction of a semitone out -- which reads as a pitch the patch does
    not play, and would be blamed on the patch.
    """
    grouped: list[list[float | None]] = []
    for v in values:
        if grouped and _same(grouped[-1][-1], v, tol):
            grouped[-1].append(v)
        else:
            grouped.append([v])
    out: list[tuple[float | None, int]] = []
    for g in grouped:
        ok = sorted(v for v in g if v is not None)
        out.append((ok[len(ok) // 2] if ok else None, len(g)))
    return out


def _same(a: float | None, b: float | None, tol: float) -> bool:
    if a is None or b is None:
        return a is None and b is None
    return abs(a - b) <= tol


# A reading BETWEEN two other readings has to hold this many windows to be a
# note rather than the boundary between them. A reading with silence on either
# side is kept however brief it is: that is a plucked note, and requiring it to
# hold made an envelope-gated patch -- one that plainly sounds -- read as
# playing nothing at all, which is the worst answer this harness can give.
MIN_NOTE_WINDOWS = 2


def heard_notes(samples: list[float], rate: float, window: float = WINDOW
                ) -> tuple[list[float], int]:
    """(semitones from middle C, one per note heard; windows discarded).

    The second number is the honest part. Every window that did not settle --
    silence, a straddled boundary, a mistracked octave -- is counted rather
    than quietly folded into a neighbour, so a reading that is mostly noise
    cannot present itself as a short tidy melody.
    """
    pitched = segment_pitches(samples, rate, window)
    semis = [volts_to_semitones(hz_to_volts(f)) if f else None
             for f in pitched]
    grouped = runs(semis)
    notes, dropped = [], 0
    for i, (value, n) in enumerate(grouped):
        before = grouped[i - 1][0] if i else None
        after = grouped[i + 1][0] if i + 1 < len(grouped) else None
        straddle = before is not None and after is not None
        if value is not None and (n >= MIN_NOTE_WINDOWS or not straddle):
            notes.append(round(value, 2))
        else:
            dropped += n
    return notes, dropped


def held_voltages(samples: list[float], rate: float, window: float = WINDOW,
                  tol: float = 0.02) -> list[float]:
    """The voltages a control signal settled on, one per hold, in order.

    The same grouping the pitch reader uses, on the voltage itself. This is
    what the patch's pitch source actually emitted, which is the middle term
    between the numbers in the file and the notes in the air.
    """
    n = max(1, int(window * rate))
    per = [sum(samples[i:i + n]) / n
           for i in range(0, max(0, len(samples) - n + 1), n)]
    return [round(v, 4) for v, count in runs(per, tol)
            if v is not None and count >= MIN_NOTE_WINDOWS]


def rms(samples: list[float]) -> float:
    if not samples:
        return 0.0
    return math.sqrt(sum(v * v for v in samples) / len(samples))


def rms_label(level: float) -> str:
    return "silent" if level < SILENCE else f"{level:.3f} V RMS"


def distinct(values: list[float], tol: float = 0.4) -> list[float]:
    """The set of values, to within `tol`, in first-seen order."""
    out: list[float] = []
    for v in values:
        if all(abs(v - u) > tol for u in out):
            out.append(v)
    return out


def tracking(notes: list[float], volts: list[float]) -> tuple[float | None, str]:
    """How far the pitches heard drift from the pitches the CV asked for.

    Compared as INTERVALS from the first note, so this needs to know nothing
    about the oscillator's tuning, its coarse knob, or where zero volts sits.
    That is what makes it a general check rather than one written for a
    particular pair of modules: whatever the patch is, a volt has to move the
    pitch an octave.

    Returns (worst error in semitones, why it could not be compared).
    """
    if not notes or not volts:
        return None, ("one of the two taps recorded nothing, so heard pitch "
                      "and asked-for pitch cannot be compared")
    if len(distinct(volts, STEP_TOLERANCE)) < 2:
        # A held note tracks any tuning perfectly, because there is no
        # interval to get wrong. Reporting zero error for it would let a
        # drone -- the commonest way a melodic patch fails -- pass the one
        # check that was supposed to catch it.
        return None, ("the pitch CV never changed, so the oscillator was "
                      "never asked to move and nothing about its tracking "
                      "was tested")
    if len(notes) != len(volts):
        return None, (f"the audio tap heard {len(notes)} notes and the CV tap "
                      f"held {len(volts)} values; they do not line up")
    worst = 0.0
    for note, volt in zip(notes, volts):
        want = volts_to_semitones(volt - volts[0])
        worst = max(worst, abs((note - notes[0]) - want))
    return worst, ""


