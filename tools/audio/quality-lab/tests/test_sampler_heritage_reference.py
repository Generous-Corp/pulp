"""Independent quality evidence for the production SampleHeritageEngine SRC.

The configured CMake gate supplies a renderer that exercises the exact two-leg
production engine.  This module supplies its own causal, zero-prehistory,
64-lobe Kaiser-beta13 sinc oracle: it shares no coefficient table or resampler
implementation with the engine.  Phase is fixed from the declared streaming
contract, never estimated: each active leg has 24 frames of lookahead delay and
the two-leg source-domain delay is ``Dsrc = hA + hB * H/M``.

Ordinary pytest collection skips the external integration.  The opt-in CMake
gate sets ``PULP_SAMPLER_AQL_REQUIRED=1``, making a missing renderer a collection
error rather than skipped evidence.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import numpy as np
import pytest

from quality_lab.audio_io import load_wav


RATIOS = (0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0)
FRAMES = 16384
OUTPUT_FREQUENCY = 521.0 / FRAMES
EDGE_TRIM = 768
PRODUCTION_HALF_WIDTH = 24.0
ORACLE_HALF_WIDTH = 64
ORACLE_BETA = 13.0


def _configured_tool() -> Path | None:
    override = os.environ.get("PULP_SAMPLER_HERITAGE_RENDER_WAV")
    if not override:
        if os.environ.get("PULP_SAMPLER_AQL_REQUIRED") == "1":
            raise RuntimeError(
                "PULP_SAMPLER_HERITAGE_RENDER_WAV is required by the configured "
                "SampleHeritageEngine Quality Lab gate"
            )
        return None
    path = Path(override)
    if not path.is_file():
        raise RuntimeError(f"heritage sampler renderer is unavailable: {path}")
    return path


TOOL = _configured_tool()
pytestmark = pytest.mark.skipif(
    TOOL is None,
    reason="heritage renderer is not configured outside the opt-in CMake gate",
)


def _render(
    tmp_path: Path,
    input_ratio: float,
    return_ratio: float,
    *,
    source_frequency: float,
    frames: int = FRAMES,
    block_size: int = 64,
    impulse_index: int | None = None,
    suffix: str = "",
) -> tuple[np.ndarray, np.ndarray]:
    assert TOOL is not None
    stem = f"a{input_ratio}-b{return_ratio}-n{block_size}{suffix}"
    source_path = tmp_path / f"source-{stem}.wav"
    candidate_path = tmp_path / f"candidate-{stem}.wav"
    command = [
        str(TOOL),
        "--source-out", str(source_path),
        "--candidate-out", str(candidate_path),
        "--input-ratio", str(input_ratio),
        "--return-ratio", str(return_ratio),
        "--source-frequency", str(source_frequency),
        "--frames", str(frames),
        "--block-size", str(block_size),
    ]
    if impulse_index is not None:
        command += ["--impulse-index", str(impulse_index)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert completed.returncode == 0, completed.stderr + "\n" + completed.stdout
    source, source_rate = load_wav(str(source_path))
    candidate, candidate_rate = load_wav(str(candidate_path))
    assert source_rate == candidate_rate == 48000
    assert candidate.size == frames
    return source.astype(np.float64), candidate.astype(np.float64)


def _kaiser_sinc_at(source: np.ndarray, positions: np.ndarray,
                    ratio: float) -> np.ndarray:
    """Evaluate an independently designed finite sinc at declared positions."""
    centers = np.floor(positions).astype(np.int64)
    fractions = np.round(positions - centers, 12)
    offsets = np.arange(1 - ORACLE_HALF_WIDTH,
                        ORACLE_HALF_WIDTH + 1, dtype=np.int64)
    cutoff = min(1.0, 1.0 / ratio)
    result = np.zeros(positions.size, dtype=np.float64)
    # The evidence matrix uses quarter-integer ratios, hence at most four
    # phases.  Designing each independent phase once avoids making np.i0 the
    # runtime cost of every sample while preserving the same oracle exactly.
    for fraction in np.unique(fractions):
        selected = np.flatnonzero(fractions == fraction)
        distance = offsets.astype(np.float64) - fraction
        normalized = distance / float(ORACLE_HALF_WIDTH)
        window = np.zeros_like(distance)
        inside = np.abs(normalized) < 1.0
        window[inside] = (
            np.i0(ORACLE_BETA * np.sqrt(1.0 - normalized[inside] ** 2))
            / np.i0(ORACLE_BETA)
        )
        coefficients = cutoff * np.sinc(cutoff * distance) * window
        coefficients /= np.sum(coefficients)
        indices = centers[selected, None] + offsets[None, :]
        valid = (indices >= 0) & (indices < source.size)
        clipped = np.clip(indices, 0, max(0, source.size - 1))
        samples = source[clipped]
        samples[~valid] = 0.0
        result[selected] = samples @ coefficients
    return result


def _leg(source: np.ndarray, ratio: float, output_frames: int) -> np.ndarray:
    if ratio == 1.0:
        result = np.zeros(output_frames, dtype=np.float64)
        copied = min(output_frames, source.size)
        result[:copied] = source[:copied]
        return result
    positions = (np.arange(output_frames, dtype=np.float64) * ratio
                 - PRODUCTION_HALF_WIDTH)
    return _kaiser_sinc_at(source, positions, ratio)


def _zero_delay_leg(source: np.ndarray, ratio: float,
                    output_frames: int) -> np.ndarray:
    positions = np.arange(output_frames, dtype=np.float64) * ratio
    return _kaiser_sinc_at(source, positions, ratio)


def _oracle(source: np.ndarray, input_ratio: float, return_ratio: float,
            output_frames: int) -> np.ndarray:
    if return_ratio == 1.0:
        machine_frames = output_frames
    else:
        last_center = ((output_frames - 1) * return_ratio
                       - PRODUCTION_HALF_WIDTH)
        machine_frames = max(0, int(np.floor(last_center)) + ORACLE_HALF_WIDTH + 2)
    machine = _leg(source, input_ratio, machine_frames)
    return _leg(machine, return_ratio, output_frames)


def _gain_and_residual_db(reference: np.ndarray,
                          candidate: np.ndarray) -> tuple[float, float]:
    denominator = float(np.dot(reference, reference))
    assert denominator > 0.0
    gain = float(np.dot(reference, candidate) / denominator)
    residual = candidate - gain * reference
    gain_db = 20.0 * np.log10(abs(gain))
    residual_db = 10.0 * np.log10(
        max(float(np.dot(residual, residual)) / denominator, 1e-30))
    return gain_db, residual_db


def _tone_spur_db(candidate: np.ndarray, frequency: float) -> float:
    frames = np.arange(candidate.size, dtype=np.float64)
    basis = np.column_stack((
        np.sin(2.0 * np.pi * frequency * frames),
        np.cos(2.0 * np.pi * frequency * frames),
    ))
    fit = basis @ np.linalg.lstsq(basis, candidate, rcond=None)[0]
    signal_power = float(np.dot(fit, fit))
    residual = candidate - fit
    return 10.0 * np.log10(
        max(float(np.dot(residual, residual)) / signal_power, 1e-30))


@pytest.mark.parametrize("ratio", RATIOS)
@pytest.mark.parametrize("leg", ("a", "b"))
def test_isolated_heritage_legs_match_causal_reference(
    tmp_path: Path, ratio: float, leg: str
) -> None:
    input_ratio, return_ratio = ((ratio, 1.0) if leg == "a" else (1.0, ratio))
    source_frequency = OUTPUT_FREQUENCY / ratio
    source, candidate = _render(
        tmp_path, input_ratio, return_ratio, source_frequency=source_frequency)
    reference = _oracle(source, input_ratio, return_ratio, candidate.size)
    interior = slice(EDGE_TRIM, -EDGE_TRIM)
    gain_db, residual_db = _gain_and_residual_db(
        reference[interior], candidate[interior])
    spur_db = _tone_spur_db(candidate[interior], OUTPUT_FREQUENCY)

    assert abs(gain_db) < 0.10, (leg, ratio, gain_db, residual_db)
    assert residual_db < -65.0, (leg, ratio, gain_db, residual_db)
    assert spur_db < -55.0, (leg, ratio, spur_db)


@pytest.mark.parametrize("input_ratio,return_ratio", (
    (0.5, 1.5), (0.75, 2.0), (1.25, 0.75), (1.5, 1.25), (2.0, 0.5),
))
def test_two_leg_heritage_pipeline_matches_causal_reference(
    tmp_path: Path, input_ratio: float, return_ratio: float
) -> None:
    overall = input_ratio * return_ratio
    source_frequency = OUTPUT_FREQUENCY / overall
    source, candidate = _render(
        tmp_path, input_ratio, return_ratio, source_frequency=source_frequency)
    reference = _oracle(source, input_ratio, return_ratio, candidate.size)
    interior = slice(EDGE_TRIM, -EDGE_TRIM)
    gain_db, residual_db = _gain_and_residual_db(
        reference[interior], candidate[interior])

    assert abs(gain_db) < 0.20, (input_ratio, return_ratio, gain_db, residual_db)
    assert residual_db < -59.0, (input_ratio, return_ratio, gain_db, residual_db)


def test_impulse_latency_uses_pinned_two_leg_phase(tmp_path: Path) -> None:
    input_ratio, return_ratio = 0.75, 1.5
    impulse_index = 2048
    _, candidate = _render(
        tmp_path, input_ratio, return_ratio, source_frequency=0.01,
        frames=4096, impulse_index=impulse_index)
    source_delay = PRODUCTION_HALF_WIDTH + PRODUCTION_HALF_WIDTH * input_ratio
    expected_peak = (impulse_index + source_delay) / (input_ratio * return_ratio)

    assert abs(int(np.argmax(np.abs(candidate))) - expected_peak) <= 1.0


@pytest.mark.parametrize("input_ratio,return_ratio", ((2.0, 1.0), (1.0, 2.0)))
def test_each_downsampling_leg_rejects_above_nyquist_source(
    tmp_path: Path, input_ratio: float, return_ratio: float
) -> None:
    source, candidate = _render(
        tmp_path, input_ratio, return_ratio, source_frequency=0.45)
    interior = candidate[EDGE_TRIM:-EDGE_TRIM]
    source_rms = np.sqrt(np.mean(source * source))
    rejection_db = 20.0 * np.log10(
        max(float(np.sqrt(np.mean(interior * interior)) / source_rms), 1e-15))
    assert rejection_db < -55.0, (input_ratio, return_ratio, rejection_db)


def test_clock_direction_and_block_partition_are_explicit(tmp_path: Path) -> None:
    input_ratio, return_ratio = 0.75, 2.0
    source_frequency = OUTPUT_FREQUENCY / (input_ratio * return_ratio)
    _, block_64 = _render(
        tmp_path, input_ratio, return_ratio, source_frequency=source_frequency,
        frames=4096, block_size=64, suffix="-64")
    _, block_257 = _render(
        tmp_path, input_ratio, return_ratio, source_frequency=source_frequency,
        frames=4096, block_size=257, suffix="-257")
    interior = block_64[EDGE_TRIM:-EDGE_TRIM]

    assert np.array_equal(block_64, block_257)
    assert _tone_spur_db(interior, OUTPUT_FREQUENCY) < -55.0
    assert _tone_spur_db(interior, source_frequency / (input_ratio * return_ratio)) > -20.0


def test_phase_and_clock_negative_controls_fail_reference_contract(
    tmp_path: Path,
) -> None:
    input_ratio, return_ratio = 0.75, 1.5
    overall = input_ratio * return_ratio
    source, candidate = _render(
        tmp_path, input_ratio, return_ratio,
        source_frequency=OUTPUT_FREQUENCY / overall)
    correct = _oracle(source, input_ratio, return_ratio, candidate.size)
    wrong_phase = _zero_delay_leg(
        _zero_delay_leg(source, input_ratio, candidate.size),
        return_ratio, candidate.size)
    wrong_clock = _oracle(source, input_ratio, 1.0 / return_ratio,
                          candidate.size)
    interior = slice(EDGE_TRIM, -EDGE_TRIM)

    _, correct_db = _gain_and_residual_db(correct[interior], candidate[interior])
    _, wrong_phase_db = _gain_and_residual_db(
        wrong_phase[interior], candidate[interior])
    _, wrong_clock_db = _gain_and_residual_db(
        wrong_clock[interior], candidate[interior])
    assert correct_db < -59.0
    assert wrong_phase_db > -20.0
    assert wrong_clock_db > -20.0


def test_spur_measurement_has_a_working_negative_control(tmp_path: Path) -> None:
    ratio = 1.5
    source, candidate = _render(
        tmp_path, ratio, 1.0, source_frequency=OUTPUT_FREQUENCY / ratio)
    reference = _oracle(source, ratio, 1.0, candidate.size)
    interior = slice(EDGE_TRIM, -EDGE_TRIM)
    corrupted = candidate.copy()
    frames = np.arange(corrupted.size, dtype=np.float64)
    corrupted += 0.5 * 10.0 ** (-50.0 / 20.0) * np.sin(
        2.0 * np.pi * (3101.0 / FRAMES) * frames + 0.7)

    _, clean_db = _gain_and_residual_db(reference[interior], candidate[interior])
    _, corrupt_db = _gain_and_residual_db(reference[interior], corrupted[interior])
    assert clean_db < -65.0
    assert -52.0 < corrupt_db < -48.0
