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
    probe     evaluate every row carrying a `probe` against observed landing
              state: `--landing-json FILE` (a saved `shipyard landing --json`
              report; what ctest uses) or `--live` (shells out to
              `shipyard landing --json`; manual and advisory, never a gate,
              because it needs network, auth, and Shipyard >= 0.208.0, the
              first release with `shipyard landing`).
              exit 0 = every probe confirmed; 1 = a probe failed; 2 = bad
              contract or unreadable observation. A missing path or an
              UNKNOWN verdict FAILS: nothing unobserved counts as confirmed.

Why exit 0 for surface: hooks are defense-in-depth / context only (Codex
PreToolUse cannot hard-block). The authoritative boundary is the CLI `validate`
gate wired into CI + the pre-push gates, plus server-side required checks — not
this advisory surface. See `.agents/contract.toml` [meta].enforcement_boundary.

Pure stdlib. `tomllib` ships with Python 3.11+, and macOS still ships
/usr/bin/python3 as 3.9 — so the documented `python3 tools/scripts/...` form in
CLAUDE.md / AGENTS.md can land on an interpreter that cannot parse TOML. Rather
than making every caller remember the prerequisite, this script finds a capable
interpreter itself and re-executes under it. A capable interpreter is one that
ACTUALLY IMPORTS `tomllib` when run — never one that merely exists, is
executable, or claims a version number. (A present, executable interpreter that
fails on every invocation is a real failure mode here: an Xcode licence lapse
makes /usr/bin/python3 exit 69 while still passing an `-x` test.) If no capable
interpreter exists the script fails loudly and names the remedy; it never
degrades to a silent no-op.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python < 3.11
    tomllib = None  # type: ignore[assignment]

# ── Capable-interpreter selection ────────────────────────────────────────────
# Kept byte-identical (and in the same order) to the candidate list in
# hooks/scripts/decisions-contract-hint.sh, which solved this first and
# correctly. `test_decisions_contract.py` asserts the two lists have not
# drifted, which is cheaper than inventing a shared file for one list.
#
# The selection rule is the whole point: a candidate qualifies only by RUNNING
# and importing tomllib. Existence, executability (`-x`) and a version string
# are all things a broken interpreter satisfies.
_PYTHON_CANDIDATES = (
    "python3", "python3.14", "python3.13", "python3.12", "python3.11",
    "/opt/homebrew/bin/python3", "/usr/local/bin/python3",
)

# Set in the child so a re-exec can never recurse, however odd the environment.
_REEXEC_SENTINEL = "PULP_DECISIONS_CONTRACT_REEXEC"

_REMEDY = (
    "no TOML-capable Python found. `tomllib` needs Python 3.11+ and this "
    "interpreter ({exe}) does not have it. Remedy: install a newer Python "
    "(`brew install python@3.12`) or run this script with one you already "
    "have, e.g. `python3.12 tools/scripts/decisions_contract.py ...`."
)


def _imports_tomllib(executable: str) -> bool:
    """True only if `executable` actually runs and imports tomllib.

    Deliberately not `os.access(..., os.X_OK)`: an interpreter can be present
    and executable and still fail every invocation (a lapsed Xcode licence
    makes /usr/bin/python3 exit 69). Capability is proven by running it.
    """
    try:
        proc = subprocess.run(
            [executable, "-c",
             "import tomllib; print(tomllib.loads('probe = 1')['probe'])"],
            capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    # Require the parsed value back, not merely exit 0: a stub that exits 0
    # without parsing anything would otherwise qualify as "capable".
    return proc.returncode == 0 and proc.stdout.strip() == "1"


def find_toml_capable_python() -> str | None:
    """First candidate that proves it can parse TOML, else None."""
    seen: set[str] = set()
    for candidate in _PYTHON_CANDIDATES:
        resolved = shutil.which(candidate)
        if not resolved or resolved in seen:
            continue
        seen.add(resolved)
        if _imports_tomllib(resolved):
            return resolved
    # uv keeps managed interpreters outside PATH; same functional test applies.
    uv = shutil.which("uv")
    if uv:
        try:
            found = subprocess.run(
                [uv, "python", "find", "3.12"],
                capture_output=True, text=True, timeout=30,
            )
        except (OSError, subprocess.SubprocessError):
            return None
        resolved = found.stdout.strip()
        if found.returncode == 0 and resolved and _imports_tomllib(resolved):
            return resolved
    return None


def _reexec_under_capable_python(argv: list[str], announce: bool) -> None:
    """Re-exec this script under a TOML-capable interpreter.

    Returns only when that is impossible — the caller then fails loudly.
    """
    if os.environ.get(_REEXEC_SENTINEL):
        return
    replacement = find_toml_capable_python()
    if replacement is None:
        return
    if announce:
        print(
            f"decisions-contract: note: {sys.executable} lacks tomllib; "
            f"re-executing under {replacement}",
            file=sys.stderr,
        )
    os.environ[_REEXEC_SENTINEL] = "1"
    script = str(Path(__file__).resolve())
    try:
        os.execv(replacement, [replacement, script, *argv])
    except OSError:
        # execv failed after all; fall through to the loud error path.
        os.environ.pop(_REEXEC_SENTINEL, None)

# Repo-root-relative default; overridable with --contract for tests.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_CONTRACT = REPO_ROOT / ".agents" / "contract.toml"

VALID_LAYERS = ("default", "pulp")
REQUIRED_DECISION_FIELDS = ("id", "layer", "tags", "title", "why", "do_not", "guards")
PROBE_SOURCES = ("shipyard-landing",)
PROBE_FIELDS = ("source", "path", "equals")
# `shipyard landing` exits 9 when a verdict is UNKNOWN; its JSON is still
# emitted and still evaluated, and an unknown verdict fails its probe.
LANDING_UNKNOWN_EXIT = 9
# `shipyard landing` first ships in this release; older binaries reject the
# subcommand with a usage error (exit 2), which is reported as unavailability
# rather than as a contract failure.
LANDING_MIN_SHIPYARD = "0.208.0"


class SchemaError(Exception):
    """Raised when the contract file is structurally invalid."""


class InterpreterError(SchemaError):
    """No interpreter is available that can actually parse TOML.

    A subclass of SchemaError so existing callers keep their one except-clause
    and exit code 2, but distinct so the message is not labelled as a problem
    with the contract file — which would send a reader to the wrong place.
    """


def load_contract(path: Path) -> dict:
    """Parse + schema-validate the contract. Raises SchemaError on any problem."""
    if tomllib is None:
        raise InterpreterError(_REMEDY.format(exe=sys.executable))
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
        if "probe" in dec:
            _validate_probes(did, dec["probe"])
    return data


def probes_of(dec: dict) -> list[dict]:
    """A row's probes as a list; `probe` may be one table or an array."""
    raw = dec.get("probe")
    if raw is None:
        return []
    return [raw] if isinstance(raw, dict) else list(raw)


def _validate_probes(did: int, raw: object) -> None:
    items = [raw] if isinstance(raw, dict) else raw
    if not isinstance(items, list) or not items:
        raise SchemaError(f"decision {did}: 'probe' must be a table or a non-empty array of tables")
    for probe in items:
        if not isinstance(probe, dict):
            raise SchemaError(f"decision {did}: each probe must be a table")
        extra = sorted(set(probe) - set(PROBE_FIELDS))
        missing = [f for f in PROBE_FIELDS if f not in probe]
        if missing or extra:
            raise SchemaError(f"decision {did}: probe needs exactly {PROBE_FIELDS} "
                              f"(missing {missing}, unexpected {extra})")
        if probe["source"] not in PROBE_SOURCES:
            raise SchemaError(f"decision {did}: probe source must be one of {PROBE_SOURCES}")
        path = probe["path"]
        if (not isinstance(path, str) or not path.startswith(".") or len(path) < 2
                or any(not part for part in path[1:].split("."))):
            raise SchemaError(f"decision {did}: probe path must look like '.a.b.c', got {path!r}")
        if not isinstance(probe["equals"], (str, int, float, bool)):
            raise SchemaError(f"decision {did}: probe 'equals' must be a scalar")


_MISSING = object()


def resolve_path(document: object, path: str) -> object:
    """Walk '.a.b.c' through nested objects; _MISSING when any hop is absent."""
    node = document
    for part in path[1:].split("."):
        if not isinstance(node, dict) or part not in node:
            return _MISSING
        node = node[part]
    return node


def evaluate_probe(probe: dict, observations: dict[str, object]) -> tuple[bool, str]:
    """(passed, detail). Absence and type mismatch are failures, never passes."""
    document = observations.get(probe["source"], _MISSING)
    if document is _MISSING:
        return False, f"no observation for source {probe['source']!r}"
    actual = resolve_path(document, probe["path"])
    if actual is _MISSING:
        return False, f"{probe['path']} is absent from the observation"
    expected = probe["equals"]
    # bool is an int subclass: `True == 1` must not confirm a row claiming 1.
    if type(actual) is not type(expected) or actual != expected:
        return False, f"{probe['path']} = {actual!r}, row claims {expected!r}"
    return True, f"{probe['path']} = {actual!r}"


def probe_rows(data: dict, observations: dict[str, object]) -> list[dict]:
    results = []
    for dec in sorted(data["decision"], key=lambda d: d["id"]):
        for probe in probes_of(dec):
            passed, detail = evaluate_probe(probe, observations)
            results.append({"id": dec["id"], "source": probe["source"],
                            "path": probe["path"], "equals": probe["equals"],
                            "passed": passed, "detail": detail})
    return results


def _live_landing() -> tuple[object | None, str | None]:
    shipyard = shutil.which("shipyard")
    if not shipyard:
        return None, "`shipyard` is not on PATH"
    try:
        proc = subprocess.run([shipyard, "landing", "--json"], capture_output=True,
                              text=True, cwd=REPO_ROOT, timeout=300)
    except (OSError, subprocess.SubprocessError) as exc:
        return None, f"`shipyard landing --json` failed to run: {exc}"
    if proc.returncode not in (0, LANDING_UNKNOWN_EXIT):
        if proc.returncode == 2 and not proc.stdout.strip():
            # clap's usage-error exit: this Shipyard predates the subcommand.
            try:
                version = subprocess.run([shipyard, "--version"], capture_output=True,
                                         text=True, timeout=30).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                version = ""
            return None, (f"shipyard landing unavailable (need >= {LANDING_MIN_SHIPYARD}; "
                          f"found {version or 'unknown version'} at {shipyard})")
        return None, (f"`shipyard landing --json` exited {proc.returncode}: "
                      f"{proc.stderr.strip()[:300]}")
    try:
        return json.loads(proc.stdout), None
    except json.JSONDecodeError as exc:
        return None, f"`shipyard landing --json` emitted unparseable JSON: {exc}"


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
    ap.add_argument("--mode", choices=("validate", "surface", "list", "probe"), default="surface")
    ap.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    ap.add_argument("--paths", nargs="*", default=None,
                    help="Explicit changed paths for --mode surface (skips git).")
    ap.add_argument("--base", default="origin/main",
                    help="Git ref to diff against for --mode surface when --paths is omitted.")
    ap.add_argument("--layer", choices=VALID_LAYERS, default=None,
                    help="Filter --mode list to one layer.")
    ap.add_argument("--landing-json", type=Path, default=None,
                    help="Saved `shipyard landing --json` report for --mode probe.")
    ap.add_argument("--live", action="store_true",
                    help="--mode probe: run `shipyard landing --json` now (manual; never a "
                         f"gate). Needs Shipyard >= {LANDING_MIN_SHIPYARD}.")
    ap.add_argument("--json", action="store_true", help="Machine-readable output.")
    args = ap.parse_args(argv)

    if tomllib is None:
        # This interpreter cannot do the job. Find one that can and hand over,
        # so the bare `python3 tools/scripts/decisions_contract.py ...` form
        # documented in CLAUDE.md / AGENTS.md is true on a stock macOS shell.
        # Stay silent in `surface` mode: that path is contractually a clean
        # no-op for external contributors, and the selftest asserts empty
        # stderr. Failure is loud in every mode (below).
        _reexec_under_capable_python(
            list(argv) if argv is not None else sys.argv[1:],
            announce=args.mode != "surface",
        )

    try:
        data = load_contract(args.contract)
    except SchemaError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            label = ("INTERPRETER ERROR" if isinstance(exc, InterpreterError)
                     else "SCHEMA ERROR")
            print(f"decisions-contract: {label}: {exc}", file=sys.stderr)
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

    if args.mode == "probe":
        return _run_probe(data, args)

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


def _run_probe(data: dict, args: argparse.Namespace) -> int:
    if bool(args.landing_json) == bool(args.live):
        print("decisions-contract: --mode probe needs exactly one of "
              "--landing-json FILE or --live", file=sys.stderr)
        return 2
    if args.live:
        landing, error = _live_landing()
    else:
        try:
            landing, error = json.loads(args.landing_json.read_text()), None
        except (OSError, json.JSONDecodeError) as exc:
            landing, error = None, f"cannot read {args.landing_json}: {exc}"
    if error:
        print(f"decisions-contract: OBSERVATION ERROR: {error}", file=sys.stderr)
        return 2
    results = probe_rows(data, {"shipyard-landing": landing})
    failed = [r for r in results if not r["passed"]]
    if args.json:
        print(json.dumps({"ok": not failed, "probes": results}, indent=2))
    else:
        for r in results:
            mark = "PASS" if r["passed"] else "FAIL"
            print(f"  {mark} #{r['id']} {r['detail']}")
        rows = sorted({r["id"] for r in failed})
        print(f"decisions-contract probe: {len(results) - len(failed)}/{len(results)} "
              f"confirmed" + (f"; rows {rows} disagree with observed state -- "
                              "rewrite the row to present truth or restore the state"
                              if failed else "."))
    if not results:
        print("decisions-contract: no row carries a probe", file=sys.stderr)
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
