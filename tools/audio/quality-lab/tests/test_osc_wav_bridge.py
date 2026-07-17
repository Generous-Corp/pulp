"""End-to-end proof that the C++ WAV bridge produces files this lane can analyze.

The bridge (`test/support/wav_bridge.*` + the `pulp-osc-render-wav` tool) renders an
in-tree Pulp oscillator to a WAV *without a plugin bundle*. This test shells out to that
tool, loads the result through the lab's own `audio_io` load path (soundfile), and asserts
the file is what was rendered: the requested sample rate and channel count, and a
fundamental that matches the requested frequency. That is the real end-to-end proof the
offline Python lane can now measure in-tree oscillator output.

The tool is a built C++ binary. It is found via `$PULP_OSC_RENDER_WAV`, or by globbing the
repo's `build*/test/` dirs; if neither yields a binary the test SKIPS (a build artifact,
not a code fact — never silently pass).
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import numpy as np
import pytest

from quality_lab.audio_io import load_wav_info, load_wav_multichannel

# tests/ -> quality-lab -> audio -> tools -> repo root.
REPO_ROOT = Path(__file__).resolve().parents[4]


def _find_tool() -> Path | None:
    override = os.environ.get("PULP_OSC_RENDER_WAV")
    if override:
        p = Path(override)
        return p if p.exists() else None
    for build in sorted(REPO_ROOT.glob("build*")):
        cand = build / "test" / "pulp-osc-render-wav"
        if cand.exists():
            return cand
    return None


TOOL = _find_tool()
requires_tool = pytest.mark.skipif(
    TOOL is None,
    reason="pulp-osc-render-wav not built (set PULP_OSC_RENDER_WAV or build the target)",
)


def _render(tmp_path: Path, **kwargs) -> Path:
    """Invoke the bridge tool and return the WAV path it wrote."""
    out = tmp_path / "osc.wav"
    args = [str(TOOL), "--out", str(out)]
    for key, value in kwargs.items():
        args += [f"--{key.replace('_', '-')}", str(value)]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, f"tool failed: {proc.stderr}\n{proc.stdout}"
    assert out.exists(), f"tool did not write {out}: {proc.stdout}"
    return out


def _fundamental_hz(y: np.ndarray, sr: int) -> float:
    """Estimate the fundamental from the windowed magnitude spectrum's peak bin,
    parabolically interpolated. For these single-oscillator renders the fundamental is
    the strongest partial for every shape, so the spectral peak is the pitch."""
    y = np.asarray(y, dtype=np.float64)
    y = y - np.mean(y)
    win = np.hanning(len(y))
    mag = np.abs(np.fft.rfft(y * win))
    freqs = np.fft.rfftfreq(len(y), 1.0 / sr)
    k = int(np.argmax(mag[1:]) + 1)  # skip DC.
    # Parabolic interpolation over log-magnitude around the peak bin.
    if 1 <= k < len(mag) - 1:
        a, b, c = (np.log(mag[k - 1] + 1e-20), np.log(mag[k] + 1e-20),
                   np.log(mag[k + 1] + 1e-20))
        offset = 0.5 * (a - c) / (a - 2 * b + c + 1e-20)
    else:
        offset = 0.0
    bin_hz = freqs[1] - freqs[0]
    return (k + offset) * bin_hz


@requires_tool
@pytest.mark.parametrize("shape", ["sine", "saw", "square", "triangle"])
@pytest.mark.parametrize("freq", [220.0, 440.0, 1000.0])
def test_bridge_wav_is_readable_and_on_pitch(tmp_path, shape, freq):
    wav = _render(tmp_path, shape=shape, freq=freq, sr=48000, dur_ms=500)

    # The lab's own load path reads it (soundfile) — the readability contract.
    y, sr, channels = load_wav_info(str(wav))
    assert sr == 48000
    assert channels == 1
    assert len(y) == 24000  # 500 ms at 48 kHz.
    assert np.max(np.abs(y)) > 0.05  # not silence.

    est = _fundamental_hz(y, sr)
    # 500 ms gives 2 Hz bin spacing; the interpolated peak lands well inside 2 Hz.
    assert abs(est - freq) < 2.0, f"{shape}@{freq}Hz read back as {est:.2f} Hz"


@requires_tool
def test_bridge_wav_reports_channels_and_rate(tmp_path):
    wav = _render(tmp_path, shape="saw", freq=330.0, sr=44100, dur_ms=250, channels=2)

    y, sr, channels = load_wav_multichannel(str(wav))
    assert sr == 44100
    assert channels == 2
    assert y.shape == (11025, 2)  # round(250 * 44100 / 1000) frames, stereo.
    # The instrument is mono-summed into both channels: they are identical here.
    assert np.allclose(y[:, 0], y[:, 1])
    assert abs(_fundamental_hz(y[:, 0], sr) - 330.0) < 2.0


@requires_tool
def test_bridge_int24_path_is_readable(tmp_path):
    wav = _render(tmp_path, shape="sine", freq=440.0, sr=48000, dur_ms=250, bits="int24")
    y, sr, channels = load_wav_info(str(wav))
    assert sr == 48000
    assert channels == 1
    assert abs(_fundamental_hz(y, sr) - 440.0) < 2.0
