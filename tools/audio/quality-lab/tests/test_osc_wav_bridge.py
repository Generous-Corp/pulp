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


@requires_tool
@pytest.mark.parametrize("engine", ["vco", "dco", "wt"])
def test_bridge_engine_renders_on_pitch(tmp_path, engine):
    """Each oscillator engine (`--engine vco|dco|wt`) renders a readable,
    on-pitch WAV. `vco`/`dco` share the classical-shape parameter; `wt` has
    none (it always plays its own bandlimited saw table), so this exercises
    the engine-selection path end to end for all three."""
    freq = 440.0
    kwargs = dict(engine=engine, freq=freq, sr=48000, dur_ms=500)
    if engine != "wt":
        kwargs["shape"] = "saw"
    wav = _render(tmp_path, **kwargs)

    y, sr, channels = load_wav_info(str(wav))
    assert sr == 48000
    assert channels == 1
    assert len(y) == 24000  # 500 ms at 48 kHz.
    assert np.max(np.abs(y)) > 0.05  # not silence.

    est = _fundamental_hz(y, sr)
    # dco's pitch is quantized (not exact) by design, but at 440 Hz against
    # the tool's 1 MHz default master clock the worst-case error is well
    # under a cent — far inside this 2 Hz bin-spacing tolerance.
    assert abs(est - freq) < 2.0, f"{engine}@{freq}Hz read back as {est:.2f} Hz"


@requires_tool
def test_bridge_seed_selects_vco_jitter_realization(tmp_path):
    """`--seed` only has an audible effect on `vco` with nonzero drift/jitter
    (the noise streams it seeds); it picks which pseudo-random realization
    plays. The same seed must render byte-identical output (the tool's own
    determinism contract — see osc_render_wav.cpp's usage doc); a different
    seed must render a different one."""
    common = dict(engine="vco", freq=440.0, sr=48000, dur_ms=100,
                  jitter_cents=20.0)

    dir_a1 = tmp_path / "seed-a1"
    dir_a2 = tmp_path / "seed-a2"
    dir_b = tmp_path / "seed-b"
    for d in (dir_a1, dir_a2, dir_b):
        d.mkdir()

    wav_a1 = _render(dir_a1, seed=111, **common)
    wav_a2 = _render(dir_a2, seed=111, **common)
    wav_b = _render(dir_b, seed=222, **common)

    assert wav_a1.read_bytes() == wav_a2.read_bytes(), (
        "same --seed must render byte-identical output"
    )
    assert wav_a1.read_bytes() != wav_b.read_bytes(), (
        "different --seed must render a different noise realization"
    )


@requires_tool
@pytest.mark.parametrize("engine,flag", [
    ("dco", "--seed"),
    ("dco", "--drift-cents"),
    ("wt", "--seed"),
    ("wt", "--jitter-cents"),
])
def test_bridge_rejects_vco_only_flags_on_other_engines(tmp_path, engine, flag):
    """`--seed`/`--drift-cents`/`--jitter-cents` apply only to `--engine vco`;
    the tool must reject them on `dco`/`wt` with a clear message rather than
    silently ignoring them."""
    out = tmp_path / "osc.wav"
    args = [str(TOOL), "--out", str(out), "--engine", engine, flag, "5"]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 2, f"expected rejection, got: {proc.stdout}{proc.stderr}"
    assert not out.exists()


@requires_tool
def test_bridge_rejects_shape_on_wt_engine(tmp_path):
    """`--shape` does not apply to `--engine wt` (it plays a fixed wavetable
    set, not a classical shape)."""
    out = tmp_path / "osc.wav"
    args = [str(TOOL), "--out", str(out), "--engine", "wt", "--shape", "saw"]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 2, f"expected rejection, got: {proc.stdout}{proc.stderr}"
    assert not out.exists()
