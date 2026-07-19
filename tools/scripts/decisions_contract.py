#!/usr/bin/env python3
"""Agent-neutral read surface for the layered decisions contract.

The decisions contract (`.agents/contract.toml`) records settled build-system /
CI / release-automation decisions, each bought with an incident. This is the ONE
checker both Claude and Codex invoke — policy logic is not forked per agent.
Every mode is noninteractive, offers `--json`, and returns stable exit codes; it
needs no Shipyard, no gh, and no network.

Modes (`--mode`):
    validate  parse + schema-check the contract file.
              exit 0 = valid; exit 2 = malformed / schema violation.
    surface   given changed paths (`--paths …` or `--base <ref>`), print the
              contract rows whose `guards` globs match a changed fleet/CI config
              path. ADVISORY: always exit 0. A change touching no guarded
              config path prints nothing (the external-contributor no-op).
    list      dump rows, optionally filtered by `--layer default|pulp`.

Why exit 0 for surface: hooks are defense-in-depth / context only (Codex
PreToolUse cannot hard-block). The authoritative boundary is the CLI `validate`
gate wired into CI + the pre-push gates, plus server-side required checks — not
this advisory surface. See `.agents/contract.toml` [meta].enforcement_boundary.

Pure stdlib. `tomllib` ships with Python 3.11+.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import subprocess
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python < 3.11
    tomllib = None  # type: ignore[assignment]

# Repo-root-relative default; overridable with --contract for tests.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_CONTRACT = REPO_ROOT / ".agents" / "contract.toml"

VALID_LAYERS = ("default", "pulp")
REQUIRED_DECISION_FIELDS = ("id", "layer", "tags", "title", "why", "do_not", "guards")


class SchemaError(Exception):
    """Raised when the contract file is structurally invalid."""


def load_contract(path: Path) -> dict:
    """Parse + schema-validate the contract. Raises SchemaError on any problem."""
    if tomllib is None:
        raise SchemaError("tomllib unavailable (needs Python 3.11+)")
    if not path.is_file():
        raise SchemaError(f"contract file not found: {path}")
    try:
        with path.open("rb") as fh:
            data = tomllib.load(fh)
    except (tomllib.TOMLDecodeError, OSError) as exc:
        raise SchemaError(f"cannot parse {path}: {exc}") from exc

    schema = data.get("schema")
    if not isinstance(schema, dict):
        raise SchemaError("missing [schema] table")
    if schema.get("version") != 1:
        raise SchemaError(f"unsupported [schema].version: {schema.get('version')!r} (expected 1)")
    if schema.get("kind") != "pulp.decisions-contract":
        raise SchemaError(f"unexpected [schema].kind: {schema.get('kind')!r}")
    config_paths = schema.get("config_paths")
    if not isinstance(config_paths, list) or not config_paths or not all(
        isinstance(p, str) and p for p in config_paths
    ):
        raise SchemaError("[schema].config_paths must be a non-empty list of glob strings")

    decisions = data.get("decision")
    if not isinstance(decisions, list) or not decisions:
        raise SchemaError("contract must define at least one [[decision]]")

    seen_ids: set[int] = set()
    for idx, dec in enumerate(decisions):
        if not isinstance(dec, dict):
            raise SchemaError(f"[[decision]] #{idx} is not a table")
        for field in REQUIRED_DECISION_FIELDS:
            if field not in dec:
                raise SchemaError(f"[[decision]] #{idx} missing required field '{field}'")
        did = dec["id"]
        if not isinstance(did, int):
            raise SchemaError(f"[[decision]] #{idx} 'id' must be an int, got {did!r}")
        if did in seen_ids:
            raise SchemaError(f"duplicate decision id {did}")
        seen_ids.add(did)
        if dec["layer"] not in VALID_LAYERS:
            raise SchemaError(f"decision {did}: layer must be one of {VALID_LAYERS}, got {dec['layer']!r}")
        for field in ("title", "why", "do_not"):
            if not isinstance(dec[field], str) or not dec[field].strip():
                raise SchemaError(f"decision {did}: '{field}' must be a non-empty string")
        for field in ("tags", "guards"):
            val = dec[field]
            if not isinstance(val, list) or not val or not all(
                isinstance(x, str) and x for x in val
            ):
                raise SchemaError(f"decision {did}: '{field}' must be a non-empty list of strings")
        # Every guard must fall within the declared fleet/CI config surface, so
        # the surface mode can never fire on a non-config path (the
        # external-contributor no-op is a structural property, not a hope).
        for guard in dec["guards"]:
            if not any(_glob_within(guard, cp) for cp in config_paths):
                raise SchemaError(
                    f"decision {did}: guard {guard!r} is not within [schema].config_paths "
                    f"(would break the external-contributor no-op)"
                )
    return data


def _glob_within(guard: str, config_path: str) -> bool:
    """True if `guard` is the same as or nested under `config_path`.

    Both are globs. Exact match, or guard sits under a `dir/**` config path, or
    the config path is itself a `**` glob that the guard refines.
    """
    if guard == config_path:
        return True
    if config_path.endswith("/**"):
        prefix = config_path[:-2]  # keep trailing slash
        return guard.startswith(prefix)
    return False


def _changed_paths_from_git(base: str) -> list[str]:
    """Return paths changed vs `base`. Degrades to [] (no-op) if git can't answer."""
    try:
        merge_base = subprocess.run(
            ["git", "merge-base", base, "HEAD"],
            capture_output=True, text=True, cwd=REPO_ROOT, timeout=10,
        )
        ref = merge_base.stdout.strip() if merge_base.returncode == 0 else base
        out = subprocess.run(
            ["git", "diff", "--name-only", f"{ref}...HEAD"],
            capture_output=True, text=True, cwd=REPO_ROOT, timeout=10,
        )
        if out.returncode != 0:
            return []
        return [line.strip() for line in out.stdout.splitlines() if line.strip()]
    except (OSError, subprocess.SubprocessError):
        return []


def _normalize(path: str) -> str:
    """Repo-relative, forward-slash form for glob matching."""
    p = path.strip().replace("\\", "/")
    # Best-effort: strip a leading absolute prefix down to repo-relative.
    try:
        rp = Path(path).resolve()
        rel = rp.relative_to(REPO_ROOT)
        return str(rel).replace("\\", "/")
    except (ValueError, OSError):
        return p.lstrip("./")


def _match_guard(guard: str, changed: str) -> bool:
    if guard.endswith("/**"):
        prefix = guard[:-3]
        return changed == prefix or changed.startswith(prefix + "/")
    return fnmatch.fnmatch(changed, guard)


def surface(data: dict, changed_paths: list[str]) -> list[dict]:
    """Return the decisions whose guards match any changed path, id-ordered."""
    norm = [_normalize(p) for p in changed_paths]
    hits: list[dict] = []
    for dec in data["decision"]:
        matched = sorted(
            {c for c in norm for g in dec["guards"] if _match_guard(g, c)}
        )
        if matched:
            row = dict(dec)
            row["_matched_paths"] = matched
            hits.append(row)
    hits.sort(key=lambda d: d["id"])
    return hits


def _render_row(dec: dict) -> str:
    lines = [
        f"  [{dec['layer']}] #{dec['id']}: {dec['title']}",
        f"      why:        {dec['why']}",
        f"      do NOT:     {dec['do_not']}",
    ]
    if dec.get("_matched_paths"):
        lines.append(f"      triggered by: {', '.join(dec['_matched_paths'])}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=("validate", "surface", "list"), default="surface")
    ap.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    ap.add_argument("--paths", nargs="*", default=None,
                    help="Explicit changed paths for --mode surface (skips git).")
    ap.add_argument("--base", default="origin/main",
                    help="Git ref to diff against for --mode surface when --paths is omitted.")
    ap.add_argument("--layer", choices=VALID_LAYERS, default=None,
                    help="Filter --mode list to one layer.")
    ap.add_argument("--json", action="store_true", help="Machine-readable output.")
    args = ap.parse_args(argv)

    try:
        data = load_contract(args.contract)
    except SchemaError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print(f"decisions-contract: SCHEMA ERROR: {exc}", file=sys.stderr)
        return 2

    if args.mode == "validate":
        n = len(data["decision"])
        if args.json:
            print(json.dumps({"ok": True, "decisions": n,
                              "contract": str(args.contract)}))
        else:
            print(f"decisions-contract: OK — {n} decisions, schema v"
                  f"{data['schema']['version']}", file=sys.stderr)
        return 0

    if args.mode == "list":
        rows = [d for d in data["decision"]
                if args.layer is None or d["layer"] == args.layer]
        if args.json:
            print(json.dumps({"ok": True, "decisions": rows}, indent=2))
        else:
            for d in sorted(rows, key=lambda x: x["id"]):
                print(_render_row(d))
        return 0

    # mode == surface
    changed = args.paths if args.paths is not None else _changed_paths_from_git(args.base)
    hits = surface(data, changed)
    if args.json:
        print(json.dumps({"ok": True, "matched": hits}, indent=2, default=str))
        return 0
    if not hits:
        # Clean no-op: no guarded fleet/CI config path was touched.
        return 0
    print("── decisions contract: settled decisions relevant to this change ──",
          file=sys.stderr)
    print("   (advisory context — the CLI validate gate + CI required checks are "
          "the boundary)", file=sys.stderr)
    for d in hits:
        print(_render_row(d), file=sys.stderr)
    print("   Reversing one requires proving its incident class can no longer "
          "occur (Step Zero).", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
