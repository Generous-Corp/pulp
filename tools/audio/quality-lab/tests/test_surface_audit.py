"""Current-surface audit (plan Step 0): every case's declared `detector_tags` must
actually be emitted by that case's runner (or be a registered detector), and the
standalone `stereo_width` detector must stay out of the mono pipeline registry.

This guards against the class of bug found during planning — the real-audio case once
listed `hnr` in its tags while its runner only emitted centroid/fizz/flux.
"""
from __future__ import annotations

from quality_lab import pipeline


def test_pipeline_cases_emit_exactly_their_registered_tags():
    for case in (pipeline.P0A_CASE, pipeline.TONAL_CASE):
        report = pipeline.run("identity", case=case)
        emitted = {d["name"] for d in report["detectors"]}
        for tag in case.detector_tags:
            assert tag in pipeline._DETECTORS, \
                f"{case.case_id}: tag {tag!r} is not in the detector registry"
            assert tag in emitted, \
                f"{case.case_id}: tag {tag!r} declared but not emitted by run()"


def test_stereo_width_is_standalone_not_in_the_mono_registry():
    # stereo_width operates on (N,2) arrays; it must not be wired into the mono pipeline
    # (which downmixes) or it would run on mono cases and silently misbehave.
    assert "stereo_width" not in pipeline._DETECTORS


def test_real_audio_tags_are_derived_from_results():
    """run_real_audio builds its detector list by hand; its tags must be derived from the
    emitted results (not a separately-maintained literal), so they can never drift. We
    assert the source contract structurally without needing a built stretchcli."""
    import inspect
    src = inspect.getsource(pipeline.run_real_audio)
    assert "detector_tags=[r.name for r in results]" in src, \
        "run_real_audio must derive detector_tags from its results list"
