"""Worst-region artifact export tests (§5 P2)."""
from __future__ import annotations

import json
import os

from quality_lab import audio_io, pipeline


def test_export_writes_full_and_region_clips(tmp_path):
    out = str(tmp_path / "run")
    report = pipeline.run_and_export("smear", out)

    # full renders always written
    assert os.path.exists(os.path.join(out, "reference.wav"))
    assert os.path.exists(os.path.join(out, "candidate.wav"))

    listening = report["listening"]
    assert listening["regions"], "smear should localize at least one worst region"
    for r in listening["regions"]:
        assert os.path.exists(os.path.join(out, r["reference_clip"]))
        assert os.path.exists(os.path.join(out, r["candidate_clip"]))
        # clip is short (well under the full render)
        y, sr = audio_io.load_wav(os.path.join(out, r["candidate_clip"]))
        assert 0 < len(y) <= int(0.5 * sr)


def test_identity_export_has_no_regions(tmp_path):
    out = str(tmp_path / "id")
    report = pipeline.run_and_export("identity", out)
    assert os.path.exists(os.path.join(out, "candidate.wav"))
    assert report["listening"]["regions"] == []  # nothing fired -> nothing to localize


def test_report_json_roundtrips(tmp_path):
    out = str(tmp_path / "r")
    report = pipeline.run_and_export("dull", out)
    s = json.dumps(report)  # must be JSON-serializable end to end
    assert json.loads(s)["verdict"] == report["verdict"]
