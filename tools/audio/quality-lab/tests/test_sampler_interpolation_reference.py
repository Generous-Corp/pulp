"""Independent offline reference for Pulp's production sample interpolation.

The C++ helper renders the real interpolation policies to float WAV.  This test
then constructs its reference with Quality Lab's separate 64-lobe Kaiser-beta13
numpy resampler.  The implementations share neither coefficients nor phase
tables.  The configured gate fails when the opt-in C++ artifact or its explicit
``PULP_SAMPLER_RENDER_WAV`` path is unavailable; it never skips this evidence.
Ordinary Quality Lab collection skips this dependency-bearing module when that
artifact has not been configured.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import numpy as np
import pytest

from quality_lab.audio_io import load_wav
from quality_lab.dsp import resample_to_length


RATIOS = (0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0)
OUTPUT_FRAMES = 16385  # N-1 is divisible by every ratio denominator above.
OUTPUT_CYCLES = 521.0 / 16384.0
EDGE_TRIM = 256


def _configured_tool() -> Path | None:
    override = os.environ.get("PULP_SAMPLER_RENDER_WAV")
    if not override:
        if os.environ.get("PULP_SAMPLER_AQL_REQUIRED") == "1":
            raise RuntimeError(
                "PULP_SAMPLER_RENDER_WAV is required; the configured Quality Lab "
                "gate must name the exact production renderer artifact"
            )
        return None
    path = Path(override)
    if not path.is_file():
        raise RuntimeError(f"sampler renderer is unavailable: {path}")
    return path


TOOL = _configured_tool()
pytestmark = pytest.mark.skipif(
    TOOL is None,
    reason="sampler renderer is not configured outside the opt-in CMake gate",
)


def _render(tmp_path: Path, ratio: float) -> tuple[np.ndarray, np.ndarray]:
    assert TOOL is not None
    source_path = tmp_path / f"source-{ratio}.wav"
    candidate_path = tmp_path / f"candidate-{ratio}.wav"
    source_frequency = OUTPUT_CYCLES / ratio
    command = [
        str(TOOL),
        "--source-out", str(source_path),
        "--candidate-out", str(candidate_path),
        "--policy", "ratio-sinc",
        "--ratio", str(ratio),
        "--source-frequency", str(source_frequency),
        "--frames", str(OUTPUT_FRAMES),
        "--block-size", "64",
    ]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert completed.returncode == 0, completed.stderr + "\n" + completed.stdout
    source, source_rate = load_wav(str(source_path))
    candidate, candidate_rate = load_wav(str(candidate_path))
    assert source_rate == candidate_rate == 48000
    assert candidate.size == OUTPUT_FRAMES
    return source, candidate


def _gain_and_residual_db(reference: np.ndarray,
                          candidate: np.ndarray) -> tuple[float, float]:
    reference = np.asarray(reference, dtype=np.float64)
    candidate = np.asarray(candidate, dtype=np.float64)
    denominator = float(np.dot(reference, reference))
    assert denominator > 0.0
    gain = float(np.dot(reference, candidate) / denominator)
    residual = candidate - gain * reference
    gain_db = 20.0 * np.log10(abs(gain))
    residual_db = 10.0 * np.log10(
        max(float(np.dot(residual, residual)) / denominator, 1e-30))
    return gain_db, residual_db


@pytest.mark.parametrize("ratio", RATIOS)
def test_ratio_sinc_matches_independent_offline_reference(
    tmp_path: Path, ratio: float
) -> None:
    source, candidate = _render(tmp_path, ratio)
    reference = resample_to_length(source, candidate.size, zeros=32, beta=13.0)

    # Both implementations have finite edge support with intentionally different
    # policies.  G6 is about the steady interpolation response, so compare the
    # shared, fully supported interior rather than grading either edge policy.
    interior = slice(EDGE_TRIM, -EDGE_TRIM)
    gain_db, residual_db = _gain_and_residual_db(
        reference[interior], candidate[interior])

    assert abs(gain_db) < 0.10, (ratio, gain_db, residual_db)
    assert residual_db < -65.0, (ratio, gain_db, residual_db)


def test_reference_comparison_has_a_working_negative_control(tmp_path: Path) -> None:
    source, candidate = _render(tmp_path, 1.5)
    reference = resample_to_length(source, candidate.size, zeros=32, beta=13.0)
    interior = slice(EDGE_TRIM, -EDGE_TRIM)

    corrupted = candidate.copy()
    frames = np.arange(corrupted.size, dtype=np.float64)
    corrupted += 0.5 * 10.0 ** (-50.0 / 20.0) * np.sin(
        2.0 * np.pi * (3101.0 / 16384.0) * frames + 0.7)
    _, clean_residual_db = _gain_and_residual_db(
        reference[interior], candidate[interior])
    _, corrupt_residual_db = _gain_and_residual_db(
        reference[interior], corrupted[interior])

    assert clean_residual_db < -65.0
    assert -52.0 < corrupt_residual_db < -48.0
