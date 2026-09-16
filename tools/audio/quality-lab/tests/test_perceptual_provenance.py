"""Layer B perceptual adapter (opt-in, license-fenced) + self-describing provenance."""
from __future__ import annotations

import json
import os
import stat

from quality_lab import perceptual, pipeline, provenance


def test_visqol_skips_when_env_unset(monkeypatch):
    """With no PULP_VISQOL_BIN, the adapter SKIPS gracefully — never errors, never a gate."""
    monkeypatch.delenv(perceptual.VISQOL_ENV, raising=False)
    r = perceptual.run_visqol("ref.wav", "cand.wav")
    assert r["status"] == "skipped"
    assert r["mos_lqo"] is None
    assert "not set" in r["reason"]


def test_visqol_skips_when_binary_missing(monkeypatch):
    monkeypatch.setenv(perceptual.VISQOL_ENV, "/nonexistent/visqol-binary-xyz")
    r = perceptual.run_visqol("ref.wav", "cand.wav")
    assert r["status"] == "skipped" and "not found" in r["reason"]


def test_visqol_parse_mos():
    assert perceptual.parse_mos("MOS-LQO: 4.823") == 4.823
    assert perceptual.parse_mos("MOS_LQO = 3.5") == 3.5
    assert abs(perceptual.parse_mos("...\nfinal score 4.21 done") - 4.21) < 1e-9
    assert perceptual.parse_mos("no score here") is None


def test_visqol_with_stub_binary(monkeypatch, tmp_path):
    """A stub 'visqol' that prints a MOS line exercises the real subprocess + parse path
    without installing ViSQOL — proving the env-path adapter works end to end."""
    stub = tmp_path / "visqol_stub.sh"
    stub.write_text("#!/bin/sh\necho 'MOS-LQO: 4.42'\n")
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(perceptual.VISQOL_ENV, str(stub))
    r = perceptual.run_visqol(_wav(tmp_path / "ref.wav"), _wav(tmp_path / "cand.wav"))
    assert r["status"] == "ok" and r["mos_lqo"] == 4.42


def test_peaq_and_aquatk_skip_when_env_unset(monkeypatch):
    """PEAQ and AQUA-Tk skip independently when their own env-paths are unset — proving
    per-tool opt-in: setting one env-path does not require the others."""
    for env in (perceptual.PEAQ_ENV, perceptual.AQUATK_ENV):
        monkeypatch.delenv(env, raising=False)
    for run in (perceptual.run_peaq, perceptual.run_aquatk):
        r = run("ref.wav", "cand.wav")
        assert r["status"] == "skipped"
        assert r["odg"] is None
        assert "not set" in r["reason"]


def test_parse_odg():
    assert perceptual.parse_odg("Objective Difference Grade: -0.293") == -0.293
    assert perceptual.parse_odg("ODG = -3.5") == -3.5
    assert perceptual.parse_odg("Objective Difference Grade:  0.00") == 0.0
    assert perceptual.parse_odg("no grade here") is None


def test_peaq_with_stub_binary(monkeypatch, tmp_path):
    """A stub 'peaq' that prints an ODG line exercises the real subprocess + parse path
    without installing a GPL PEAQ — proving the env-path adapter works end to end."""
    stub = tmp_path / "peaq_stub.sh"
    stub.write_text("#!/bin/sh\necho 'Objective Difference Grade: -1.23'\n")
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(perceptual.PEAQ_ENV, str(stub))
    r = perceptual.run_peaq("ref.wav", "cand.wav")
    assert r["status"] == "ok" and r["odg"] == -1.23


def test_aquatk_with_stub_binary(monkeypatch, tmp_path):
    stub = tmp_path / "aquatk_stub.sh"
    stub.write_text("#!/bin/sh\necho 'ODG: -0.51'\n")
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(perceptual.AQUATK_ENV, str(stub))
    r = perceptual.run_aquatk("ref.wav", "cand.wav")
    assert r["status"] == "ok" and r["odg"] == -0.51


def test_evaluate_lists_all_three_and_each_skips_independently(monkeypatch):
    """`evaluate()` consults every full-reference model; each skips on its own env-path."""
    for env in (perceptual.VISQOL_ENV, perceptual.PEAQ_ENV, perceptual.AQUATK_ENV):
        monkeypatch.delenv(env, raising=False)
    results = perceptual.evaluate("ref.wav", "cand.wav")
    assert [r["tool"] for r in results] == ["visqol", "peaq", "aquatk"]
    assert all(r["status"] == "skipped" for r in results)


def test_export_writes_provenance_sidecars_and_perceptual(tmp_path, monkeypatch):
    for env in (perceptual.VISQOL_ENV, perceptual.PEAQ_ENV, perceptual.AQUATK_ENV):
        monkeypatch.delenv(env, raising=False)  # ensure perceptual skips
    monkeypatch.delenv("PULP_AUBIO_BIN", raising=False)  # ensure MIR oracle skips
    out = str(tmp_path / "run")
    report = pipeline.run_and_export("smear", out)

    cand_sidecar = os.path.join(out, "candidate.wav.provenance.json")
    assert os.path.exists(cand_sidecar)
    block = json.load(open(cand_sidecar))
    # round-trips the recipe and carries a content hash for the sample
    assert block["recipe"]["degradation"] == "smear"
    assert len(block["sample"]["content_sha256"]) == 64

    # all three perceptual models present and gracefully skipped (none installed)
    assert [r["tool"] for r in report["perceptual"]] == ["visqol", "peaq", "aquatk"]
    assert all(r["status"] == "skipped" for r in report["perceptual"])
    # MIR oracle block present and gracefully skipped (no aubio installed)
    assert report["advisory"]["mir_oracles"][0]["status"] == "skipped"


def test_content_hash_matches_file(tmp_path):
    p = tmp_path / "x.bin"
    p.write_bytes(b"hello world")
    import hashlib
    assert provenance.content_hash(str(p)) == hashlib.sha256(b"hello world").hexdigest()


# --- ViSQOL input contract + failure honesty -------------------------------------
#
# ViSQOL's own WAV reader (`src/wav_reader.cc`) accepts 16-bit PCM only, and its `main.cc`
# exits 0 even when a comparison fails — so "the binary ran" and "the binary scored" are
# different claims, and the adapter has to keep them apart.

_VISQOL_CONSOLE = (
    "ViSQOL conformance version: 310\n"
    "Audio mode, support vector regression model\n"
    "MOS-LQO:\t\t4.234\n"
)


def _stub(tmp_path, name, body):
    p = tmp_path / name
    p.write_text("#!/bin/sh\n" + body)
    p.chmod(p.stat().st_mode | stat.S_IXUSR)
    return str(p)


def _wav(path, sr=48000, subtype="FLOAT", seconds=0.2, amp=0.5):
    import numpy as np
    import soundfile as sf
    t = np.arange(int(sr * seconds)) / sr
    sf.write(str(path), (amp * np.sin(2 * np.pi * 440 * t)).astype("float32"), sr, subtype=subtype)
    return str(path)


def test_visqol_parses_the_real_console_format(monkeypatch, tmp_path):
    """The literal stdout ViSQOL emits — banner line first, then a tab-separated score."""
    monkeypatch.setenv(perceptual.VISQOL_ENV,
                       _stub(tmp_path, "v.sh", f"cat <<'EOF'\n{_VISQOL_CONSOLE}EOF\n"))
    r = perceptual.run_visqol(_wav(tmp_path / "a.wav"), _wav(tmp_path / "b.wav"))
    assert r["status"] == "ok" and r["mos_lqo"] == 4.234


def test_visqol_integral_score_still_parses():
    """Default ostream precision prints an exactly-integral MOS as `4`, not `4.0`."""
    assert perceptual.parse_mos("MOS-LQO:\t\t4") == 4.0


def test_failed_run_is_not_reported_as_a_score(monkeypatch, tmp_path):
    """A run that produced NO score must never surface as `ok`.

    ViSQOL exits 0 on a failed comparison and writes the reason to stderr, so a failure
    looks exactly like a success to the exit code. Scanning stderr for a bare float turns
    the incidental number in an error message into a confident, fabricated MOS — which is
    strictly worse than reporting nothing.
    """
    binary = _stub(tmp_path, "fail.sh",
                   "echo 'ERROR: Expected 16bit samples.' >&2\n"
                   "echo 'elapsed 3.14 s' >&2\n"
                   "exit 0\n")
    monkeypatch.setenv(perceptual.VISQOL_ENV, binary)
    r = perceptual.run_visqol(_wav(tmp_path / "a.wav"), _wav(tmp_path / "b.wav"))
    assert r["status"] == "error", f"a scoreless run reported {r}"
    assert r["mos_lqo"] is None
    assert "3.14" not in str(r.get("mos_lqo"))
    assert "16bit" in r["reason"]  # the actionable cause is carried, not swallowed


def test_version_banner_is_not_mistaken_for_a_score(monkeypatch, tmp_path):
    """`ViSQOL 3.3.3` on stdout is a version, not a 3.3 MOS."""
    monkeypatch.setenv(perceptual.VISQOL_ENV,
                       _stub(tmp_path, "ver.sh", "echo 'ViSQOL version 3.3.3 built ok'\n"))
    r = perceptual.run_visqol(_wav(tmp_path / "a.wav"), _wav(tmp_path / "b.wav"))
    assert r["status"] == "error" and r["mos_lqo"] is None


def test_float32_input_is_transcoded_to_pcm16(monkeypatch, tmp_path):
    """The lab writes float32 WAVs; ViSQOL reads 16-bit PCM only. The adapter must
    transcode, or every real run dies in ViSQOL's WAV header parser."""
    import soundfile as sf
    got = tmp_path / "got"
    got.mkdir()
    # The adapter transcodes into a temp dir it deletes on exit, so the stub has to
    # capture the bytes while it is still running — inspecting the paths afterwards
    # measures nothing.
    binary = _stub(tmp_path, "see.sh",
                   f'cp "$2" {got}/ref.wav; cp "$4" {got}/deg.wav\n'
                   + f"cat <<'EOF'\n{_VISQOL_CONSOLE}EOF\n")
    monkeypatch.setenv(perceptual.VISQOL_ENV, binary)
    r = perceptual.run_visqol(_wav(tmp_path / "a.wav", subtype="FLOAT"),
                              _wav(tmp_path / "b.wav", subtype="FLOAT"))
    assert r["status"] == "ok"
    for name in ("ref.wav", "deg.wav"):
        info = sf.info(str(got / name))
        assert info.subtype == "PCM_16", f"ViSQOL was handed {info.subtype}"
        assert info.samplerate == 48000


def test_pcm16_input_is_passed_through_untouched(monkeypatch, tmp_path):
    """Already-conformant input is not needlessly rewritten."""
    seen = tmp_path / "seen.txt"
    binary = _stub(tmp_path, "see2.sh",
                   f'echo "$2" > {seen}\n' + f"cat <<'EOF'\n{_VISQOL_CONSOLE}EOF\n")
    monkeypatch.setenv(perceptual.VISQOL_ENV, binary)
    ref = _wav(tmp_path / "a.wav", subtype="PCM_16")
    assert perceptual.run_visqol(ref, _wav(tmp_path / "b.wav", subtype="PCM_16"))["status"] == "ok"
    assert seen.read_text().strip() == ref


def test_non_48k_is_refused_rather_than_mis_scored(monkeypatch, tmp_path):
    """ViSQOL's audio mode is defined at 48 kHz. Handing it 44.1 kHz produces a number
    that looks fine and means nothing, so the adapter refuses with the reason."""
    monkeypatch.setenv(perceptual.VISQOL_ENV,
                       _stub(tmp_path, "v2.sh", f"cat <<'EOF'\n{_VISQOL_CONSOLE}EOF\n"))
    r = perceptual.run_visqol(_wav(tmp_path / "a.wav", sr=44100, subtype="PCM_16"),
                              _wav(tmp_path / "b.wav", subtype="PCM_16"))
    assert r["status"] == "error" and "48000 Hz" in r["reason"]


def test_real_visqol_binary_discriminates_when_installed(tmp_path):
    """Run the REAL ViSQOL when the developer has one installed.

    This is the only test here that exercises ViSQOL's own DSP; every other test proves
    the adapter around a stub. It asserts a *discrimination*, not a threshold: an
    identical pair must score strictly above a grossly degraded pair. A constant-returning
    adapter passes every stub test above and fails this one.

    With no binary installed it SKIPS — and a skip is not a pass, which is why the reason
    says so out loud.
    """
    import numpy as np
    import pytest
    import soundfile as sf

    if not os.environ.get(perceptual.VISQOL_ENV, "").strip():
        pytest.skip("NOT VERIFIED against a real ViSQOL: PULP_VISQOL_BIN is unset, so "
                    "ViSQOL's own scoring is UNTESTED here (the stub tests above cover "
                    "only the adapter). This skip is not a pass.")

    sr, rng = 48000, np.random.default_rng(7)
    t = np.arange(sr * 4) / sr
    x = 0.5 * np.sin(2 * np.pi * 220 * t) + 0.25 * np.sin(2 * np.pi * 441 * t)
    x = (x / (np.max(np.abs(x)) * 1.05)).astype("float32")
    ref = str(tmp_path / "ref.wav"); sf.write(ref, x, sr, subtype="PCM_16")
    same = str(tmp_path / "same.wav"); sf.write(same, x, sr, subtype="PCM_16")
    bad = str(tmp_path / "bad.wav")
    sf.write(bad, np.clip(np.round(x * 4) / 4 + 0.05 * rng.standard_normal(len(x)), -1, 1)
             .astype("float32"), sr, subtype="PCM_16")

    ceiling = perceptual.run_visqol(ref, same)
    floor = perceptual.run_visqol(ref, bad)
    assert ceiling["status"] == "ok", f"real ViSQOL ceiling run failed: {ceiling}"
    assert floor["status"] == "ok", f"real ViSQOL floor run failed: {floor}"
    assert ceiling["mos_lqo"] > floor["mos_lqo"], (
        f"no discrimination: identical={ceiling['mos_lqo']} degraded={floor['mos_lqo']}")


def test_unreadable_input_is_reported_not_silently_handed_over(monkeypatch, tmp_path):
    """A path that is not a readable WAV fails at the adapter with the file named, rather
    than being passed to ViSQOL to fail opaquely inside its WAV header parser."""
    monkeypatch.setenv(perceptual.VISQOL_ENV,
                       _stub(tmp_path, "v3.sh", f"cat <<'EOF'\n{_VISQOL_CONSOLE}EOF\n"))
    r = perceptual.run_visqol(str(tmp_path / "nope.wav"), _wav(tmp_path / "b.wav"))
    assert r["status"] == "error" and "nope.wav" in r["reason"]
