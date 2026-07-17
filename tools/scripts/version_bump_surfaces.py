#!/usr/bin/env python3
"""Version-bump gate — Surface cluster.

Surface/version-file domain model + config loading + version-file I/O.
`version_bump_check.py` imports and re-exports every public symbol here,
so external importers and the CLI entrypoint are unaffected.

Holds:
    - LEVELS, the bump-rank tuple shared across the pipeline
    - VersionFile / Surface / Config / Verdict dataclasses
    - load_config()
    - version-file readers/writers (read_version / write_version /
      _json_walk_get / _json_walk_set / _extract_version_from_text /
      version_at_base / already_bumped)
"""

from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

# Shared gate helpers. `_strip_meta` is the version-bump-specific alias
# for `strip_meta`; keep the public alias so callers in this file and any
# external imports don't break.
from gate_common import (
    strip_meta as _strip_meta,
)


# Patchable-helper indirection. See the matching comment in
# `version_bump_heuristics.py`; `already_bumped` resolves
# `version_at_base` through the `version_bump_check` entrypoint so
# entrypoint-level patches keep taking effect. Deferred import avoids the
# entrypoint↔cluster import cycle.
def _vbc():
    try:
        import version_bump_check as _v
        return _v
    except ImportError:  # pragma: no cover - isolated-module fallback
        return None


def _h(name: str):
    v = _vbc()
    if v is not None and hasattr(v, name):
        return getattr(v, name)
    return globals()[name]


# ── Types ───────────────────────────────────────────────────────────────

# Ordered least → most impactful.
LEVELS = ("none", "patch", "minor", "major")


@dataclass
class VersionFile:
    path: str
    kind: str
    field: str | None = None
    pattern: str | None = None


@dataclass
class Surface:
    name: str
    label: str
    version_files: list[VersionFile]
    trigger_paths: list[str]
    public_api_paths: list[str] = field(default_factory=list)
    internal_only_paths: list[str] = field(default_factory=list)
    changelog: str | None = None


@dataclass
class Config:
    surfaces: list[Surface]
    generated_globs: list[str]
    trailer_version_bump: str


@dataclass
class Verdict:
    surface: Surface
    heuristic: str       # "none"|"patch"|"minor"|"major"
    trailer_override: str | None  # set / skip / None
    current_version: str | None
    final_level: str     # what level is actually needed after overrides


# ── Config loading ──────────────────────────────────────────────────────


def load_config(path: Path) -> Config:
    data = _strip_meta(json.loads(path.read_text()))
    surfaces: list[Surface] = []
    for name, entry in (data.get("surfaces") or {}).items():
        vfs = [VersionFile(**_strip_meta(vf) if isinstance(vf, dict) else vf)
               for vf in entry.get("version_files", [])]
        surfaces.append(Surface(
            name=name,
            label=entry.get("label", name),
            version_files=vfs,
            trigger_paths=entry.get("trigger_paths", []),
            public_api_paths=entry.get("public_api_paths", []),
            internal_only_paths=entry.get("internal_only_paths", []),
            changelog=entry.get("changelog"),
        ))
    trailers = data.get("trailers") or {}
    return Config(
        surfaces=surfaces,
        generated_globs=data.get("generated_globs", []) or [],
        trailer_version_bump=trailers.get("version_bump", "Version-Bump"),
    )


# ── Version-file I/O ────────────────────────────────────────────────────

_CMAKE_PROJECT_VERSION_RE = re.compile(
    r"(project\s*\(\s*[A-Za-z_][\w]*\s+VERSION\s+)(\d+\.\d+\.\d+)",
    re.MULTILINE,
)


def read_version(repo: Path, vf: VersionFile) -> str | None:
    p = repo / vf.path
    if not p.exists():
        return None
    text = p.read_text()
    if vf.kind == "cmake_project_version":
        m = _CMAKE_PROJECT_VERSION_RE.search(text)
        return m.group(2) if m else None
    if vf.kind == "json_field":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return None
        return data.get(vf.field)
    if vf.kind == "json_path":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return None
        return _json_walk_get(data, vf.field or "")
    if vf.kind == "pyproject_version":
        m = re.search(r'^\s*version\s*=\s*"([^"]+)"', text, re.MULTILINE)
        return m.group(1) if m else None
    if vf.kind == "python_dunder_version":
        m = re.search(r'^\s*__version__\s*=\s*"([^"]+)"', text, re.MULTILINE)
        return m.group(1) if m else None
    if vf.kind == "regex" and vf.pattern:
        m = re.search(vf.pattern, text)
        return m.group(1) if m else None
    return None


def _json_walk_get(data, dotted_path: str):
    # Walk `plugins.0.version`-style paths. Numeric segments index into
    # arrays; string segments index into objects. Returns None on any
    # miss rather than raising, so the gate stays advisory-style on
    # shape drift.
    if not dotted_path:
        return None
    cur = data
    for seg in dotted_path.split("."):
        if seg == "":
            return None
        if isinstance(cur, list):
            try:
                cur = cur[int(seg)]
            except (ValueError, IndexError):
                return None
        elif isinstance(cur, dict):
            if seg not in cur:
                return None
            cur = cur[seg]
        else:
            return None
    return cur


def _json_walk_set(data, dotted_path: str, value) -> bool:
    # Mirror of _json_walk_get for write_version. Returns True iff the
    # walk reached a settable leaf.
    if not dotted_path:
        return False
    parts = dotted_path.split(".")
    if any(p == "" for p in parts):
        return False
    cur = data
    for seg in parts[:-1]:
        if isinstance(cur, list):
            try:
                cur = cur[int(seg)]
            except (ValueError, IndexError):
                return False
        elif isinstance(cur, dict):
            if seg not in cur:
                return False
            cur = cur[seg]
        else:
            return False
    last = parts[-1]
    if isinstance(cur, list):
        try:
            cur[int(last)] = value
        except (ValueError, IndexError):
            return False
        return True
    if isinstance(cur, dict):
        cur[last] = value
        return True
    return False


def write_version(repo: Path, vf: VersionFile, new: str) -> bool:
    p = repo / vf.path
    if not p.exists():
        return False
    text = p.read_text()
    new_text = text
    if vf.kind == "cmake_project_version":
        new_text = _CMAKE_PROJECT_VERSION_RE.sub(
            lambda m: f"{m.group(1)}{new}", text, count=1
        )
    elif vf.kind == "json_field":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return False
        data[vf.field] = new
        # Preserve trailing newline if original had one.
        end = "\n" if text.endswith("\n") else ""
        new_text = json.dumps(data, indent=2) + end
    elif vf.kind == "json_path":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return False
        if not _json_walk_set(data, vf.field or "", new):
            return False
        end = "\n" if text.endswith("\n") else ""
        new_text = json.dumps(data, indent=2) + end
    elif vf.kind == "pyproject_version":
        new_text = re.sub(
            r'(^\s*version\s*=\s*")([^"]+)(")',
            lambda m: f"{m.group(1)}{new}{m.group(3)}", text,
            count=1, flags=re.MULTILINE,
        )
    elif vf.kind == "python_dunder_version":
        new_text = re.sub(
            r'(^\s*__version__\s*=\s*")([^"]+)(")',
            lambda m: f"{m.group(1)}{new}{m.group(3)}", text,
            count=1, flags=re.MULTILINE,
        )
    elif vf.kind == "regex" and vf.pattern:
        # A capture-group-1 version string is required.
        new_text = re.sub(vf.pattern, lambda m: m.group(0).replace(m.group(1), new), text, count=1)
    else:
        return False
    if new_text == text:
        return False
    p.write_text(new_text)
    return True


def _extract_version_from_text(text: str, vf: VersionFile) -> str | None:
    if vf.kind == "cmake_project_version":
        m = _CMAKE_PROJECT_VERSION_RE.search(text)
        return m.group(2) if m else None
    if vf.kind == "json_field":
        try:
            return json.loads(text).get(vf.field)
        except json.JSONDecodeError:
            return None
    if vf.kind == "json_path":
        try:
            return _json_walk_get(json.loads(text), vf.field or "")
        except json.JSONDecodeError:
            return None
    if vf.kind == "pyproject_version":
        m = re.search(r'^\s*version\s*=\s*"([^"]+)"', text, re.MULTILINE)
        return m.group(1) if m else None
    if vf.kind == "python_dunder_version":
        m = re.search(r'^\s*__version__\s*=\s*"([^"]+)"', text, re.MULTILINE)
        return m.group(1) if m else None
    return None


def version_at_base(base: str, vf: VersionFile) -> str | None:
    """Read the version from a file as it existed at `base`, or None."""
    try:
        base_text = subprocess.run(
            ["git", "show", f"{base}:{vf.path}"],
            check=True, capture_output=True, text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return None
    return _extract_version_from_text(base_text, vf)


def _version_order_key(version: str) -> tuple[int, int, int] | None:
    """(major, minor, patch) for ordering, or None when not semver-shaped."""
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", version)
    if not m:
        return None
    major, minor, patch = m.groups()
    return (int(major), int(minor), int(patch))


def already_bumped(base: str, vf: VersionFile, repo: Path) -> bool:
    """True iff the version file's version at HEAD is AHEAD of the one at base.

    Ordinal, deliberately not `base_ver != head_ver`. `version_at_base` reads
    the TIP of `base` (usually `origin/main`), so on a busy main a branch is
    BEHIND on every surface someone else bumped since it forked. Inequality
    read "behind" as "bumped", which made `--mode=apply` skip the write and
    still render `✓ bumped` — the gate reporting success while doing nothing,
    then the fix/feat CI gate failing downstream with no marker to explain it.
    Ordering also stops a stale branch from merging a version REGRESSION
    (head 0.2.0 under base 0.3.0 is not a bump).

    Unparseable versions fall back to inequality — the pre-ordinal behavior —
    since a non-semver string has no order to compare.
    """
    p = repo / vf.path
    if not p.exists():
        return False
    base_ver = _h("version_at_base")(base, vf)
    if base_ver is None:
        return False
    head_ver = _extract_version_from_text((repo / vf.path).read_text(), vf)
    if head_ver is None:
        return False
    base_key = _version_order_key(base_ver)
    head_key = _version_order_key(head_ver)
    if base_key is None or head_key is None:
        return base_ver != head_ver
    return head_key > base_key
