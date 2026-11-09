"""Decoder differential: Pulp's C++ decoders against libsndfile, bit for bit.

For every lossless seed in the shared audio-decoder fuzz corpus (`.wav`,
`.flac`, `.aiff`), `pulp-audio-decode-dump` (built with the C++ tests) emits a
JSON header and raw little-endian float32 interleaved samples. This test
decodes the same file with `soundfile` (libsndfile) as float32 and requires the
two byte streams to be identical. A seed that only one side decodes is an
acceptance split: it must appear in EXPECTED_ACCEPTANCE_SPLITS with its reason
(a deliberate Pulp contract, or a difference tracked in an issue), and a split
not listed there, or a listed split that no longer happens, fails the test.
The sample comparison itself is never relaxed.

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

# Seeds where one decoder accepts and the other refuses BY DESIGN: libsndfile is
# lenient on malformed files that Pulp refuses by contract, or reads a container
# Pulp registers no reader for. Each row is (pulp_decodes, libsndfile_decodes,
# reason), the same columns the C++ replay's seed_expectations() carries as
# must_decode / libsndfile_decodes / reason (test/test_audio_decoder_corpus.cpp),
# so the two tables diff against each other. A split not listed here is a
# finding, so is a listed split that no longer happens, and the sample
# comparison itself is never relaxed.
EXPECTED_ACCEPTANCE_SPLITS: dict[str, tuple[bool, bool, str]] = {
    "aiff-ssnd-past-eof.aiff": (
        False, True, "the SSND chunk runs past the end of the file; Pulp refuses truncated streams"),
    "wav-w64-pcm16-mono.wav": (
        False, True, "Sony Wave64 envelope; Pulp registers no Wave64 reader"),
    "wav-block-align-smaller-than-frame.wav": (
        False, True, "nBlockAlign smaller than channels x sample width; Pulp refuses a frame "
        "layout the fmt chunk contradicts, libsndfile derives the stride"),
    "wav-bw64-ds64.wav": (
        True, False, "BW64 envelope (ITU-R BS.2088); libsndfile does not recognise the tag"),
    "wav-fmt-after-data.wav": (
        True, False, "fmt chunk after the data chunk; Pulp accepts it with a warning, "
        "libsndfile looks for fmt first"),
    "wav-duplicate-fmt-identical.wav": (
        True, False, "a fmt chunk repeated byte for byte; Pulp accepts it with a warning, "
        "libsndfile refuses any second fmt"),
}


def test_every_acceptance_split_states_its_reason():
    # A row without a reason is a split nobody can review; this runs without
    # libsndfile or the dump binary, so it holds wherever the module is collected.
    for name, (pulp_decodes, sf_decodes, reason) in EXPECTED_ACCEPTANCE_SPLITS.items():
        assert pulp_decodes != sf_decodes, f"{name}: a split row must name two different outcomes"
        assert len(reason.strip()) >= 20, f"{name}: the split's reason is missing or too short"
    for name in ("wav-bw64-ds64.wav", "wav-duplicate-fmt-identical.wav", "wav-fmt-after-data.wav"):
        assert name in EXPECTED_ACCEPTANCE_SPLITS, f"{name}: expected a listed libsndfile split"
        assert EXPECTED_ACCEPTANCE_SPLITS[name][:2] == (True, False), name
    assert "BW64" in EXPECTED_ACCEPTANCE_SPLITS["wav-bw64-ds64.wav"][2]
    assert "fmt" in EXPECTED_ACCEPTANCE_SPLITS["wav-duplicate-fmt-identical.wav"][2]
    assert "after the data" in EXPECTED_ACCEPTANCE_SPLITS["wav-fmt-after-data.wav"][2]


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
    outcomes: dict[str, tuple[bool, bool]] = {}  # seed -> (pulp decoded, libsndfile decoded)
    for seed in seeds:
        code, header, payload = _run_dump(binary, seed)
        reference = _soundfile_decode(sf, seed)
        pulp_decoded = code == 0
        outcomes[seed.name] = (pulp_decoded, reference is not None)
        if code not in (0, DUMP_REFUSED):
            findings.append(f"{seed.name}: dump exited {code}")
            continue
        if pulp_decoded != (reference is not None):
            expected = EXPECTED_ACCEPTANCE_SPLITS.get(seed.name)
            if expected is not None and expected[:2] == (pulp_decoded, reference is not None):
                continue  # a documented contract difference, not a finding
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

    for name, (pulp_decodes, sf_decodes, _reason) in EXPECTED_ACCEPTANCE_SPLITS.items():
        observed = outcomes.get(name)
        if observed is None:
            findings.append(f"{name}: listed in EXPECTED_ACCEPTANCE_SPLITS but absent from the corpus")
        elif observed != (pulp_decodes, sf_decodes):
            findings.append(f"{name}: EXPECTED_ACCEPTANCE_SPLITS row is stale; the split no longer happens")

    assert not findings, "\n".join(findings)
    # At least one seed must be well formed, or the differential compared nothing.
    assert compared > 0, "no seed was decoded by both sides"
