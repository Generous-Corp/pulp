"""Click detector — falsification in BOTH directions.

The true negatives carry the weight here. A clean square, a clean saw, and a clean
hard-synced oscillator are *full of legitimate discontinuities*; a detector that fires
on them is worse than no detector, because it condemns correct oscillators. So the
clean fixtures are tested first and hardest, and the sensitivity claim is measured by
sweeping defect magnitude until detection fails — never derived.

Every threshold asserted here is pinned to a MEASURED value with margin, so a
regression that quietly degrades the floor fails the suite instead of silently
widening the detector's blind spot.
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab import osc_fixtures as fx
from quality_lab.detectors import click

DUR = 0.3
FIRE_DB = -45.0

# (sample_rate, f0) — non-integer periods throughout except where a test needs an
# integer one. 48000/440 = 109.0909..., 44100/220 = 200.4545..., etc: the fractional
# period is the normal case, not an edge case, so it is what the grid exercises.
GRID = [
    (44100, 55.0), (44100, 220.0), (44100, 1760.0),
    (48000, 110.0), (48000, 440.0), (48000, 880.0),
    (96000, 220.0), (96000, 1760.0),
]

# Pinned from measurement across GRID: the worst clean saw/square reading is -57.9 dB
# and the worst hard-sync reading is -59.0 dB. -52 leaves ~6 dB of margin without
# letting a real regression through.
MEASURED_CLEAN_FLOOR_DB = -52.0


# ── True negatives: waveforms whose whole point is discontinuity ────────────────────

@pytest.mark.parametrize("sr,f0", GRID)
@pytest.mark.parametrize("wave", ["saw", "square"])
def test_clean_bandlimited_oscillator_does_not_fire(sr, f0, wave):
    """The case the naive "big step ⇒ click" rule gets wrong: a square edge and a saw
    wrap are full-scale discontinuities BY DESIGN, twice and once per period."""
    y = getattr(fx, f"bandlimited_{wave}")(sr, f0, DUR)
    r = click.detect(y, sr, f0_hint=f0)
    assert not r.fired, f"clean {wave} at {f0} Hz / {sr} flagged as clicky: {r.notes}"
    assert r.scalar <= MEASURED_CLEAN_FLOOR_DB, f"floor regressed: {r.scalar:.1f} dB"


# Slave:master ratios spanning the easy and the hard case. 661/220 = 3.005 and
# 392/130.81 = 2.997 are NEAR-INTEGER: the reset lands almost where the slave already
# was, so the signal very nearly repeats at the SLAVE period and a period search is
# strongly attracted to it — a period search that lands there reads the leftover sync
# discontinuity as a click, so these ratios belong in the grid, not in a footnote.
SYNC_PAIRS = [(110.0, 277.0), (110.0, 440.0), (220.0, 661.0),
              (146.83, 523.25), (130.81, 392.0), (98.0, 294.5)]


@pytest.mark.parametrize("sr", [44100, 48000])
@pytest.mark.parametrize("f_master,f_slave", SYNC_PAIRS)
def test_clean_hard_sync_does_not_fire(sr, f_master, f_slave):
    """Hard sync resets the slave mid-cycle: a violent, intentional discontinuity at a
    phase unrelated to the slave's own waveform. It stays legitimate because the
    COMPOSITE repeats at the master period — and nothing special-cases sync to know
    that. The period fit finds the master period on its own."""
    y = fx.hard_synced_saw(sr, f_master, f_slave, DUR)
    r = click.detect(y, sr, f0_hint=f_master)
    assert not r.fired, f"clean hard sync {f_master}->{f_slave} flagged: {r.notes}"
    assert r.scalar <= MEASURED_CLEAN_FLOOR_DB, f"floor regressed: {r.scalar:.1f} dB"


@pytest.mark.parametrize("f_master,f_slave", SYNC_PAIRS)
def test_clean_hard_sync_does_not_fire_without_a_hint(f_master, f_slave):
    """The same, unaided. This is where the slave-period trap actually bites: a YIN seed
    dips at the slave period for a near-integer ratio, and fitting there reads the
    leftover sync discontinuity as a -10 dB click at 0.99 confidence. `_resolve_period`
    is what turns that into a correct reading."""
    sr = 48000
    r = click.detect(fx.hard_synced_saw(sr, f_master, f_slave, DUR), sr)
    assert not r.fired, f"unaided hard sync {f_master}->{f_slave} flagged: {r.notes}"


def test_hard_sync_fit_finds_the_master_period_not_the_slave():
    """The mechanism behind the tests above, asserted directly on the worst case: at a
    ratio of 3.005 the signal nearly repeats at the slave's ~73-sample period, and the
    fit must still land on the master's."""
    sr, f_master, f_slave = 48000, 220.0, 661.0
    a = click.analyze(fx.hard_synced_saw(sr, f_master, f_slave, DUR), sr)
    assert a.period_samples == pytest.approx(sr / f_master, abs=0.1)


def test_guard_follows_the_resolved_period_not_the_seed():
    """A guard sized from the SEED rather than from the period actually used leaves
    start-up transient inside the measured interior when the two differ — worth ~8 dB of
    floor on hard sync, which is a false positive's worth. Asserted by requiring the
    unaided reading (seed = slave period) to match the hinted one (seed = master)."""
    sr, f_master, f_slave = 48000, 130.81, 392.0
    y = fx.hard_synced_saw(sr, f_master, f_slave, DUR)
    assert click.analyze(y, sr).click_db == pytest.approx(
        click.analyze(y, sr, f0_hint=f_master).click_db, abs=1.0
    )


# ── True positives: a known defect of a known size at a known time ──────────────────

def _square(sr, f0):
    y = fx.bandlimited_square(sr, f0, DUR)
    return y, float(np.max(np.abs(y)))


@pytest.mark.parametrize("sr,f0", [(44100, 220.0), (48000, 440.0), (48000, 880.0)])
def test_band_switch_seam_fires(sr, f0):
    _, peak = _square(sr, f0)
    y = fx.band_switch_seam(sr, f0, DUR, 0.15, peak * 10 ** (-30 / 20.0))
    assert click.detect(y, sr, f0_hint=f0).fired


@pytest.mark.parametrize("sr,f0", [(44100, 220.0), (48000, 440.0), (48000, 880.0)])
def test_crossfade_seam_fires(sr, f0):
    y, peak = _square(sr, f0)
    seam = fx.inject_step(y, int(0.15 * sr), peak * 10 ** (-30 / 20.0))
    assert click.detect(seam, sr, f0_hint=f0).fired


@pytest.mark.parametrize("sr,f0", [(44100, 220.0), (48000, 440.0), (48000, 880.0)])
def test_voice_steal_pop_fires(sr, f0):
    y, peak = _square(sr, f0)
    pop = fx.inject_pop(y, sr, int(0.15 * sr), peak * 10 ** (-30 / 20.0))
    assert click.detect(pop, sr, f0_hint=f0).fired


@pytest.mark.parametrize("block", [16, 256])
def test_param_zipper_fires(block):
    """A zipper is PERIODIC — at the block rate. It must still fire, which is what
    proves the rule keys on the OSCILLATOR's period rather than on periodicity itself.

    Block 64 is excluded here and covered by the drift-guard trade test instead: at
    48 kHz it lands at 375 Hz, close enough to a 440 Hz fundamental to perturb the
    period fit, so the drift guard refuses it rather than diagnosing it.
    """
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    assert click.detect(fx.zipper(y, peak * 10 ** (-30 / 20.0), block), sr, f0_hint=f0).fired


@pytest.mark.parametrize("block", [16, 64, 256])
@pytest.mark.parametrize("db", [-20, -30])
def test_a_param_zipper_never_reads_clean(block, db):
    """The gate-relevant invariant, weaker than `fired` but true for every zipper: a
    zipper is either diagnosed or refused, never passed. Which one depends on whether
    its block rate disturbs the period fit — a refusal loses the diagnosis but not the
    gate."""
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    r = click.detect(fx.zipper(y, peak * 10 ** (db / 20.0), block), sr, f0_hint=f0)
    assert r.fired or r.low_coverage, f"a {db} dB zipper at block {block} read CLEAN"


def test_defect_is_localized_to_where_it_was_injected():
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    at = 0.15
    r = click.detect(fx.inject_step(y, int(at * sr), peak * 0.01), sr, f0_hint=f0)
    assert r.fired and r.worst_regions
    assert r.worst_regions[0].time_s == pytest.approx(at, abs=0.002)


# ── The negative control: the reading must COLLAPSE when the defect is removed ──────

def test_negative_control_reading_collapses_when_the_defect_is_removed():
    """The only construction that proves the analyzer SEES what it claims: run the
    IDENTICAL measurement with the defect removed and show the reading collapse.
    Asserting a computed floor instead would be the analyzer grading its own homework —
    it would pass just as happily if the measurement were blind."""
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    with_defect = click.analyze(fx.inject_step(y, int(0.15 * sr), peak * 0.01), sr, f0_hint=f0)
    without = click.analyze(y, sr, f0_hint=f0)
    assert with_defect.click_db >= FIRE_DB
    assert without.click_db < MEASURED_CLEAN_FLOOR_DB
    assert with_defect.click_db - without.click_db > 30.0


@pytest.mark.parametrize("dc", [0.0, 0.5, 2.0, -1.0])
def test_a_dc_offset_does_not_move_the_scale(dc):
    """The reading is "the step's height relative to the waveform", so a DC offset must
    not touch it. Scaling by max|y| instead of max|y - mean(y)| let DC quietly shrink
    every reading: at +2.0 DC a -40 dB seam read -50.0 and did NOT fire, because the
    rail counted as signal."""
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    seam = fx.inject_step(y, int(0.15 * sr), peak * 10 ** (-40 / 20.0))
    got = click.analyze(seam + dc, sr, f0_hint=f0).click_db
    assert got == pytest.approx(-40.0, abs=1.5), f"DC {dc:+} moved the reading to {got:.1f} dB"
    assert click.detect(seam + dc, sr, f0_hint=f0).fired


def test_reading_matches_the_injected_step_from_independent_math():
    """Ground truth from construction, not from running the code and pinning its output:
    a step of delta into a signal of peak p must read 20*log10(delta/p)."""
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    for db in (-20.0, -30.0, -40.0):
        delta = peak * 10 ** (db / 20.0)
        got = click.analyze(fx.inject_step(y, int(0.15 * sr), delta), sr, f0_hint=f0).click_db
        assert got == pytest.approx(db, abs=1.5), f"injected {db} dB, read {got:.1f} dB"


# ── Measured sensitivity: sweep until detection fails ───────────────────────────────

@pytest.mark.parametrize("sr,f0", [(44100, 220.0), (48000, 440.0), (48000, 880.0)])
def test_sensitivity_threshold_is_the_fire_threshold(sr, f0):
    """Sweep the defect down until the detector stops firing, and pin where that is.

    Because the reading is calibrated to the step height, the sensitivity IS the fire
    threshold: measured across the grid, a -44 dB step fires and the last firing level
    sits at -44 to -46 dB. Anything meaningfully worse means a real regression.
    """
    y, peak = _square(sr, f0)
    fired_at = [
        db for db in range(-20, -70, -2)
        if click.detect(fx.inject_step(y, int(0.15 * sr), peak * 10 ** (db / 20.0)),
                        sr, f0_hint=f0).fired
    ]
    assert fired_at, "detector fired at no level at all"
    assert min(fired_at) <= -44, f"sensitivity regressed: quietest detected = {min(fired_at)} dB"


# ── Blind spots: measured, named, and pinned so they cannot silently change ─────────

def test_blind_spot_a_period_locked_defect_is_invisible_by_construction():
    """The rule's price, isolated to one variable: the SAME glitch at the SAME level,
    period-locked vs not.

    A defect repeating at exactly the oscillator period is indistinguishable from the
    waveform — it IS a waveform, as far as any period-synchronous rule can tell. A LOUD
    (-20 dB) locked glitch is missed completely; a single-shot one fires. Pinned so the
    honesty claim in the module docstring cannot quietly stop being true.
    """
    sr, f0 = 48000, 500.0  # period = 96 samples exactly, so "every period" is expressible
    period = int(sr / f0)
    y, peak = _square(sr, f0)
    delta = peak * 10 ** (-20 / 20.0)

    locked = y + delta * ((np.arange(len(y)) % period) < period // 2)
    assert click.analyze(locked, sr, f0_hint=f0).click_db < -100.0
    assert not click.detect(locked, sr, f0_hint=f0).fired

    single = np.array(y)
    single[8000 : 8000 + period // 2] += delta
    assert click.detect(single, sr, f0_hint=f0).fired


def test_a_glitch_one_sample_off_period_is_seen():
    """The knife edge: the locked glitch above is invisible, but re-injecting it at a
    period ONE SAMPLE longer makes it read -22 dB. `analyze` is asserted rather than
    `detect` because a 96-vs-97 beat legitimately moves the best-fit period between the
    render's two halves, so `detect` REFUSES it on drift — a different guard, tested
    separately below."""
    sr, f0 = 48000, 500.0
    period = int(sr / f0)
    y, peak = _square(sr, f0)
    delta = peak * 10 ** (-20 / 20.0)
    unlocked = y + delta * ((np.arange(len(y)) % (period + 1)) < period // 2)
    assert click.analyze(unlocked, sr, f0_hint=f0).click_db >= FIRE_DB


def test_blind_spot_b_a_subharmonic_locked_defect_needs_the_hint_to_be_seen():
    """Period doubling: alternate cycles differ — an audible f0/2 subharmonic, and a real
    oscillator defect class.

    Unaided this is not merely hard, it is genuinely ambiguous: "a 440 Hz oscillator
    whose cycles alternate" and "a 220 Hz oscillator" are the same signal, and no rule
    can call one of them broken. So the unaided path reads it clean, and that is the
    honest answer rather than a bug.

    With `f0_hint` the caller has named the commanded period, so the ambiguity is gone
    and the defect must be caught. It was NOT: searching multiples of the hint let the
    resolver adopt 4x the commanded period and null the defect away, reading -236.7 dB —
    a clean PASS on a -20 dB defect, with a correct hint supplied.
    """
    sr, f0 = 48000, 500.0  # period = 96 samples exactly
    period = int(sr / f0)
    y, peak = _square(sr, f0)
    delta = peak * 10 ** (-20 / 20.0)

    for every in (2, 3, 4):
        cycle = np.arange(len(y)) // period
        doubled = y + delta * (((cycle % every) == 0) & ((np.arange(len(y)) % period) < period // 2))
        a = click.analyze(doubled, sr, f0_hint=f0)
        assert a.period_samples == pytest.approx(period, abs=0.5), (
            f"every-{every} defect: fit ran away to {a.period_samples:.1f} instead of the "
            f"commanded {period} — the defect gets nulled away"
        )
        assert click.detect(doubled, sr, f0_hint=f0).fired, f"missed a -20 dB every-{every} defect"


def test_a_hint_is_fitted_directly_and_never_searched_for_multiples():
    """The mechanism behind the test above. The multiple search exists only to escape the
    unaided slave-period trap; applied to a hint it is actively harmful, because a
    periodic defect makes the commanded period score badly and the search then prefers a
    multiple that explains the defect away."""
    sr, f0 = 48000, 500.0
    y, _ = _square(sr, f0)
    assert click.analyze(y, sr, f0_hint=f0).period_samples == pytest.approx(sr / f0, abs=0.01)
    assert click.analyze(y, sr, f0_hint=f0 / 2).period_samples == pytest.approx(2 * sr / f0, abs=0.01), (
        "a hint must be taken at face value, not silently re-derived"
    )


def _modulated(sr, cents, rate=None, f0=440.0, dur=DUR):
    """A CLEAN bandlimited square whose pitch moves: a linear glide over `cents` when
    `rate` is None, otherwise vibrato of +/-`cents` at `rate` Hz. Nothing is injected —
    every discontinuity in here is a legitimate square edge."""
    n = int(dur * sr)
    t = np.arange(n) / sr
    depth = cents * (t / t[-1] if rate is None else np.sin(2 * np.pi * rate * t))
    phase = 2 * np.pi * np.cumsum(f0 * 2 ** (depth / 1200.0)) / sr
    y = np.zeros(n)
    for k in range(1, int((sr / 2) / f0), 2):
        y += np.sin(k * phase) / k
    return y * 0.7 * 4 / np.pi


def test_slow_vibrato_is_refused_not_reported():
    """Symmetric modulation is the case a two-point drift check cannot see.

    Fitting the period on the first and second half measures NET drift, and vibrato has
    almost none — it returns where it started. Measured that way, +/-0.5 cents at
    6.67 Hz scores 2.8e-09 (i.e. rock steady) while reading -27.4 dB and firing: a
    confident false positive on a CLEAN oscillator, which is the worst failure this
    detector can have. Only a guard that samples the period at several points sees it.
    """
    sr = 48000
    y = _modulated(sr, 0.5, rate=6.6667)
    a = click.analyze(y, sr, f0_hint=440.0)
    assert a.period_confidence > 0.9, "premise: confidence does NOT see the vibrato"
    assert a.click_db >= FIRE_DB, "premise: the raw reading IS a false positive"

    r = click.detect(y, sr, f0_hint=440.0)
    assert not r.fired, "vibrato reported as a click on a clean oscillator"
    assert r.low_coverage, "a refused reading must surface as low_coverage, never as clean"


@pytest.mark.parametrize("cents,rate", [(1.0, 5.0), (2.0, 5.0), (3.0, 4.0), (0.5, 13.333), (1.0, 3.0)])
def test_vibrato_across_rates_and_depths_is_refused(cents, rate):
    """Swept rather than spot-checked, because the two-half guard's blind spot depended
    on the modulation's cycle count landing a particular way across the render — a hole
    a single fixture would have walked straight past."""
    r = click.detect(_modulated(48000, cents, rate=rate), 48000, f0_hint=440.0)
    assert not r.fired and r.low_coverage


def test_a_segment_count_alone_would_be_blind_to_its_own_multiple():
    """Why `_DRIFT_SEGMENT_COUNTS` has more than one entry, asserted at the mechanism.

    A segmentation cannot see a modulation whose cycle count across the render is an
    exact multiple of the segment count: every segment then averages the same value.
    Four segments over exactly 4 vibrato cycles is that case — and alone it collapses to
    near-steady. The 7-segment view of the same signal must not.
    """
    sr = 48000
    cycles_matching_four_segments = 4 / DUR  # 13.33 Hz: exactly 4 cycles across the render
    y = _modulated(sr, 0.5, rate=cycles_matching_four_segments)
    seed = sr / 440.0
    blind = click._segment_spread(y, seed, 4)
    seeing = click._segment_spread(y, seed, 7)
    assert blind is not None and seeing is not None
    assert seeing > blind, "the second segment count must see what the first is blind to"
    assert click.measure_period_drift(y, seed, 0) >= seeing


def test_glide_is_refused_not_reported():
    sr = 48000
    y = _modulated(sr, 5.0)
    a = click.analyze(y, sr, f0_hint=440.0)
    assert a.click_db >= FIRE_DB, "premise: the raw reading IS a false positive"
    r = click.detect(y, sr, f0_hint=440.0)
    assert not r.fired and r.low_coverage and "drift" in r.notes


def test_fast_modulation_is_an_admitted_hole_not_a_closed_one():
    """The blind spot the module docstring claims, pinned so the claim stays true.

    Past roughly one modulation cycle per segment, each segment averages over the whole
    cycle and the drift score COLLAPSES toward clean while the reading stays a false
    positive. This asserts the hole is exactly where the docs say it is: a fast, shallow
    vibrato reads above the fire threshold AND scores a drift no guard could act on,
    because it is below what the clean fixtures themselves measure.

    If a future change closes this, this test fails — and the docstring's blind-spot
    section must then be corrected in the honest direction.
    """
    sr = 48000
    fast = _modulated(sr, 0.08, rate=40.0)
    a = click.analyze(fast, sr, f0_hint=440.0)
    assert a.click_db >= FIRE_DB, "premise: a clean fast vibrato still reads as a click"
    clean_worst = max(
        click.analyze(fx.bandlimited_square(s, f, DUR), s, f0_hint=f).period_drift
        for s, f in ((44100, 220.0), (48000, 440.0), (48000, 880.0))
    )
    assert a.period_drift < clean_worst * 2, (
        "fast vibrato now scores clear of the clean fixtures — the hole may be closable; "
        "re-derive the guard window and update the blind-spot docs"
    )


def test_the_drift_guard_prices_no_false_positives_over_diagnosing_a_loud_zipper():
    """The trade the guard makes, pinned in both directions.

    There is no threshold that both refuses false-firing modulation and reports a loud
    block-rate zipper: the guard cannot tell a moving pitch from an interfering
    component, since both make a segment fit a different period. Measured, the mildest
    false-firing modulation always scores BELOW the loudest zipper, at every segment
    count tried — the window is empty.

    So the guard is placed to avoid false positives, and a loud zipper is REFUSED rather
    than diagnosed. That is not a silent pass: the gate still does not pass it. This
    asserts both halves so the trade cannot be quietly reversed.
    """
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)

    loud = fx.zipper(y, peak * 10 ** (-20 / 20.0), 64)
    r_loud = click.detect(loud, sr, f0_hint=f0)
    assert not r_loud.fired, "if this now fires, the window opened — re-derive the guard"
    assert r_loud.low_coverage, "a refused loud zipper must NOT read as clean"
    assert click.analyze(loud, sr, f0_hint=f0).click_db >= FIRE_DB, (
        "the measurement still SEES it; only the refusal withholds the diagnosis"
    )

    quiet = fx.zipper(y, peak * 10 ** (-40 / 20.0), 64)
    assert click.detect(quiet, sr, f0_hint=f0).fired, "a quiet zipper must still be diagnosed"


def test_a_glide_too_small_to_false_fire_is_not_refused():
    """The guard must not refuse everything that moves at all — a glide below the
    false-positive boundary reads clean and should be reported as such."""
    r = click.detect(_modulated(48000, 0.02), 48000, f0_hint=440.0)
    assert not r.fired and not r.low_coverage


def test_a_steady_clean_oscillator_is_not_refused():
    """The other side of the drift guard: it must not refuse the normal case, or every
    reading becomes a shrug."""
    r = click.detect(fx.bandlimited_square(48000, 440.0, DUR), 48000, f0_hint=440.0)
    assert not r.fired and not r.low_coverage
    assert click.analyze(fx.bandlimited_square(48000, 440.0, DUR), 48000, f0_hint=440.0).period_drift == 0.0


def test_blind_spot_c_a_defect_on_an_edge_is_NOT_masked():
    """Worth pinning because the opposite is the intuitive guess: since the comb
    SUBTRACTS the neighboring period rather than thresholding around it, an edge
    cancels and cannot hide anything sitting on top of it."""
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    edge = int(np.argmax(np.abs(np.diff(y[4000:4400])))) + 4000
    mid = edge + int(sr / f0 / 4)
    delta = peak * 10 ** (-40 / 20.0)
    on = click.analyze(fx.inject_step(y, edge, delta), sr, f0_hint=f0).click_db
    off = click.analyze(fx.inject_step(y, mid, delta), sr, f0_hint=f0).click_db
    assert on == pytest.approx(off, abs=2.0)


# ── Preconditions and machinery ─────────────────────────────────────────────────────

def test_noise_has_no_period_and_is_refused_not_called_clicky():
    r = click.detect(np.random.default_rng(0).standard_normal(48000) * 0.3, 48000)
    assert not r.fired and r.low_coverage


def test_silence_is_refused():
    r = click.detect(np.zeros(48000), 48000, f0_hint=440.0)
    assert not r.fired and r.low_coverage


@pytest.mark.parametrize("sr,f0", GRID)
def test_period_is_found_without_an_f0_hint(sr, f0):
    """The seed must work unaided. An octave error UP is harmless (a signal periodic at
    P is periodic at 2P); an error DOWN nulls nothing and reads as a click, so the seed
    is required to land on a true period, not merely near one."""
    a = click.analyze(fx.bandlimited_square(sr, f0, DUR), sr)
    ratio = a.period_samples / (sr / f0)
    assert ratio == pytest.approx(round(ratio), abs=0.01) and ratio >= 0.99
    assert a.click_db <= MEASURED_CLEAN_FLOOR_DB


@pytest.mark.parametrize("delay", [5.0, 5.5, 109.0909, 200.25])
def test_sinc_delay_is_exact_for_a_bandlimited_signal(delay):
    """The premise the whole rule rests on: a fractional delay of a BANDLIMITED sequence
    is exact, so 'you can't interpolate a discontinuity' does not apply — the jump is in
    the underlying waveform, not in the data."""
    sr = 48000
    t = np.arange(4096) / sr
    got = click.sinc_delay(np.sin(2 * np.pi * 1000 * t), delay)
    want = np.sin(2 * np.pi * 1000 * (t - delay / sr))
    assert np.max(np.abs(got[400:-400] - want[400:-400])) < 1e-3


def test_result_is_advisory_until_validated():
    r = click.detect(fx.bandlimited_square(48000, 440.0, DUR), 48000, f0_hint=440.0)
    assert r.maturity == "experimental"
    assert r.unit == "unexpected_step_db" and r.tolerance_class == "click.v1"


def _flux_outlier_margin_db(y, n_fft=1024, hop=48):
    """The spectral-flux alternative: per-hop (1 ms) flux, flag hops exceeding median +
    12 dB. Returns the worst hop's margin over the median."""
    win = np.hanning(n_fft)
    frames = np.lib.stride_tricks.sliding_window_view(y, n_fft)[::hop] * win
    flux = np.sum(np.abs(np.diff(np.abs(np.fft.rfft(frames, axis=-1)), axis=0)), axis=-1)
    med = float(np.median(flux))
    return 20 * np.log10(float(np.max(flux)) / med) if med > 1e-20 else 999.0


def test_the_flux_outlier_alternative_misses_a_zipper_and_the_residual_rule_does_not():
    """Why this detector is not built on spectral flux, measured rather than asserted.

    A spectral-flux outlier rule is a real contender, not a straw man: a steady
    oscillator's magnitude spectrum does not change at its own edges, so flux does NOT
    false-fire on a square (measured +2.9 dB against a +12 dB rule), and it catches a
    one-shot seam down to about -40 dB.

    But it has a structural hole exactly where an oscillator gate needs coverage: a
    block-rate parameter zipper is STATIONARY churn, so it lifts the median along with
    every hop and produces no outlier at any magnitude. A LOUD -20 dB zipper reads
    +2.6 dB — indistinguishable from a clean square. The two routes are complementary,
    not interchangeable.
    """
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    loud = peak * 10 ** (-20 / 20.0)

    assert _flux_outlier_margin_db(y) < 12.0, "premise: flux is quiet on a clean square"

    seam = fx.inject_step(y, int(0.15 * sr), loud)
    assert _flux_outlier_margin_db(seam) > 12.0, "premise: flux does catch a one-shot seam"

    # Block 256, not 64: at 48 kHz a 64-sample block lands at 375 Hz, close enough to a
    # 440 Hz fundamental that the residual route's own drift guard refuses it. Block 256
    # is 94 Hz, far from the fundamental, so the two routes are compared on a signal the
    # residual route genuinely diagnoses.
    zipped = fx.zipper(y, loud, 256)
    assert _flux_outlier_margin_db(zipped) < 12.0, "the flux route's hole: a zipper is invisible to it"
    assert click.detect(zipped, sr, f0_hint=f0).fired, "the residual route must cover that hole"


def test_click_is_standalone_not_in_the_mono_pipeline_registry():
    """Like `stereo_width`: this detector is reference-free and takes (signal, sr), not
    (reference, candidate, sr), so it must not be wired into the pair-wise registry."""
    from quality_lab import pipeline
    assert "click" not in pipeline._DETECTORS
