"""Layer B — perceptual quality models (opt-in, license-fenced).

A full-reference perceptual quality predictor is a *coarse global guard* complementary
to the Layer-A artifact detectors: it won't say "smear at 42 ms", but it flags "this got
perceptually worse overall." It is **advisory, never a gate**.

This layer holds only **full-reference, music/general-audio** metrics — the lab's use
case is "did this candidate get worse than the reference, and where?" over musical
material. Speech-intelligibility/telephony metrics (PESQ, POLQA) and no-reference neural
speech metrics (DNSMOS, NISQA) are deliberately out of scope: they are band-limited or
tuned to speech, and the no-reference ones don't fit the reference-vs-candidate contract
at all. If a vocal/speech-processing use case ever appears, a *separate* no-reference
lane is the place for it — not this full-reference layer.

Supported here, each behind its own env-path:
  - ViSQOL  (`PULP_VISQOL_BIN`, Apache-2.0) — MOS-LQO, audio/music mode
  - PEAQ    (`PULP_PEAQ_BIN`, GPL impls)    — ITU-R BS.1387 ODG (-4..0)
  - AQUA-Tk (`PULP_AQUATK_BIN`, GPL-3.0)    — PEAQ-family ODG

License fence (the plan's §4, enforced here in code, not just docs): each tool is
reached ONLY across a process boundary, ONLY via an explicit env-path the developer
sets. No bundling, no auto-download, no import. When an env-path is unset or its binary
is missing, that entry is `skipped` with a reason — never an error, never a failure, and
each tool skips independently (so the developer picks their subset by which env-paths
they set). Public CI never sets the env-paths, so the whole layer always skips.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from typing import Any

VISQOL_ENV = "PULP_VISQOL_BIN"
# ViSQOL's audio mode is defined at 48 kHz, and its own WAV reader
# (`src/wav_reader.cc`) accepts 16-bit PCM only — anything else is rejected with
# "Error parsing WAV Header - Expected 16bit samples." The lab writes float32 WAVs
# (`audio_io.save_wav`), so a pair straight off the pipeline is unreadable by ViSQOL
# and must be transcoded first.
VISQOL_REQUIRED_SR = 48000
PEAQ_ENV = "PULP_PEAQ_BIN"
AQUATK_ENV = "PULP_AQUATK_BIN"


def _resolve(env_var: str) -> tuple[str | None, str]:
    path = os.environ.get(env_var, "").strip()
    if not path:
        return None, f"{env_var} not set (opt-in perceptual model; skipping)"
    resolved = shutil.which(path) or (path if os.path.exists(path) else None)
    if not resolved:
        return None, f"{env_var}={path} not found on disk/PATH; skipping"
    return resolved, ""


_VERSION_TRIPLE = re.compile(r"\b\d+\.\d+\.\d+\b")


def parse_mos(text: str) -> float | None:
    """Pull a MOS-LQO float from ViSQOL's console output. Robust to format drift.

    ViSQOL prints `MOS-LQO:\t\t<value>` with default ostream precision, so an exactly
    integral score arrives as `4` rather than `4.0` — hence the optional fraction.

    The bare-float fallback exists only for format drift and is deliberately narrow: a
    dotted version triple is stripped first so a banner like "ViSQOL 3.3.3" cannot be
    read as a score. Callers must apply it to stdout only — ViSQOL writes diagnostics to
    stderr, where any incidental float would otherwise become a fabricated MOS.
    """
    m = re.search(r"MOS[-_ ]?LQO\s*[:=]?\s*([0-9]+(?:\.[0-9]+)?)", text, re.IGNORECASE)
    if m:
        return float(m.group(1))
    cleaned = _VERSION_TRIPLE.sub(" ", text)
    m = re.search(r"\b([1-4]\.[0-9]+|5\.0+)\b", cleaned)  # fallback: a plausible MOS
    return float(m.group(1)) if m else None


def prepare_for_visqol(path: str, tmpdir: str) -> tuple[str, str | None]:
    """Make one WAV readable by ViSQOL, returning `(path_to_use, error_or_None)`.

    Transcodes to 16-bit PCM when needed (the lab's own exports are float32) and refuses
    a sample rate ViSQOL's audio mode is not defined at, rather than letting it through
    to be silently mis-scored. When soundfile is unavailable the original path is handed
    through unchanged — the adapter degrades, it never blocks.
    """
    try:
        import soundfile as sf
    except Exception:
        return path, None
    try:
        info = sf.info(path)
    except Exception as exc:
        return path, f"cannot read {os.path.basename(path)}: {exc}"
    if int(info.samplerate) != VISQOL_REQUIRED_SR:
        return path, (f"{os.path.basename(path)} is {info.samplerate} Hz; ViSQOL audio "
                      f"mode requires {VISQOL_REQUIRED_SR} Hz — resample before comparing")
    if info.subtype == "PCM_16":
        return path, None
    out = os.path.join(tmpdir, os.path.basename(path) + ".pcm16.wav")
    data, sr = sf.read(path, always_2d=True)
    sf.write(out, data, sr, subtype="PCM_16")
    return out, None


def run_visqol(reference_wav: str, candidate_wav: str, timeout_s: float = 180.0) -> dict[str, Any]:
    """Run ViSQOL (audio/music mode) on two WAVs via its env-path binary. Returns an
    advisory result dict; status is `skipped` when the tool isn't installed."""
    binary, reason = _resolve(VISQOL_ENV)
    if binary is None:
        return {"tool": "visqol", "status": "skipped", "reason": reason, "mos_lqo": None}
    try:
        with tempfile.TemporaryDirectory(prefix="visqol-in-") as tmpdir:
            ref, ref_err = prepare_for_visqol(reference_wav, tmpdir)
            cand, cand_err = prepare_for_visqol(candidate_wav, tmpdir)
            if ref_err or cand_err:
                return {"tool": "visqol", "status": "error", "mos_lqo": None,
                        "reason": "; ".join(e for e in (ref_err, cand_err) if e)}
            proc = subprocess.run(
                [binary, "--reference_file", ref, "--degraded_file", cand,
                 "--use_speech_mode=false"],
                capture_output=True, text=True, timeout=timeout_s,
            )
        # Parse stdout ONLY. ViSQOL prints the score to stdout and routes every
        # diagnostic to stderr, and it exits 0 even when a comparison fails (`main.cc`
        # logs the error and continues), so neither the exit code nor stderr can be
        # trusted — scanning stderr turns an incidental float in an error message or a
        # file path into a fabricated MOS for a run that produced no score at all.
        mos = parse_mos(proc.stdout)
        if mos is None:
            tail = [ln for ln in (proc.stderr or "").strip().splitlines() if ln][-3:]
            detail = f"; stderr: {' | '.join(tail)}" if tail else ""
            return {"tool": "visqol", "status": "error", "mos_lqo": None,
                    "reason": f"no MOS-LQO on stdout{detail}", "exit": proc.returncode}
        return {"tool": "visqol", "status": "ok", "mos_lqo": round(mos, 3),
                "mode": "audio", "advisory": True}
    except subprocess.TimeoutExpired:
        return {"tool": "visqol", "status": "error", "mos_lqo": None, "reason": "timeout"}
    except Exception as exc:  # never let an opt-in tool break the run
        return {"tool": "visqol", "status": "error", "mos_lqo": None, "reason": str(exc)}


def parse_odg(text: str) -> float | None:
    """Pull a PEAQ Objective Difference Grade (-4..0) from a PEAQ/AQUA-Tk stdout line.
    0 = imperceptible difference, -4 = very annoying. Robust to format drift."""
    m = re.search(r"Objective\s+Difference\s+Grade\s*[:=]?\s*(-?[0-9]+\.[0-9]+)", text, re.IGNORECASE)
    if m:
        return float(m.group(1))
    m = re.search(r"\bODG\s*[:=]?\s*(-?[0-9]+\.[0-9]+)", text, re.IGNORECASE)
    return float(m.group(1)) if m else None


def run_peaq(reference_wav: str, candidate_wav: str, timeout_s: float = 180.0) -> dict[str, Any]:
    """Run a PEAQ (ITU-R BS.1387) implementation on two WAVs via `PULP_PEAQ_BIN`. Emits
    an Objective Difference Grade (ODG, -4..0), not a MOS. Advisory; `skipped` when the
    tool isn't installed. The invocation targets the common GstPEAQ CLI
    (`peaq <ref> <test>`); wrap a different PEAQ front-end in a shim if its args differ."""
    binary, reason = _resolve(PEAQ_ENV)
    if binary is None:
        return {"tool": "peaq", "status": "skipped", "reason": reason, "odg": None}
    try:
        proc = subprocess.run(
            [binary, reference_wav, candidate_wav],
            capture_output=True, text=True, timeout=timeout_s,
        )
        odg = parse_odg(proc.stdout + "\n" + proc.stderr)
        if odg is None:
            return {"tool": "peaq", "status": "error", "odg": None,
                    "reason": "could not parse ODG from output", "exit": proc.returncode}
        return {"tool": "peaq", "status": "ok", "odg": round(odg, 3),
                "metric": "ITU-R BS.1387 ODG", "advisory": True}
    except subprocess.TimeoutExpired:
        return {"tool": "peaq", "status": "error", "odg": None, "reason": "timeout"}
    except Exception as exc:  # never let an opt-in tool break the run
        return {"tool": "peaq", "status": "error", "odg": None, "reason": str(exc)}


def run_aquatk(reference_wav: str, candidate_wav: str, timeout_s: float = 180.0) -> dict[str, Any]:
    """Run AQUA-Tk (a PEAQ port + embedding distances) on two WAVs via `PULP_AQUATK_BIN`.
    Reports the PEAQ-family ODG. Advisory; `skipped` when the tool isn't installed. Point
    the env-path at an executable that prints an ODG line for `<ref> <test>`."""
    binary, reason = _resolve(AQUATK_ENV)
    if binary is None:
        return {"tool": "aquatk", "status": "skipped", "reason": reason, "odg": None}
    try:
        proc = subprocess.run(
            [binary, reference_wav, candidate_wav],
            capture_output=True, text=True, timeout=timeout_s,
        )
        odg = parse_odg(proc.stdout + "\n" + proc.stderr)
        if odg is None:
            return {"tool": "aquatk", "status": "error", "odg": None,
                    "reason": "could not parse ODG from output", "exit": proc.returncode}
        return {"tool": "aquatk", "status": "ok", "odg": round(odg, 3),
                "metric": "PEAQ-family ODG", "advisory": True}
    except subprocess.TimeoutExpired:
        return {"tool": "aquatk", "status": "error", "odg": None, "reason": "timeout"}
    except Exception as exc:  # never let an opt-in tool break the run
        return {"tool": "aquatk", "status": "error", "odg": None, "reason": str(exc)}


def evaluate(reference_wav: str, candidate_wav: str) -> list[dict[str, Any]]:
    """All full-reference perceptual models for a (reference, candidate) WAV pair.
    Advisory; each entry degrades to `skipped` independently when its tool isn't
    installed — so "support all by default" and "just the one I installed" are the same
    behavior, selected purely by which env-paths the developer sets."""
    return [
        run_visqol(reference_wav, candidate_wav),
        run_peaq(reference_wav, candidate_wav),
        run_aquatk(reference_wav, candidate_wav),
    ]
