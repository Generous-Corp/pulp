#!/usr/bin/env python3
"""Managed SDK Consumer Sweep — build every downstream Pulp consumer against one
installed Pulp SDK and report each project's measured min-OS floor.

This is the turnkey runner behind the manual sweep. It answers two questions in
one pass over the consumer registry:

  1. Does the new SDK still build every example/demo? (build ok / fail per repo)
  2. Does the SDK's min-OS floor actually PROPAGATE into each consumer's shipped
     binaries? (measured floor per repo vs the SDK's own floor)

Question 2 is the reason this exists: a consumer that inherits the build host's
OS floor instead of Pulp's is a silent portability regression. The sweep links
each repo against the installed SDK, then measures the FINAL artifact with
`measure_min_os.py --measure` (the honest "derive the floor from everything
linked" primitive) and flags any repo whose floor != the SDK floor.

The registry of WHAT to sweep is the private planning submodule's
`planning/sdk-consumers/consumers.yaml`; the public per-repo build knobs live in
`tools/scripts/sdk_consumer_sweep_recipes.yaml`.

Typical use (macOS, an already-unpacked installed SDK):

    tools/scripts/sdk_consumer_sweep.py \
        --sdk-prefix /path/to/pulp-sdk \
        --only pulp-example-plugins,pulp-gpu-nam

    tools/scripts/sdk_consumer_sweep.py --sdk-prefix ... --dry-run   # plan only
    tools/scripts/sdk_consumer_sweep.py --sdk-prefix ... --json report.json
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONSUMERS = REPO / "planning" / "sdk-consumers" / "consumers.yaml"
DEFAULT_RECIPES = REPO / "tools" / "scripts" / "sdk_consumer_sweep_recipes.yaml"
MEASURE = REPO / "tools" / "scripts" / "measure_min_os.py"

# Intermediate/vendored build subtrees whose binaries are not the consumer's
# shipped artifact — skip them when discovering what to measure.
_SKIP_DIRS = {"CMakeFiles", "_deps", ".git", "Testing"}


def short_name(repo: str) -> str:
    """'danielraffel/pulp-gpu-nam' -> 'pulp-gpu-nam'."""
    return repo.split("/", 1)[-1]


def load_yaml(path: Path) -> dict:
    # Imported lazily so the module (and its pure-helper unit tests) load on a
    # host without PyYAML; only an actual sweep run — which parses the registry
    # and recipes — needs it.
    try:
        import yaml
    except ModuleNotFoundError:
        raise SystemExit(
            "PyYAML is required to parse the consumer registry/recipes.\n"
            "Install it with:  python3 -m pip install pyyaml")
    return yaml.safe_load(path.read_text()) or {}


@dataclass
class Plan:
    """One repo's resolved build plan."""
    repo: str                       # owner/name
    name: str                       # short name
    skip_reason: str | None = None
    configure_flags: list[str] = field(default_factory=list)
    build_target: str | None = None


def build_plans(consumers: dict, recipes: dict, only: list[str] | None) -> list[Plan]:
    """Resolve the per-repo build plan from the registry + recipe overrides.

    A repo is skipped when: its recipe carries an explicit `skip`, or its
    registry `status.state` is 'not-applicable' (a README/PKG-only mirror with no
    buildable source). `only` (short or full names) filters the set.
    """
    recipe_repos = (recipes or {}).get("repos", {}) or {}
    default_flags = ((recipes or {}).get("defaults", {}) or {}).get("configure_flags", []) or []

    only_set = None
    if only:
        only_set = {short_name(o) for o in only}

    plans: list[Plan] = []
    for entry in consumers.get("repos", []) or []:
        repo = entry.get("repo", "")
        name = short_name(repo)
        if only_set is not None and name not in only_set:
            continue

        rec = recipe_repos.get(name, {}) or {}
        skip = rec.get("skip")
        if not skip:
            state = (entry.get("status", {}) or {}).get("state")
            if state == "not-applicable":
                skip = ((entry.get("status", {}) or {}).get("next_action")
                        or "registry marks this repo not-applicable (no buildable source).")

        plans.append(Plan(
            repo=repo,
            name=name,
            skip_reason=skip,
            configure_flags=list(rec.get("configure_flags", default_flags)),
            build_target=rec.get("build_target"),
        ))
    return plans


def _ver_eq(a: str | None, b: str | None) -> bool:
    """Compare two version strings by numeric components, not textually. A
    measured floor of "13.3.0" and a declared floor of "13.3" are the SAME floor;
    a raw string compare would call that DRIFT. Shorter forms are zero-padded so
    (13,3) == (13,3,0). Unparseable values fall back to a string compare."""
    if a is None or b is None:
        return a == b
    try:
        ta = tuple(int(x) for x in a.split("."))
        tb = tuple(int(x) for x in b.split("."))
    except ValueError:
        return a == b
    n = max(len(ta), len(tb))
    return ta + (0,) * (n - len(ta)) == tb + (0,) * (n - len(tb))


def measure_artifact(path: Path) -> tuple[str | None, str | None]:
    """Delegate to measure_min_os.measure_artifact without importing at module
    load (keeps this script importable even if that module moves)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("measure_min_os", MEASURE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.measure_artifact(path)


def discover_artifacts(build_dir: Path) -> list[Path]:
    """Final linked binaries under a build tree: Mach-O / ELF / PE files that are
    the consumer's shipped output, not intermediate static archives (.a) or
    vendored dependency builds. Returns a sorted, de-duplicated list."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("measure_min_os", MEASURE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    found: set[Path] = set()
    for root, dirs, files in os.walk(build_dir):
        dirs[:] = [d for d in dirs if d not in _SKIP_DIRS]
        rp = Path(root)
        for fn in files:
            p = rp / fn
            if p.is_symlink():
                continue
            kind = mod._artifact_kind(p)
            # 'ar' is an intermediate static lib, not a shipped artifact.
            if kind in ("macho", "elf", "pe"):
                found.add(p)
    return sorted(found)


def repo_floor(artifacts: list[Path]) -> tuple[str | None, list[dict]]:
    """Measure every artifact and return (max_floor, per_artifact_details).

    The repo floor is the MAX over all shipped binaries — the highest OS a user
    would need for any one of them to load."""
    details: list[dict] = []
    floors: list[tuple[tuple[int, ...], str]] = []
    for a in artifacts:
        kind, floor = measure_artifact(a)
        details.append({"path": str(a), "kind": kind, "floor": floor})
        if floor:
            try:
                floors.append((tuple(int(x) for x in floor.split(".")), floor))
            except ValueError:
                pass
    if not floors:
        return None, details
    return max(floors)[1], details


def sdk_floor(sdk_prefix: Path) -> str | None:
    """The installed SDK's own declared macOS floor, read from the min_os.json it
    ships next to its CMake package (lib/cmake/Pulp/min_os.json). Falls back to
    measuring a shipped static lib if the json is absent."""
    j = sdk_prefix / "lib" / "cmake" / "Pulp" / "min_os.json"
    if j.exists():
        try:
            doc = json.loads(j.read_text())
            plat = "macos-arm64" if sys.platform == "darwin" else \
                   "linux-x64" if sys.platform.startswith("linux") else "windows-x64"
            node = (doc.get("platforms", {}) or {}).get(plat, {}) or {}
            floor = node.get("floor")
            if floor and floor != "null":
                return floor
            deps = node.get("deps", {}) or {}
            vals = [v.get("measured") for v in deps.values()
                    if isinstance(v, dict) and v.get("measured") not in (None, "null")]
            if vals:
                return max(vals, key=lambda s: tuple(int(x) for x in s.split(".")))
        except (json.JSONDecodeError, ValueError):
            pass
    # Fallback: measure any shipped lib.
    for lib in sorted((sdk_prefix / "lib").glob("libpulp-*.a")):
        _, floor = measure_artifact(lib)
        if floor:
            return floor
    return None


def _run(cmd: list[str], cwd: Path | None = None, log: Path | None = None) -> tuple[int, str]:
    """Run a command, capturing combined output (also teed to `log` if given)."""
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    if log:
        log.write_text(out)
    return proc.returncode, out


def clone(repo: str, dest: Path, log: Path) -> tuple[bool, str]:
    """Clone via SSH (agent git is SSH-routed; never rewrite to HTTPS)."""
    if dest.exists():
        # Already present (e.g. a pre-cloned checkout in --workdir) — reuse it.
        return True, "reused existing checkout"
    url = f"git@github.com:{repo}.git"
    rc, out = _run(["git", "clone", "--depth", "1", url, str(dest)], log=log)
    return rc == 0, out.strip().splitlines()[-1] if out.strip() else "clone failed"


def build_one(plan: Plan, sdk_prefix: Path, checkout: Path, jobs: int,
              logdir: Path) -> dict:
    """Configure + build one consumer against the installed SDK, then measure the
    floor of every artifact it produced. Returns a result record."""
    result: dict = {
        "repo": plan.repo, "name": plan.name,
        "clone": None, "configure": None, "build": None,
        "floor": None, "artifacts": [], "notes": [],
    }
    build_dir = checkout / "build-sweep"

    clone_log = logdir / f"{plan.name}.clone.log"
    ok, msg = clone(plan.repo, checkout, clone_log)
    result["clone"] = ok
    if not ok:
        result["notes"].append(f"clone: {msg}")
        return result

    configure = [
        "cmake", "-S", str(checkout), "-B", str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_PREFIX_PATH={sdk_prefix}",
        *plan.configure_flags,
    ]
    rc, _ = _run(configure, log=logdir / f"{plan.name}.configure.log")
    result["configure"] = rc == 0
    if rc != 0:
        result["notes"].append(f"configure failed (see {plan.name}.configure.log)")
        return result

    build = ["cmake", "--build", str(build_dir), "-j", str(jobs)]
    if plan.build_target:
        build += ["--target", plan.build_target]
    rc, _ = _run(build, log=logdir / f"{plan.name}.build.log")
    result["build"] = rc == 0
    if rc != 0:
        result["notes"].append(f"build failed (see {plan.name}.build.log)")
        return result

    floor, details = repo_floor(discover_artifacts(build_dir))
    result["floor"] = floor
    result["artifacts"] = details
    if not details:
        result["notes"].append("no measurable artifacts found under build-sweep")
    return result


def bounded_jobs(requested: int | None) -> int:
    """Explicit, bounded job count (never a bare --parallel). Defaults to
    min(cores, 8) to stay friendly on a shared machine."""
    if requested and requested > 0:
        return requested
    return max(1, min(os.cpu_count() or 4, 8))


def format_report(results: list[dict], skipped: list[Plan], sdk: str | None) -> str:
    lines = []
    lines.append(f"SDK floor (this host): {sdk or 'unknown'}")
    lines.append("")
    hdr = f"{'repo':32} {'build':6} {'floor':8} {'vs SDK':8} notes"
    lines.append(hdr)
    lines.append("-" * len(hdr))
    for r in results:
        if r["clone"] is False:
            state = "clone✗"
        elif r["configure"] is False:
            state = "cfg✗"
        elif r["build"] is False:
            state = "build✗"
        elif r["build"]:
            state = "ok"
        else:
            state = "?"
        floor = r["floor"] or "-"
        if r["floor"] and sdk:
            vs = "match" if _ver_eq(r["floor"], sdk) else "DRIFT"
        else:
            vs = "-"
        note = "; ".join(r["notes"]) if r["notes"] else ""
        lines.append(f"{r['name']:32} {state:6} {floor:8} {vs:8} {note}")
    for p in skipped:
        lines.append(f"{p.name:32} {'skip':6} {'-':8} {'-':8} {p.skip_reason or ''}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk-prefix", type=Path, required=True,
                    help="path to an unpacked installed Pulp SDK (contains "
                         "lib/cmake/Pulp). Consumers link against it via "
                         "CMAKE_PREFIX_PATH.")
    ap.add_argument("--consumers", type=Path, default=DEFAULT_CONSUMERS,
                    help="consumer registry (default: planning/sdk-consumers/consumers.yaml)")
    ap.add_argument("--recipes", type=Path, default=DEFAULT_RECIPES,
                    help="per-repo build recipes (default: tools/scripts/sdk_consumer_sweep_recipes.yaml)")
    ap.add_argument("--only", help="comma-separated repo names to sweep (short or owner/name)")
    ap.add_argument("--workdir", type=Path,
                    help="where to clone/build (default: a temp dir; reuses an "
                         "existing checkout of the same name if present)")
    ap.add_argument("--jobs", type=int, help="build parallelism (default: min(cores, 8))")
    ap.add_argument("--dry-run", action="store_true", help="print the plan; do not clone/build")
    ap.add_argument("--json", type=Path, metavar="PATH", help="write the machine report")
    ap.add_argument("--keep", action="store_true", help="keep the workdir after running")
    args = ap.parse_args(argv)

    if not args.consumers.exists():
        print(f"consumers registry not found: {args.consumers}\n"
              "(initialize the planning submodule: git submodule update --init planning)",
              file=sys.stderr)
        return 2
    consumers = load_yaml(args.consumers)
    recipes = load_yaml(args.recipes) if args.recipes.exists() else {}

    only = args.only.split(",") if args.only else None
    plans = build_plans(consumers, recipes, only)
    to_build = [p for p in plans if not p.skip_reason]
    skipped = [p for p in plans if p.skip_reason]

    if args.dry_run:
        print("Planned sweep:")
        for p in to_build:
            flags = " ".join(p.configure_flags)
            print(f"  build  {p.name:32} {flags}")
        for p in skipped:
            print(f"  skip   {p.name:32} {p.skip_reason}")
        return 0

    if not args.sdk_prefix.exists():
        print(f"--sdk-prefix does not exist: {args.sdk_prefix}", file=sys.stderr)
        return 2
    sdk = sdk_floor(args.sdk_prefix)

    workdir = args.workdir or Path(
        __import__("tempfile").mkdtemp(prefix="pulp-sdk-sweep-"))
    workdir.mkdir(parents=True, exist_ok=True)
    logdir = workdir / "logs"
    logdir.mkdir(exist_ok=True)
    jobs = bounded_jobs(args.jobs)

    print(f"Sweeping {len(to_build)} consumer(s) against SDK floor {sdk or '?'} "
          f"(-j{jobs}) in {workdir}")
    results = []
    for p in to_build:
        print(f"  → {p.name} …", flush=True)
        results.append(build_one(p, args.sdk_prefix, workdir / p.name, jobs, logdir))

    report = format_report(results, skipped, sdk)
    print("\n" + report)

    if args.json:
        args.json.write_text(json.dumps(
            {"sdk_floor": sdk, "results": results,
             "skipped": [{"name": p.name, "reason": p.skip_reason} for p in skipped]},
            indent=2))
        print(f"\nWrote {args.json}")

    if not args.keep and not args.workdir:
        shutil.rmtree(workdir, ignore_errors=True)

    # Propagation (question #2) can only be checked against a known SDK floor. If
    # sdk_floor() came back empty, the "vs SDK" column is all "-" and a raw pass
    # would be a silent false success — the whole reason the tool exists went
    # unverified. Fail loudly instead.
    if sdk is None:
        print("\nERROR: could not determine the SDK's own min-OS floor "
              f"(no lib/cmake/Pulp/min_os.json and no measurable lib under "
              f"{args.sdk_prefix}/lib). Floor propagation was NOT verified.",
              file=sys.stderr)
        return 2

    # Exit non-zero if any built repo failed to build or drifted from the SDK floor.
    bad = [r for r in results
           if r["clone"] is False or r["configure"] is False or r["build"] is False
           or (r["floor"] and not _ver_eq(r["floor"], sdk))]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
