"""MkDocs hooks for Pulp (#577 PR 2).

Two responsibilities:

1.  `on_pre_build` — run the existing drift-check scripts that the custom
    generator relied on:
      - tools/docs_generate.py check  (refuses drift in auto-generated
                                       blocks of capabilities.md)
      - tools/check-docs-consistency.py  (cross-checks support-matrix.yaml
                                          against capabilities.md)
    Any failure aborts the build with a non-zero exit, mirroring the
    existing `tools/check-docs.sh` behavior.

2.  `on_files` — flatten the output URL of every page listed in
    `docs/status/docs-index.yaml` to `{slug}.html` at the site root, so
    the Material build emits the same URLs the custom generator produced
    (`/pulp/modules.html`, not `/pulp/reference/modules.html`). This
    preserves external deep-links when the switchover lands in PR 3.

    Pages not listed in docs-index.yaml keep their nested MkDocs default
    URL — the custom generator didn't publish them either, so no external
    contract exists.

Registered in `mkdocs.yml` via:
    hooks:
      - tools/mkdocs_hooks.py
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[1]
_DOCS_INDEX = _REPO_ROOT / "docs" / "status" / "docs-index.yaml"
_DOCS_GENERATE = _REPO_ROOT / "tools" / "docs_generate.py"
_CONSISTENCY = _REPO_ROOT / "tools" / "check-docs-consistency.py"


def _load_slug_map() -> dict[str, str]:
    """Parse docs-index.yaml into {relative_md_path: slug}.

    Uses the same minimal regex parse the custom generator uses — no PyYAML
    dependency. Format is a list under a top-level `docs:` key with per-entry
    `slug:` and `path:` fields.
    """
    if not _DOCS_INDEX.exists():
        return {}
    mapping: dict[str, str] = {}
    current_slug: str | None = None
    for raw in _DOCS_INDEX.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip()
        if not line.strip() or line.strip().startswith("#"):
            continue
        m = re.match(r"\s+-\s+slug:\s+(.+)", line)
        if m:
            current_slug = m.group(1).strip()
            continue
        m = re.match(r"\s+path:\s+(.+)", line)
        if m and current_slug:
            path = m.group(1).strip()
            mapping[path] = current_slug
            current_slug = None
    return mapping


def _run_check(label: str, cmd: list[str]) -> None:
    """Run a drift-check script, stream its output, raise on non-zero."""
    sys.stdout.write(f"[mkdocs-hooks] {label}...\n")
    sys.stdout.flush()
    result = subprocess.run(cmd, cwd=_REPO_ROOT)
    if result.returncode != 0:
        raise SystemExit(
            f"[mkdocs-hooks] {label} failed (exit {result.returncode}). "
            f"Fix the drift or revert the change and retry `mkdocs build`."
        )


def on_pre_build(config):  # noqa: ANN001 — mkdocs hook signature is untyped
    """Run docs_generate.py check + check-docs-consistency.py before build."""
    if _DOCS_GENERATE.exists():
        _run_check(
            "docs_generate.py check",
            [sys.executable, str(_DOCS_GENERATE), "check"],
        )
    if _CONSISTENCY.exists():
        _run_check(
            "check-docs-consistency.py",
            [sys.executable, str(_CONSISTENCY)],
        )


def on_files(files, config):  # noqa: ANN001 — mkdocs hook signature is untyped
    """Rewrite File.url/dest_uri for every page listed in docs-index.yaml.

    Produces `/pulp/{slug}.html` URLs matching the legacy custom generator.
    """
    slug_map = _load_slug_map()
    if not slug_map:
        return files

    for f in files:
        if not f.is_documentation_page():
            continue
        src = f.src_uri  # POSIX-style path relative to docs_dir, e.g. "reference/modules.md"
        slug = slug_map.get(src)
        if slug is None:
            continue
        flat = f"{slug}.html"
        f.dest_uri = flat
        f.url = flat
        # abs_dest_path must mirror dest_uri for the writer.
        f.abs_dest_path = str(Path(config["site_dir"]) / flat)
    return files
