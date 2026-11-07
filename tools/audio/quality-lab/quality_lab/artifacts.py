"""Display-only image artifacts (spectrogram / waveform PNGs) for a lab report.

The lab draws nothing itself. It shells to the Pulp CLI's `pulp audio spectrogram`
and `pulp audio waveform` verbs (the one STFT, the one peak pyramid, the one PNG
encoder) and attaches what came back under the report's `advisory.artifacts`
namespace with `display_only: true`. Nothing here is read by any detector, gate
or verdict; the structural test in `tests/test_artifacts.py` keeps it that way.

Env-path fence, like every other optional tool in the lab: the CLI is taken from
`PULP_BIN` (a path or a name on PATH). When it is unset or not found the block
is recorded as `skipped` with the reason and the report is otherwise unchanged.

Redaction: every path in the attached block is a basename relative to the run's
output directory, and the CLI's sidecar is itself basename-only by default. The
lab never passes `--absolute-paths`.

approach after SimZhou/vscode-audiolens c5c5a24; re-authored
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
from typing import Any

PULP_BIN_ENV = "PULP_BIN"
ARTIFACT_SCHEMA = "pulp.audio.image.v1"


def _resolve(env_var: str = PULP_BIN_ENV) -> tuple[str | None, str]:
    path = os.environ.get(env_var, "").strip()
    if not path:
        return None, f"{env_var} not set (opt-in image artifacts; skipping)"
    resolved = shutil.which(path) or (path if os.path.exists(path) else None)
    if not resolved:
        return None, f"{env_var}={path} not found on disk/PATH; skipping"
    return resolved, ""


def _redact(text: str, *paths: str) -> str:
    """Replace every absolute path the CLI may echo (the input, the output dir and
    each of their parent directories) with its basename, longest first so a parent
    never clips a child. Error text is the one place a path can leak past the
    basename-only sidecar, so it is scrubbed before it is stored."""
    candidates: set[str] = set()
    for path in paths:
        current = os.path.abspath(path)
        while current and current not in ("/", os.path.dirname(current)):
            candidates.add(current)
            current = os.path.dirname(current)
    for candidate in sorted(candidates, key=len, reverse=True):
        text = text.replace(candidate, os.path.basename(candidate) or "/")
    return text


def _render(binary: str, verb: str, wav_path: str, out_dir: str, stem: str,
            extra_args: list[str], timeout_s: float) -> dict[str, Any]:
    png = os.path.join(out_dir, f"{stem}.{verb}.png")
    sidecar = png + ".json"
    cmd = [binary, "audio", verb, wav_path, "--png", png, "--json", sidecar, *extra_args]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", timeout=timeout_s)
    except (OSError, subprocess.TimeoutExpired) as exc:  # pragma: no cover - environment
        return {"kind": verb, "status": "failed",
                "reason": _redact(f"{type(exc).__name__}: {exc}", wav_path, out_dir)}
    if proc.returncode != 0:
        return {
            "kind": verb,
            "status": "failed" if proc.returncode != 2 else "refused",
            "exit_code": proc.returncode,
            "reason": _redact((proc.stderr or proc.stdout).strip(), wav_path, out_dir)[-400:],
        }
    try:
        with open(sidecar, encoding="utf-8") as fh:
            sidecar_doc = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        return {"kind": verb, "status": "failed", "reason": f"sidecar unreadable: {exc}"}
    return {
        "kind": verb,
        "status": "ok",
        "png": os.path.basename(png),
        "sidecar": os.path.basename(sidecar),
        "schema": sidecar_doc.get("schema"),
        "input_sha256": (sidecar_doc.get("input") or {}).get("sha256"),
        "true_peak": sidecar_doc.get("true_peak"),
        "settings": sidecar_doc.get("settings"),
    }


def render(wav_path: str, out_dir: str, *, stem: str | None = None,
           spectrogram: bool = True, waveform: bool = True,
           spectrogram_args: list[str] | None = None,
           timeout_s: float = 120.0) -> dict[str, Any]:
    """Render the requested images for one WAV into `out_dir`. Returns the block to
    attach: `{"status": ok|partial|skipped, "items": [...]}`. `skipped` means the
    CLI fence did not resolve; the report is still complete without it."""
    binary, reason = _resolve()
    base = {"display_only": True, "schema": ARTIFACT_SCHEMA, "source": os.path.basename(wav_path)}
    if binary is None:
        return {**base, "status": "skipped", "reason": reason, "items": []}
    stem = stem or os.path.splitext(os.path.basename(wav_path))[0]
    items: list[dict[str, Any]] = []
    if spectrogram:
        items.append(_render(binary, "spectrogram", wav_path, out_dir, stem,
                             spectrogram_args or [], timeout_s))
    if waveform:
        items.append(_render(binary, "waveform", wav_path, out_dir, stem, [], timeout_s))
    if not items:
        return {**base, "status": "skipped", "reason": "no image kinds requested", "items": []}
    status = "ok" if all(i["status"] == "ok" for i in items) else "partial"
    return {**base, "status": status, "items": items}


def attach(report: dict[str, Any], wavs: dict[str, str], out_dir: str, **render_kwargs: Any) -> dict[str, Any]:
    """Render images for each named WAV (`{"reference": path, "candidate": path}`) and
    append them under `report["advisory"]["artifacts"]`. The verdict, worst regions
    and detector results are never read or written. Returns the same report."""
    advisory = report.setdefault("advisory", {"detectors": [], "reviewers": [], "perceptual": []})
    advisory["artifacts"] = {
        "display_only": True,
        "images": {name: render(path, out_dir, stem=name, **render_kwargs) for name, path in wavs.items()},
    }
    return report
