#!/usr/bin/env python3
"""Runner for deterministic visual layout snapshots."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable, Optional

HERE = Path(__file__).resolve().parent
REPO_ROOT_GUESS = HERE.parent.parent.parent
if str(REPO_ROOT_GUESS) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT_GUESS))

from tools.harness.visual import differ  # noqa: E402


def find_repo_root(start: Path) -> Path:
    cur = start.resolve()
    for _ in range(10):
        if (cur / "compat.json").exists() and (cur / "CMakeLists.txt").exists():
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    return start.resolve()


def fixtures_dir(repo_root: Path, surface: str) -> Path:
    return repo_root / "tools" / "harness" / "visual" / "fixtures" / surface


def goldens_dir(repo_root: Path, surface: str) -> Path:
    return repo_root / "tools" / "harness" / "visual" / "goldens" / surface


def list_fixtures(repo_root: Path, surface: str) -> list[Path]:
    root = fixtures_dir(repo_root, surface)
    if not root.exists():
        return []
    return sorted(root.glob("*.json"))


def entry_name(path: Path) -> str:
    return path.stem


def resolve_fixtures(repo_root: Path, surface: str, entries: Iterable[str] | None) -> list[Path]:
    fixtures = list_fixtures(repo_root, surface)
    by_name = {entry_name(path): path for path in fixtures}
    by_surface_name = {f"{surface}/{entry_name(path)}": path for path in fixtures}

    selected = list(entries or [])
    if not selected:
        return fixtures

    out: list[Path] = []
    missing: list[str] = []
    for item in selected:
        path = Path(item)
        if path.exists():
            out.append(path)
        elif item in by_name:
            out.append(by_name[item])
        elif item in by_surface_name:
            out.append(by_surface_name[item])
        else:
            missing.append(item)
    if missing:
        known = ", ".join(sorted(by_surface_name))
        raise ValueError(f"unknown visual fixture(s): {', '.join(missing)} (known: {known})")
    return out


def golden_path_for(repo_root: Path, surface: str, fixture_path: Path) -> Path:
    return goldens_dir(repo_root, surface) / f"{entry_name(fixture_path)}.json"


def locate_binary(repo_root: Path, build_dir: Path | None, override: Path | None) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    if override:
        return override

    candidates: list[Path] = []
    if build_dir:
        candidates.extend(
            [
                build_dir / "test" / f"pulp-test-visual{suffix}",
                build_dir / "test" / "Debug" / f"pulp-test-visual{suffix}",
                build_dir / "test" / "Release" / f"pulp-test-visual{suffix}",
                build_dir / f"pulp-test-visual{suffix}",
            ]
        )
    candidates.extend(
        [
            repo_root / "build" / "test" / f"pulp-test-visual{suffix}",
            repo_root / "build-visual" / "test" / f"pulp-test-visual{suffix}",
        ]
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    found = shutil.which(f"pulp-test-visual{suffix}")
    if found:
        return Path(found)
    searched = "\n  ".join(str(p) for p in candidates)
    raise FileNotFoundError(
        "pulp-test-visual binary not found. Build it first with "
        "`cmake --build <build-dir> --target pulp-test-visual`.\n"
        f"Searched:\n  {searched}"
    )


def run_snapshot(binary: Path, fixture_path: Path) -> dict[str, Any]:
    proc = subprocess.run(
        [str(binary), "--fixture", str(fixture_path)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"{binary} failed for {fixture_path} with exit {proc.returncode}\n"
            f"{proc.stderr.strip()}"
        )
    return json.loads(proc.stdout)


def load_fixture(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def generate(binary: Path, repo_root: Path, surface: str, fixtures: list[Path]) -> int:
    for fixture in fixtures:
        payload = run_snapshot(binary, fixture)
        out = golden_path_for(repo_root, surface, fixture)
        write_json(out, payload)
        print(f"generated {out.relative_to(repo_root)}")
    return 0


def verify(binary: Path, repo_root: Path, surface: str, fixtures: list[Path]) -> int:
    failures: list[str] = []
    for fixture in fixtures:
        golden = golden_path_for(repo_root, surface, fixture)
        if not golden.exists():
            failures.append(
                f"{surface}/{entry_name(fixture)}: missing golden {golden.relative_to(repo_root)}"
            )
            continue

        actual = run_snapshot(binary, fixture)
        expected = json.loads(golden.read_text(encoding="utf-8"))
        tolerance = differ.tolerance_from_fixture(load_fixture(fixture))
        diffs = differ.compare(expected, actual, tolerance=tolerance)
        if diffs:
            failures.append(
                f"{surface}/{entry_name(fixture)} failed semantic diff:\n"
                f"{differ.format_differences(diffs)}"
            )
        else:
            print(f"ok {surface}/{entry_name(fixture)}")

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        print(
            "regenerate with: pulp harness visual --generate --surface "
            f"{surface} --all",
            file=sys.stderr,
        )
        return 1
    return 0


def visual_pass_counts(repo_root: Path, surfaces: Iterable[str] | None = None) -> dict[str, dict[str, Any]]:
    compat_path = repo_root / "compat.json"
    compat = json.loads(compat_path.read_text(encoding="utf-8")) if compat_path.exists() else {}
    surface_list = list(surfaces or sorted(compat))
    out: dict[str, dict[str, Any]] = {}
    for surface in surface_list:
        golden_count = len(list(goldens_dir(repo_root, surface).glob("*.json")))
        total = len(compat.get(surface, {}))
        out[surface] = {
            "pass": golden_count,
            "total": total,
            "label": f"{golden_count}/{total}" if total else f"{golden_count}/0",
        }
    return out


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="pulp harness visual",
        description="Generate or verify deterministic semantic visual snapshots.",
    )
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--generate", action="store_true", help="Regenerate checked-in goldens.")
    mode.add_argument("--verify", action="store_true", help="Verify fixtures against checked-in goldens.")
    p.add_argument("--surface", action="append", default=None, help="Surface to run, default: yoga.")
    p.add_argument("--entry", action="append", default=None, help="Fixture entry name. May be repeated.")
    p.add_argument("--all", action="store_true", help="Run all fixtures for the selected surface.")
    p.add_argument("--binary", type=Path, default=None, help="Path to pulp-test-visual.")
    p.add_argument("--build-dir", type=Path, default=None, help="CMake build directory.")
    p.add_argument("--repo-root", type=Path, default=None, help="Override repo root discovery.")
    return p.parse_args(argv)


def main(argv: Optional[list[str]] = None) -> int:
    if argv is None:
        argv = sys.argv[1:]
    if argv and argv[0] == "visual":
        argv = argv[1:]

    args = parse_args(argv)
    repo_root = (args.repo_root or find_repo_root(Path.cwd())).resolve()
    surfaces = args.surface or ["yoga"]
    mode_generate = bool(args.generate)
    mode_verify = bool(args.verify or not args.generate)

    if args.all and args.entry:
        print("error: pass --all OR --entry, not both", file=sys.stderr)
        return 2

    try:
        binary = locate_binary(repo_root, args.build_dir, args.binary)
        for surface in surfaces:
            fixtures = resolve_fixtures(repo_root, surface, args.entry)
            if not fixtures:
                print(f"error: no visual fixtures found for surface {surface!r}", file=sys.stderr)
                return 2
            if mode_generate:
                rc = generate(binary, repo_root, surface, fixtures)
            elif mode_verify:
                rc = verify(binary, repo_root, surface, fixtures)
            else:
                rc = 2
            if rc != 0:
                return rc
    except (FileNotFoundError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
