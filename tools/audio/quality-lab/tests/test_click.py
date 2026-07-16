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
# strongly attracted to it. Those are the configurations that broke earlier drafts, so
# they belong in the grid rather than in a footnote.
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


@pytest.mark.parametrize("block", [16, 64, 256])
def test_param_zipper_fires(block):
    """A zipper is PERIODIC — at the block rate. It must still fire, which is what
    proves the rule keys on the OSCILLATOR's period rather than on periodicity itself."""
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    assert click.detect(fx.zipper(y, peak * 10 ** (-30 / 20.0), block), sr, f0_hint=f0).fired


def test_defect_is_localized_to_where_it_was_injected():
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    at = 0.15
    r = click.detect(fx.inject_step(y, int(at * sr), peak * 0.01), sr, f0_hint=f0)
    assert r.fired and r.worst_regions
    assert r.worst_regions[0].time_s == pytest.approx(at, abs=0.002)


# ── The negative control: the reading must COLLAPSE when the defect is removed ──────

def test_negative_control_reading_collapses_when_the_defect_is_removed():
    """Per the plan's §2.8: the only construction that proves the analyzer SEES what it
    claims is running the IDENTICAL measurement with the defect removed and showing the
    reading collapse. Asserting a computed floor would be the analyzer grading its own
    homework."""
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    with_defect = click.analyze(fx.inject_step(y, int(0.15 * sr), peak * 0.01), sr, f0_hint=f0)
    without = click.analyze(y, sr, f0_hint=f0)
    assert with_defect.click_db >= FIRE_DB
    assert without.click_db < MEASURED_CLEAN_FLOOR_DB
    assert with_defect.click_db - without.click_db > 30.0


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


def test_blind_spot_b_a_drifting_pitch_is_refused_not_reported():
    """A clean oscillator gliding by 1 cent reads -28 dB — a confident false positive —
    and `period_confidence` stays ~1.000, so confidence alone does NOT protect against
    it. The drift guard must turn that into a refusal: low_coverage, not fired."""
    sr = 48000
    y = _glide(sr, 5.0)
    a = click.analyze(y, sr, f0_hint=440.0)
    assert a.period_confidence > 0.9, "premise of this test: confidence does NOT see the glide"
    assert a.click_db > FIRE_DB, "premise of this test: the raw reading IS a false positive"

    r = click.detect(y, sr, f0_hint=440.0)
    assert not r.fired, "a glide must not be reported as a click"
    assert r.low_coverage, "a refused reading must surface as low_coverage, never as clean"
    assert "drift" in r.notes


def _glide(sr, cents, f0=440.0, dur=DUR):
    n = int(dur * sr)
    t = np.arange(n) / sr
    phase = 2 * np.pi * np.cumsum(f0 * 2 ** ((cents * t / t[-1]) / 1200.0)) / sr
    y = np.zeros(n)
    for k in range(1, int((sr / 2) / f0), 2):
        y += np.sin(k * phase) / k
    return y * 0.7 * 4 / np.pi


def test_the_drift_guard_window_is_narrow_and_both_of_its_edges_are_pinned():
    """The most fragile number in the detector, so both walls are asserted.

    The drift guard cannot tell a drifting pitch from a strong aperiodic component —
    both make the halves fit different periods — so its threshold is squeezed between
    two measured facts:

      * ceiling: a 0.15-cent glide (drift 4.3e-5) reads -44 dB and WOULD false-fire, so
        the guard must refuse at or below that drift;
      * floor: a -20 dB block-rate zipper (drift 1.9e-5) is a real defect that must
        still be reported, so the guard must NOT refuse at that drift.

    The default 3e-5 sits between with ~1.5x either side. If a future change moves
    either wall, this fails rather than silently trading a false positive for a false
    negative.
    """
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)

    loud_zipper = fx.zipper(y, peak * 10 ** (-20 / 20.0), 64)
    assert click.analyze(loud_zipper, sr, f0_hint=f0).period_drift < 3e-5
    assert click.detect(loud_zipper, sr, f0_hint=f0).fired, "guard too tight: a loud zipper became a refusal"

    mild_glide = _glide(sr, 0.15)
    a = click.analyze(mild_glide, sr, f0_hint=f0)
    assert a.click_db >= FIRE_DB, "premise: this glide WOULD false-fire unguarded"
    assert a.period_drift > 3e-5
    assert not click.detect(mild_glide, sr, f0_hint=f0).fired, "guard too loose: a glide false-fired"


def test_a_glide_too_small_to_false_fire_is_not_refused():
    """The guard must not refuse everything that moves at all — a glide below the
    false-positive boundary reads clean and should be reported as such."""
    r = click.detect(_glide(48000, 0.05), 48000, f0_hint=440.0)
    assert not r.fired and not r.low_coverage


def test_a_steady_clean_oscillator_is_not_refused():
    """The other side of the drift guard: it must not refuse the normal case, or every
    reading becomes a shrug."""
    r = click.detect(fx.bandlimited_square(48000, 440.0, DUR), 48000, f0_hint=440.0)
    assert not r.fired and not r.low_coverage
    assert click.analyze(fx.bandlimited_square(48000, 440.0, DUR), 48000, f0_hint=440.0).period_drift == 0.0


def test_blind_spot_c_a_defect_on_an_edge_is_NOT_masked():
    """Worth pinning because the opposite is the intuitive guess: since the comb
    SUBTRACTS the neighbouring period rather than thresholding around it, an edge
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
    """The plan's §2.4 alternative: per-hop (1 ms) spectral flux, flag hops exceeding
    median + 12 dB. Returns the worst hop's margin over the median."""
    win = np.hanning(n_fft)
    frames = np.lib.stride_tricks.sliding_window_view(y, n_fft)[::hop] * win
    flux = np.sum(np.abs(np.diff(np.abs(np.fft.rfft(frames, axis=-1)), axis=0)), axis=-1)
    med = float(np.median(flux))
    return 20 * np.log10(float(np.max(flux)) / med) if med > 1e-20 else 999.0


def test_the_flux_outlier_alternative_misses_a_zipper_and_the_residual_rule_does_not():
    """Why this detector is not built on spectral flux, measured rather than asserted.

    §2.4 offers "or a spectral-flux outlier detector" as an equivalent route, and it is
    better than it sounds: a steady oscillator's magnitude spectrum does not change at
    its own edges, so flux does NOT false-fire on a square (measured +2.9 dB against a
    +12 dB rule), and it catches a one-shot seam down to about -40 dB.

    But it has a structural hole exactly where an oscillator gate needs coverage: a
    block-rate parameter zipper is STATIONARY churn, so it lifts the median along with
    every hop and produces no outlier at any magnitude. A LOUD -20 dB zipper reads
    +2.6 dB — indistinguishable from a clean square. The two routes are complementary,
    not equivalent, and §2.4's "or" is doing more work than it appears to.
    """
    sr, f0 = 48000, 440.0
    y, peak = _square(sr, f0)
    loud = peak * 10 ** (-20 / 20.0)

    assert _flux_outlier_margin_db(y) < 12.0, "premise: flux is quiet on a clean square"

    seam = fx.inject_step(y, int(0.15 * sr), loud)
    assert _flux_outlier_margin_db(seam) > 12.0, "premise: flux does catch a one-shot seam"

    zipped = fx.zipper(y, loud, 64)
    assert _flux_outlier_margin_db(zipped) < 12.0, "the flux route's hole: a zipper is invisible to it"
    assert click.detect(zipped, sr, f0_hint=f0).fired, "the residual route must cover that hole"


def test_click_is_standalone_not_in_the_mono_pipeline_registry():
    """Like `stereo_width`: this detector is reference-free and takes (signal, sr), not
    (reference, candidate, sr), so it must not be wired into the pair-wise registry."""
    from quality_lab import pipeline
    assert "click" not in pipeline._DETECTORS
