#!/usr/bin/env python3
"""Version-bump gate.

Given a diff range (base..head), decide whether each configured surface
needs a version bump (patch/minor/major). Three modes:

    report  exit 0 if every surface that moved has a bumped version,
            exit 1 otherwise. Authoritative gate for CI.
    apply   same as report, but for every surface missing a bump, rewrite
            the version file(s) in place and stage them for commit.
            Used by `pulp pr` to make bumps automatic.
    hint    advisory text only; always exits 0. Used by agent hooks.

Heuristics (per surface, deliberately conservative):
    - If only internal_only_paths changed       → patch-suggested
    - If any public_api_paths changed (non-comment/whitespace diff)
                                                → minor-required
    - If a Version-Bump: <surface>=<level>      → that level wins (after
      path filtering — a plugin-only `BREAKING` does not bump the SDK)
    - Conventional-commit subjects (`feat:`, `fix:`, `BREAKING:` or `!:`
      in subjects) may RAISE the heuristic verdict on a surface whose
      trigger_paths were actually touched. Cannot lower it.
    - Revert commits (subject starts with `Revert` or `Revert-Of:` trailer)
      suppress signals from the reverted work.

Uses JSON configs (zero-dep).
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


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


# ── Git helpers ─────────────────────────────────────────────────────────


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        check=True, capture_output=True, text=True,
    )
    return Path(out.stdout.strip())


def git_diff_names(base: str, head: str) -> list[str]:
    out = subprocess.run(
        ["git", "diff", "--name-only", f"{base}..{head}"],
        check=True, capture_output=True, text=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


def git_diff_ignore_whitespace_nonempty(base: str, head: str, path: str) -> bool:
    """True iff there is a meaningful (non-whitespace, non-comment) diff for `path`.

    First tries --ignore-all-space; if that leaves any hunks, checks whether
    every hunk line is a C/C++/Python comment. Useful for collapsing comment
    reflows down to patch-suggested instead of minor-required.
    """
    out = subprocess.run(
        ["git", "diff", "--ignore-all-space", f"{base}..{head}", "--", path],
        check=True, capture_output=True, text=True,
    )
    if not out.stdout.strip():
        return False

    has_real = False
    for line in out.stdout.splitlines():
        if not (line.startswith("+") or line.startswith("-")):
            continue
        if line.startswith(("+++", "---")):
            continue
        body = line[1:].strip()
        if not body:
            continue
        if body.startswith(("//", "#", "/*", "*/", "*")):
            continue
        has_real = True
        break
    return has_real


def git_log_subjects_and_bodies(base: str, head: str) -> list[tuple[str, str, str]]:
    """Return (sha, subject, body) tuples for each commit in base..head."""
    out = subprocess.run(
        ["git", "log", "--format=%H%x00%s%x00%B%x01", f"{base}..{head}"],
        check=True, capture_output=True, text=True,
    )
    commits: list[tuple[str, str, str]] = []
    for chunk in out.stdout.split("\x01"):
        chunk = chunk.strip()
        if not chunk:
            continue
        parts = chunk.split("\x00", 2)
        if len(parts) < 3:
            continue
        sha, subject, body = parts
        commits.append((sha, subject, body))
    return commits


def git_commit_files(sha: str) -> list[str]:
    """Files touched by a single commit. Used to scope conv-commit signals
    to surfaces whose trigger_paths the commit actually modified."""
    out = subprocess.run(
        ["git", "show", "--name-only", "--format=", sha],
        check=True, capture_output=True, text=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


def git_range_trailers(base: str, head: str) -> dict[str, list[str]]:
    """Collect trailers from every commit in base..head (CI checks out
    a synthetic merge commit as HEAD, so a bypass on the branch tip
    wouldn't be visible if we only looked at HEAD)."""
    try:
        body = subprocess.run(
            ["git", "log", "--format=%B%x00", f"{base}..{head}"],
            check=True, capture_output=True, text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return {}
    result: dict[str, list[str]] = {}
    for body_chunk in body.split("\x00"):
        if not body_chunk.strip():
            continue
        trailers = subprocess.run(
            ["git", "interpret-trailers", "--parse"],
            input=body_chunk, capture_output=True, text=True,
        )
        for line in trailers.stdout.splitlines():
            if ":" not in line:
                continue
            key, _, value = line.partition(":")
            result.setdefault(key.strip().lower(), []).append(value.strip())
    return result


def git_commit_trailers(ref: str) -> dict[str, list[str]]:
    try:
        body = subprocess.run(
            ["git", "log", "-1", "--format=%B", ref],
            check=True, capture_output=True, text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return {}
    trailers = subprocess.run(
        ["git", "interpret-trailers", "--parse"],
        input=body, capture_output=True, text=True,
    )
    result: dict[str, list[str]] = {}
    for line in trailers.stdout.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        result.setdefault(key.strip().lower(), []).append(value.strip())
    return result


# ── Config loading ──────────────────────────────────────────────────────


def _strip_meta(data: dict) -> dict:
    return {k: v for k, v in data.items() if not k.startswith("_") and k != "$schema"}


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


# ── Matching / heuristics ───────────────────────────────────────────────


def _glob_match(path: str, pattern: str) -> bool:
    if pattern.startswith("**/"):
        tail = pattern[3:]
        return fnmatch.fnmatchcase(path, tail) or fnmatch.fnmatchcase(path, pattern)
    return fnmatch.fnmatchcase(path, pattern)


def _matches_any(path: str, patterns: Iterable[str]) -> bool:
    p = path.replace(os.sep, "/")
    for pat in patterns:
        if _glob_match(p, pat):
            return True
    return False


def filter_generated(changed: list[str], globs: Iterable[str]) -> list[str]:
    gs = list(globs)
    return [f for f in changed if not _matches_any(f, gs)]


def is_revert_commit(subject: str, trailers: dict[str, list[str]]) -> bool:
    if subject.lower().startswith("revert"):
        return True
    if "revert-of" in trailers:
        return True
    return False


def classify_conventional(subject: str) -> str:
    """Map a commit subject to a bump level it would *suggest*.

    `BREAKING:` anywhere in subject or `!:` after the type → major.
    `feat:` → minor. `fix:` or `perf:` → patch. Otherwise none.
    """
    s = subject.strip()
    if re.search(r"\bBREAKING\b", s):
        return "major"
    # `type!:` or `type(scope)!:` signals breaking change.
    if re.match(r"^[a-zA-Z]+(\([^)]*\))?!:", s):
        return "major"
    m = re.match(r"^([a-zA-Z]+)(\([^)]*\))?:", s)
    if not m:
        return "none"
    kind = m.group(1).lower()
    if kind == "feat":
        return "minor"
    if kind in ("fix", "perf"):
        return "patch"
    return "none"


def max_level(*levels: str) -> str:
    best = "none"
    for lv in levels:
        if LEVELS.index(lv) > LEVELS.index(best):
            best = lv
    return best


def heuristic_for_surface(
    surface: Surface,
    changed: list[str],
    base: str,
    head: str,
) -> str:
    touched = [p for p in changed if _matches_any(p, surface.trigger_paths)]
    if not touched:
        return "none"

    # Filter to touched paths with a meaningful (non-whitespace, non-comment-only)
    # diff. Paths whose changes are entirely whitespace or comments don't
    # trigger a version bump — fall through to "none".
    meaningful = [p for p in touched if git_diff_ignore_whitespace_nonempty(base, head, p)]
    if not meaningful:
        return "none"

    # Any public-API path with a meaningful diff → minor-required.
    if any(_matches_any(p, surface.public_api_paths) for p in meaningful):
        return "minor"

    # Otherwise internal-only changes → patch-suggested (advisory; report mode
    # treats this as a warning, never hard fail).
    return "patch"


def surface_trailer_override(
    trailers: dict[str, list[str]],
    trailer_key: str,
    surface_name: str,
) -> str | None:
    """Parse `Version-Bump: <surface>=<level> reason="..."` from trailers."""
    for v in trailers.get(trailer_key.lower(), []):
        m = re.search(rf"{re.escape(surface_name)}\s*=\s*([A-Za-z]+)", v)
        if not m:
            continue
        level = m.group(1).lower()
        if level in LEVELS or level == "skip":
            return level
    return None


# ── Version bumping arithmetic ──────────────────────────────────────────


def bump_version(current: str, level: str) -> str:
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", current)
    if not m:
        return current
    major, minor, patch = map(int, m.groups())
    if level == "major":
        return f"{major + 1}.0.0"
    if level == "minor":
        return f"{major}.{minor + 1}.0"
    if level == "patch":
        return f"{major}.{minor}.{patch + 1}"
    return current


# ── Reporting / apply ───────────────────────────────────────────────────


def _extract_version_from_text(text: str, vf: VersionFile) -> str | None:
    if vf.kind == "cmake_project_version":
        m = _CMAKE_PROJECT_VERSION_RE.search(text)
        return m.group(2) if m else None
    if vf.kind == "json_field":
        try:
            return json.loads(text).get(vf.field)
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


def already_bumped(base: str, vf: VersionFile, repo: Path) -> bool:
    """True iff the version file's version at HEAD differs from at base."""
    p = repo / vf.path
    if not p.exists():
        return False
    base_ver = version_at_base(base, vf)
    if base_ver is None:
        return False
    head_ver = _extract_version_from_text((repo / vf.path).read_text(), vf)
    if head_ver is None:
        return False
    return base_ver != head_ver


def assess_surfaces(
    cfg: Config,
    changed: list[str],
    base: str,
    head: str,
    repo: Path,
) -> list[Verdict]:
    trailers = git_range_trailers(base, head)
    verdicts: list[Verdict] = []
    for s in cfg.surfaces:
        heur = heuristic_for_surface(s, changed, base, head)

        override = surface_trailer_override(trailers, cfg.trailer_version_bump, s.name)
        final = heur
        skip_requested = (override == "skip")

        if skip_requested:
            final = "none"
        elif override in LEVELS:
            # Only raise, never lower. And only if the surface was actually touched.
            if heur != "none" and LEVELS.index(override) > LEVELS.index(heur):
                final = override
            elif heur != "none":
                final = heur
            else:
                # Surface not touched; ignore the override to avoid
                # rubber-stamping unrelated bumps.
                final = "none"

        # Promote via conventional-commit subjects on commits that touched
        # THIS surface — never from commits that only touched unrelated
        # paths. A plugin-only `feat:` cannot raise the SDK ceiling. An
        # explicit `Version-Bump: <surface>=skip` on the tip commit is
        # authoritative and is NOT raised back up by conv-commit subjects.
        if heur != "none" and not skip_requested:
            conv_ceiling = "none"
            for sha, subject, body in git_log_subjects_and_bodies(base, head):
                if is_revert_commit(subject, {}):
                    continue
                # Scope to commits whose files intersect this surface's
                # trigger_paths — otherwise a feat: on the plugin can raise
                # the SDK.
                files = git_commit_files(sha)
                if not any(_matches_any(f, s.trigger_paths) for f in files):
                    continue
                conv_ceiling = max_level(conv_ceiling, classify_conventional(subject))
            if LEVELS.index(conv_ceiling) > LEVELS.index(final):
                final = conv_ceiling

        # patch-suggested is advisory; never hard-fails.
        # (Callers map this to a warning in report mode.)

        current = None
        for vf in s.version_files:
            current = read_version(repo, vf)
            if current:
                break

        verdicts.append(Verdict(
            surface=s,
            heuristic=heur,
            trailer_override=override,
            current_version=current,
            final_level=final,
        ))
    return verdicts


def render_report(
    verdicts: list[Verdict],
    mode: str,
    base: str,
    repo: Path,
) -> tuple[str, int]:
    lines: list[str] = []
    failures = 0
    warnings = 0
    for v in verdicts:
        if v.final_level == "none":
            lines.append(f"[{v.surface.name}] {v.surface.label}: no bump needed")
            continue
        # Every version file in the surface must have moved, not just one.
        # Otherwise surfaces with multiple files (plugin.json + marketplace.json)
        # could pass with only one bumped, causing split-brain versions.
        per_file = [(vf, already_bumped(base, vf, repo)) for vf in v.surface.version_files]
        all_bumped = all(bumped for _, bumped in per_file)
        any_bumped = any(bumped for _, bumped in per_file)

        if all_bumped:
            tag = "✓ bumped"
        elif any_bumped:
            unbumped = [vf.path for vf, b in per_file if not b]
            tag = f"✗ partial bump — not moved: {', '.join(unbumped)}"
        elif v.final_level == "patch":
            # Advisory only — not a hard fail.
            tag = "? bump suggested (patch)"
        else:
            tag = "✗ bump required"
        lines.append(
            f"[{v.surface.name}] {v.surface.label}: "
            f"heuristic={v.heuristic}"
            f"{' override=' + v.trailer_override if v.trailer_override else ''} "
            f"final={v.final_level} "
            f"current={v.current_version or '?'} "
            f"{tag}"
        )
        if not all_bumped:
            # Partial-bump is always a hard fail — split-brain versions are
            # never acceptable. Patch-suggested stays advisory only when
            # nothing has been bumped at all.
            if any_bumped:
                failures += 1
            elif v.final_level == "patch":
                warnings += 1
            else:
                failures += 1

    if mode == "hint":
        return "\n".join(lines), 0

    if failures:
        lines.append("")
        lines.append("Version-bump check FAILED.")
        lines.append("Apply the required bump with:")
        lines.append("  python3 tools/scripts/version_bump_check.py --mode=apply")
        lines.append("Or record an explicit override on the tip commit:")
        lines.append('  Version-Bump: <surface>=<patch|minor|major|skip> reason="..."')
        return "\n".join(lines), 1

    return "\n".join(lines), 0


def apply_bumps(
    verdicts: list[Verdict],
    base: str,
    repo: Path,
) -> list[str]:
    """Write new versions for surfaces that need a bump and aren't already bumped."""
    edited: list[str] = []
    for v in verdicts:
        if v.final_level in ("none", "patch"):
            continue
        # Skip if ALL version files are already at the target; otherwise
        # apply to every file (keeps plugin.json and marketplace.json in
        # lockstep after a partial-bump from a prior run).
        all_bumped = all(already_bumped(base, vf, repo) for vf in v.surface.version_files)
        if all_bumped or not v.current_version:
            continue
        # Compute the target from the BASE version, not v.current_version.
        # v.current_version reflects the first readable file at HEAD, which
        # may already have been bumped by a prior partial run — bumping it
        # again would double-bump (e.g. 0.1.0 -> 0.2.0 -> 0.3.0). Reading
        # the base keeps a partial-apply idempotent.
        base_ver: str | None = None
        for vf in v.surface.version_files:
            base_ver = version_at_base(base, vf)
            if base_ver:
                break
        source_ver = base_ver or v.current_version
        new_ver = bump_version(source_ver, v.final_level)
        for vf in v.surface.version_files:
            if write_version(repo, vf, new_ver):
                edited.append(vf.path)
        # Changelog stub.
        if v.surface.changelog:
            cl_path = repo / v.surface.changelog
            if cl_path.exists():
                header = f"## [{new_ver}]\n\n"
                cl_text = cl_path.read_text()
                pos = cl_text.find("## [")
                if pos != -1:
                    cl_text = cl_text[:pos] + header + cl_text[pos:]
                else:
                    cl_text = header + cl_text
                cl_path.write_text(cl_text)
                edited.append(str(cl_path.relative_to(repo)))
    # Stage for commit so callers see them in `git status`.
    if edited:
        subprocess.run(["git", "-C", str(repo), "add", "--"] + edited, check=False)
    return edited


# ── Main ────────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Version-bump gate")
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--config", default=None)
    parser.add_argument("--mode", choices=("report", "hint", "apply"), default="report")
    parser.add_argument("--repo-root", default=None)
    args = parser.parse_args(argv)

    root = Path(args.repo_root) if args.repo_root else repo_root()
    cfg_path = Path(args.config) if args.config else root / "tools" / "scripts" / "versioning.json"
    if not cfg_path.exists():
        sys.stderr.write(f"version_bump_check: config not found: {cfg_path}\n")
        return 2

    cfg = load_config(cfg_path)

    changed = git_diff_names(args.base, args.head)
    changed = filter_generated(changed, cfg.generated_globs)

    verdicts = assess_surfaces(cfg, changed, args.base, args.head, root)

    if args.mode == "apply":
        edited = apply_bumps(verdicts, args.base, root)
        # Re-assess after editing: re-read current versions and re-check.
        verdicts_after = assess_surfaces(cfg, changed, args.base, args.head, root)
        text, code = render_report(verdicts_after, mode="report", base=args.base, repo=root)
        if edited:
            print("Edited files:")
            for e in edited:
                print(f"  {e}")
        if text:
            print(text)
        return code

    text, code = render_report(verdicts, args.mode, args.base, root)
    if text:
        print(text)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
