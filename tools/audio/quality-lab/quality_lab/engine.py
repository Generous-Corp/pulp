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


_STRETCHCLI_REL = os.path.join("build", "examples", "offline-stretch", "stretchcli")


def _repo_root() -> str:
    # quality_lab/engine.py -> quality-lab -> audio -> tools -> <repo root>
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "..", ".."))


def _find_upwards(start: str, rel: str) -> str | None:
    """Walk up from `start` looking for the relative path `rel`."""
    cur = os.path.abspath(start)
    while True:
        cand = os.path.join(cur, rel)
        if os.path.exists(cand):
            return cand
        parent = os.path.dirname(cur)
        if parent == cur:  # reached the filesystem root
            return None
        cur = parent


def resolve() -> str | None:
    """Locate stretchcli, in priority order:

    1. the ``PULP_STRETCHCLI`` env-path (explicit override),
    2. a ``build/.../stretchcli`` found by walking up from the current directory
       — this is what makes the engine path work when the lab is *pip-installed*
       as a ``pulp tool`` (its ``__file__`` lives in a managed venv's
       site-packages, not the source tree) but the user runs ``engine`` from a
       Pulp checkout where they built stretchcli,
    3. the package-relative repo build dir (plain source-tree checkout).
    """
    env = os.environ.get(STRETCHCLI_ENV, "").strip()
    if env and os.path.exists(env):
        return env
    cwd_hit = _find_upwards(os.getcwd(), _STRETCHCLI_REL)
    if cwd_hit:
        return cwd_hit
    cand = os.path.join(_repo_root(), _STRETCHCLI_REL)
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
                "reason": ("stretchcli not found — build it from a Pulp checkout "
                           "(cmake --build build --target stretchcli) or set "
                           f"{STRETCHCLI_ENV}=/path/to/stretchcli")}
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
