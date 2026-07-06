"""Raw-comparator + corroboration (S4) — the advisory, off-gate namespace of the compare report.

The `null_residual` raw comparator is a deterministic, level-matched, phase-sensitive
sample-domain measure. Corroboration is a MATERIALITY cross-check (does the raw residual ALSO
register a change?), NEVER a trust score — so these tests pin three things: the numbers are
right, the advisory block never moves the verdict, and disagreement is surfaced honestly (a
change the axis can't see, or an axis flag the raw residual won't back).
"""
from __future__ import annotations

import numpy as np

from quality_lab import compare, generate, schema
from quality_lab.dsp import null_residual_db


def _drum(seed: int = 0):
    y, _ = generate.render_drum_break(sr=48000, seed=seed)
    return y, 48000


# ── dsp.null_residual_db primitive ───────────────────────────────────────────────────────

def test_null_residual_identity_is_deep_floor():
    ref, _ = _drum()
    nr = null_residual_db(ref, ref.copy())
    assert nr.residual_db == -160.0    # bit-identical → finite deep floor, not -inf
    assert np.isfinite(nr.ref_rms_db) and nr.level_matched is True


def test_null_residual_grows_with_difference():
    ref, _ = _drum()
    small = null_residual_db(ref, ref + 1e-3 * np.random.RandomState(1).randn(ref.size)).residual_db
    large = null_residual_db(ref, ref + 1e-1 * np.random.RandomState(1).randn(ref.size)).residual_db
    assert small < large           # more added noise → higher residual
    assert np.isfinite(small) and np.isfinite(large)


def test_null_residual_zero_pads_length_mismatch():
    ref, _ = _drum()
    # A shorter candidate must not raise, and the DROPPED tail must count: the shorter signal is
    # zero-padded to the reference length, so half a render reads as clearly material (not the
    # near-identity a silent truncation would show).
    finite = null_residual_db(ref, ref[:-1].copy())[0]
    assert np.isfinite(finite)
    half = null_residual_db(ref, ref[: ref.size // 2].copy())[0]
    assert half > -60.0            # a dropped half is a material residual, not identity


def test_null_residual_trailing_silence_is_immaterial():
    ref, _ = _drum()
    # Appending trailing SILENCE is not a material change — the padded region matches the
    # reference's own zero-pad, so the residual stays at the identity floor.
    padded = np.concatenate([ref, np.zeros(2000)])
    assert null_residual_db(ref, padded)[0] == -160.0


def test_null_residual_silent_reference_is_not_applicable_sentinel():
    silent = np.zeros(4096)
    nr = null_residual_db(silent, np.ones(4096))
    assert nr.residual_db == float("inf") and nr.ref_rms_db == float("-inf")


def test_null_residual_silent_overlap_loud_tail_stays_finite_and_material():
    """Codex round-3 must-fix: a silent OVERLAP with loud non-overlap content must NOT drop to the
    not-applicable sentinel — that would hide a dropped/shifted loud tail. The residual is computed
    raw (no gain from the silent overlap) and flagged `level_matched=False`, staying material."""
    loud = 0.3 * np.sin(2 * np.pi * 440 * np.arange(4096) / 48000)
    # reference = silent head + loud tail; candidate = only the silent head (loud tail dropped)
    ref = np.concatenate([np.zeros(4096), loud])
    nr = null_residual_db(ref, np.zeros(4096))
    assert np.isfinite(nr.residual_db) and nr.residual_db > -60.0   # material, not dropped
    assert nr.level_matched is False
    # full-render delay (candidate = silence then the tone): overlap silent on the candidate side
    nr2 = null_residual_db(loud, np.concatenate([np.zeros(4096), loud]))
    assert np.isfinite(nr2.residual_db) and nr2.residual_db > -60.0
    assert nr2.level_matched is False


# ── corroboration through the full report ────────────────────────────────────────────────

def test_material_change_is_corroborated():
    """A real dulling: the axis fires AND the sample-domain residual is material → they agree."""
    ref, sr = _drum()
    cand = generate.dull(ref, sr)
    report = compare.compare_arrays(ref, cand, sr, reference_role="golden")
    adv = report["advisory"]
    raw = adv["raw_comparators"][0]
    assert raw["name"] == "null_residual"
    assert raw["maturity"] == "experimental" and raw["participates_in_verdict"] is False
    assert raw["detail"]["level_match_applied"] is True   # normal same-length case matches cleanly
    assert np.isfinite(raw["value"])
    assert adv["corroboration"]["status"] == compare.CORROBORATED
    assert adv["corroboration"]["basis"]["axis_exceeds"] is True
    assert adv["corroboration"]["basis"]["raw_material"] is True


def test_identity_change_is_corroborated_as_immaterial():
    ref, sr = _drum()
    report = compare.compare_arrays(ref, ref.copy(), sr, reference_role="golden")
    adv = report["advisory"]
    assert report["verdict"] == compare.VERDICT_NO_CHANGE
    assert adv["corroboration"]["status"] == compare.CORROBORATED
    assert adv["corroboration"]["basis"]["axis_exceeds"] is False
    assert adv["corroboration"]["basis"]["raw_material"] is False


def test_pure_delay_is_not_corroborated_change_axis_cannot_see():
    """A whole-signal sample shift leaves the LTAS centroid unchanged (no material tonal
    change) but produces a large sample-domain residual — the honest 'a real difference this
    axis does not capture' signal. This is the disagreement corroboration must surface, and it
    must NOT change the verdict."""
    ref, sr = _drum()
    cand = np.roll(ref, 64)
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance", reference_role="golden")
    assert report["verdict"] == compare.VERDICT_NO_CHANGE          # axis sees no tonal change
    adv = report["advisory"]
    assert adv["corroboration"]["status"] == compare.NOT_CORROBORATED
    assert adv["corroboration"]["basis"]["axis_exceeds"] is False
    assert adv["corroboration"]["basis"]["raw_material"] is True   # raw residual DOES see it


def _sine(seconds: float, sr: int = 48000, hz: float = 440.0):
    t = np.arange(int(seconds * sr)) / sr
    return 0.3 * np.sin(2 * np.pi * hz * t), sr


def test_truncated_candidate_is_not_falsely_corroborated_as_identity():
    """The must-fix: a candidate that is the first half of the reference must NOT read as an
    immaterial identity match. Zero-padding the shorter signal makes the dropped tail count as
    residual, so raw_material is True even though the axis sees identity on the shared content."""
    ref, sr = _sine(2.0)
    cand = ref[: ref.size // 2].copy()               # first half only
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance", reference_role="golden")
    adv = report["advisory"]
    corr = adv["corroboration"]
    assert corr["status"] == compare.NOT_CORROBORATED         # NOT a false "corroborated"
    assert corr["basis"]["raw_material"] is True              # dropped tail → material residual...
    assert corr["basis"]["axis_exceeds"] is False            # ...even though the axis sees identity
    assert corr["basis"]["cand_samples"] < corr["basis"]["ref_samples"]
    assert "duration" in corr["note"]
    # The raw comparator itself exposes the sample counts — the truncation is never hidden.
    detail = adv["raw_comparators"][0]["detail"]
    assert detail["ref_samples"] == ref.size and detail["cand_samples"] == cand.size


def test_small_absolute_content_truncation_on_long_render_is_material():
    """Codex's second must-fix: on a long render, dropping a small FRACTION of content is still a
    large ABSOLUTE tail — a fraction-only threshold hid it. Zero-padding registers the missing
    content proportionally, so ~1% of a long render (a real dropped tail) reads material."""
    ref, sr = _sine(2.5, hz=440.0)                   # 120000 samples
    cand = ref[:-990].copy()                         # 990 dropped → 0.8%, but real content
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance", reference_role="golden")
    corr = report["advisory"]["corroboration"]
    assert corr["basis"]["raw_material"] is True
    assert corr["status"] == compare.NOT_CORROBORATED


def test_extended_candidate_with_content_is_material():
    """A candidate materially LONGER than the reference (extra content, not silence) is flagged."""
    ref, sr = _sine(1.0)
    cand = np.concatenate([ref, ref])                # twice as long, real content
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance", reference_role="peer")
    corr = report["advisory"]["corroboration"]
    assert corr["basis"]["raw_material"] is True
    assert corr["basis"]["cand_samples"] > corr["basis"]["ref_samples"]


def test_silent_overlap_material_tail_still_reports_advisory():
    """Codex round-3 must-fix, through the full report: a full-render delay (candidate = a block of
    silence then the reference tone) has a silent overlap, but it is a real change. The advisory
    must still be present and flag it material (not silently dropped)."""
    tone, sr = _sine(0.5)                            # 24000 samples
    delayed = np.concatenate([np.zeros(tone.size), tone])   # same tone, pushed past the overlap
    report = compare.compare_arrays(tone, delayed, sr, profile="added-hf", reference_role="peer")
    assert "advisory" in report                      # NOT dropped
    corr = report["advisory"]["corroboration"]
    assert corr["basis"]["raw_material"] is True
    assert report["advisory"]["raw_comparators"][0]["detail"]["level_match_applied"] is False


def test_trailing_silence_pad_stays_immaterial():
    """The honest counterpart: appending trailing SILENCE is not a material change — the residual
    over the padded (zero) region is zero, so it stays an identity match. This is what keeps the
    zero-pad rule from turning every block-boundary padding into a spurious material flag."""
    ref, sr = _sine(2.0)
    cand = np.concatenate([ref, np.zeros(1500)])     # +31ms of silence
    report = compare.compare_arrays(ref, cand, sr, profile="tonal-balance", reference_role="golden")
    corr = report["advisory"]["corroboration"]
    assert corr["basis"]["raw_material"] is False
    assert corr["status"] == compare.CORROBORATED    # identity content + silence pad → agree


def test_corroboration_never_moves_the_verdict():
    """The verdict from a report WITH advisory must equal the verdict computed from the same
    inputs — corroboration is advisory-only and cannot flip a judgment either way."""
    ref, sr = _drum()
    for cand, role in [(generate.dull(ref, sr), "golden"),
                       (np.roll(ref, 64), "golden"),
                       (ref.copy(), "peer")]:
        report = compare.compare_arrays(ref, cand, sr, reference_role=role)
        # Recompute the verdict purely from the primary measurement + role; advisory absent.
        primary = report["measurements"][0]
        axis = compare._resolve(report["profile"])
        assert report["verdict"] == compare._verdict(axis, primary, role)


def test_corroboration_note_disclaims_trust_score():
    """The plan's non-goal: corroboration must read as a materiality cross-check, never trust."""
    ref, sr = _drum()
    report = compare.compare_arrays(ref, generate.dull(ref, sr), sr, reference_role="peer")
    note = report["advisory"]["corroboration"]["note"].lower()
    assert "not a trust score" in note
    assert "materiality" in note
    assert "disagreement is legitimate" in note


# ── advisory is absent when the primary measurement can't support it ──────────────────────

def test_no_advisory_when_not_applicable():
    ref = np.zeros(1024)  # shorter than one LTAS frame → not_applicable
    report = compare.compare_arrays(ref, ref.copy(), 48000, reference_role="peer")
    assert report["measurements"][0]["status"] == compare.STATUS_NOT_APPLICABLE
    assert "advisory" not in report

def test_no_advisory_when_invalid():
    ref, sr = _drum()
    bad = ref.copy()
    bad[10] = np.nan
    report = compare.compare_arrays(ref, bad, sr, reference_role="peer")
    assert report["verdict"] == compare.VERDICT_INVALID
    assert "advisory" not in report


# ── schema constructors (shape ownership) ────────────────────────────────────────────────

def test_raw_comparator_relational_and_per_side_shapes():
    relational = schema.compare_raw_comparator("null_residual", "t", "db", "n", value=-40.0)
    assert relational["value"] == -40.0 and "ref_value" not in relational
    assert relational["participates_in_verdict"] is False and relational["maturity"] == "experimental"
    per_side = schema.compare_raw_comparator("tempo", "t", "bpm", "n", ref_value=120.0,
                                             cand_value=124.0, delta=4.0)
    assert per_side["ref_value"] == 120.0 and per_side["cand_value"] == 124.0 and per_side["delta"] == 4.0


def test_advisory_constructor_defaults_empty():
    adv = schema.compare_advisory()
    assert adv == {"raw_comparators": [], "corroboration": None}
