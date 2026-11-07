"""Decoder differential: Pulp's C++ decoders against libsndfile, bit for bit.

For every lossless seed in the shared audio-decoder fuzz corpus (`.wav`,
`.flac`, `.aiff`), `pulp-audio-decode-dump` (built with the C++ tests) emits a
JSON header and raw little-endian float32 interleaved samples. This test
decodes the same file with `soundfile` (libsndfile) as float32 and requires the
two byte streams to be identical. A seed that only one side decodes is a
finding, never a tolerated case.

Developer lane only: it skips cleanly when `soundfile` is not importable or
`PULP_AUDIO_DECODE_DUMP_BIN` does not name an executable, so it can never hold a
required gate. Lossy formats stay out: libsndfile's Ogg and MP3 paths are
different codecs from Pulp's, so byte equality is not a meaningful property.
approach after SimZhou/vscode-audiolens c5c5a24; re-authored
"""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[4]
CORPUS_DIR = REPO_ROOT / "test" / "fixtures" / "fuzz" / "audio-decoder"
DUMP_ENV = "PULP_AUDIO_DECODE_DUMP_BIN"
# Set by the ctest registration behind PULP_AUDIO_QUALITY_LAB_GATE: there the
# dependencies are promised, so a missing one is a failure, not a skip.
REQUIRED_ENV = "PULP_AUDIO_DECODE_DUMP_REQUIRED"
LOSSLESS_SUFFIXES = (".wav", ".wave", ".flac", ".aiff", ".aif", ".aifc")
DUMP_REFUSED = 2


def _unavailable(reason: str) -> None:
    if os.environ.get(REQUIRED_ENV):
        pytest.fail(f"{REQUIRED_ENV} is set but {reason}")
    pytest.skip(reason)


def _dump_binary() -> str:
    path = os.environ.get(DUMP_ENV, "")
    if not path:
        _unavailable(
            f"{DUMP_ENV} is unset; point it at the pulp-audio-decode-dump built with the C++ tests"
        )
    if not (os.path.isfile(path) and os.access(path, os.X_OK)):
        _unavailable(f"{DUMP_ENV}={path!r} is not an executable file")
    return path


def _soundfile():
    try:
        import soundfile  # noqa: PLC0415 - optional dependency, imported on demand
    except ImportError:
        _unavailable("soundfile (libsndfile) is not installed")
    return soundfile


def _lossless_seeds() -> list[Path]:
    if not CORPUS_DIR.is_dir():
        return []
    return sorted(p for p in CORPUS_DIR.iterdir()
                  if p.is_file() and p.suffix.lower() in LOSSLESS_SUFFIXES)


def _run_dump(binary: str, seed: Path) -> tuple[int, dict | None, bytes]:
    """Returns (exit code, header or None, interleaved sample bytes)."""
    proc = subprocess.run([binary, str(seed)], capture_output=True, timeout=120, check=False)
    if proc.returncode != 0:
        return proc.returncode, None, b""
    newline = proc.stdout.find(b"\n")
    assert newline >= 0, f"{seed.name}: dump emitted no header line"
    header = json.loads(proc.stdout[:newline].decode("ascii"))
    assert header.get("schema") == "pulp.audio.decode_dump.v1", header
    return 0, header, proc.stdout[newline + 1:]


def _soundfile_decode(sf, seed: Path):
    """Returns (rate, float32 C-order frames x channels array) or None when refused."""
    try:
        frames, rate = sf.read(str(seed), dtype="float32", always_2d=True)
    except Exception:  # noqa: BLE001 - any libsndfile refusal counts as "refused"
        return None
    return rate, frames


def test_cpp_decoders_match_libsndfile_bit_exactly():
    sf = _soundfile()
    binary = _dump_binary()
    seeds = _lossless_seeds()
    # The control: an empty or mis-pathed corpus must fail, not pass vacuously.
    assert seeds, f"no lossless seeds under {CORPUS_DIR}"

    findings: list[str] = []
    compared = 0
    for seed in seeds:
        code, header, payload = _run_dump(binary, seed)
        reference = _soundfile_decode(sf, seed)
        pulp_decoded = code == 0
        if code not in (0, DUMP_REFUSED):
            findings.append(f"{seed.name}: dump exited {code}")
            continue
        if pulp_decoded != (reference is not None):
            findings.append(
                f"{seed.name}: pulp {'decoded' if pulp_decoded else 'refused'}, "
                f"libsndfile {'decoded' if reference is not None else 'refused'}"
            )
            continue
        if not pulp_decoded:
            continue  # both refused a malformed seed: agreement

        rate, frames = reference
        assert header is not None
        if header["sample_rate"] != rate or header["channels"] != frames.shape[1] \
                or header["frames"] != frames.shape[0]:
            findings.append(
                f"{seed.name}: shape differs: pulp {header['sample_rate']} Hz "
                f"{header['channels']} ch {header['frames']} frames, libsndfile {rate} Hz "
                f"{frames.shape[1]} ch {frames.shape[0]} frames"
            )
            continue
        expected = frames.astype("<f4", copy=False).tobytes(order="C")
        if payload != expected:
            first = next((i for i in range(0, min(len(payload), len(expected)), 4)
                          if payload[i:i + 4] != expected[i:i + 4]), None)
            findings.append(f"{seed.name}: samples differ, first at byte offset {first}")
            continue
        compared += 1

    assert not findings, "\n".join(findings)
    # At least one seed must be well formed, or the differential compared nothing.
    assert compared > 0, "no seed was decoded by both sides"
