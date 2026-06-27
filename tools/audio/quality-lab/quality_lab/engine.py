"""Real Pulp stretch-engine adapter (opt-in) — the strongest credibility upgrade.

Validating detectors against my own `reference_pv` is partly self-referential. This
adapter runs the lab against the ACTUAL product engine (`pulp::signal::OfflineStretch`
via the `stretchcli` dev harness), so the detectors are exercised on the real artifacts
the engine produces — the evidence that actually matters for tuning it.

Like the perceptual adapter, this is reached only across a process boundary and is
optional: `stretchcli` is discovered at the conventional build path or via the
`PULP_STRETCHCLI` env-path. When absent (e.g. in public CI, which doesn't build it),
callers skip with a reason rather than failing.
"""
from __future__ import annotations

import os
import subprocess
from typing import Any

STRETCHCLI_ENV = "PULP_STRETCHCLI"


def _repo_root() -> str:
    # quality_lab/engine.py -> quality-lab -> audio -> tools -> <repo root>
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "..", ".."))


def resolve() -> str | None:
    """Locate stretchcli: explicit env-path first, then the conventional build path."""
    env = os.environ.get(STRETCHCLI_ENV, "").strip()
    if env and os.path.exists(env):
        return env
    cand = os.path.join(_repo_root(), "build", "examples", "offline-stretch", "stretchcli")
    return cand if os.path.exists(cand) else None


def available() -> bool:
    return resolve() is not None


def stretch(
    in_wav: str,
    out_wav: str,
    ratio: float,
    character: str = "clean",
    quality: int | None = None,
    timeout_s: float = 180.0,
) -> dict[str, Any]:
    """Render a real-engine time-stretch of `in_wav` -> `out_wav`. Returns a status dict;
    `skipped` when stretchcli isn't built/available (never raises for absence)."""
    binary = resolve()
    if binary is None:
        return {"engine": "stretchcli", "status": "skipped",
                "reason": f"stretchcli not found (build it or set {STRETCHCLI_ENV})"}
    cmd = [binary, in_wav, out_wav, "--ratio", str(ratio), "--character", character]
    if quality is not None:
        cmd += ["--quality", str(quality)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
        if proc.returncode != 0 or not os.path.exists(out_wav):
            return {"engine": "stretchcli", "status": "error", "cmd": cmd,
                    "reason": (proc.stderr or proc.stdout).strip()[:200], "exit": proc.returncode}
        return {"engine": "stretchcli", "status": "ok", "cmd": cmd,
                "character": character, "ratio": ratio}
    except subprocess.TimeoutExpired:
        return {"engine": "stretchcli", "status": "error", "reason": "timeout", "cmd": cmd}
    except Exception as exc:
        return {"engine": "stretchcli", "status": "error", "reason": str(exc), "cmd": cmd}
