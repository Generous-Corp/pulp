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
from tools.harness.visual import spec as visual_spec  # noqa: E402


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
    fixture = visual_spec.fixture_spec_from_file(fixture_path, default_surface=surface)
    return visual_spec.golden_path(repo_root, fixture)


def actual_path_for(
    repo_root: Path,
    actuals_dir: Path,
    fixture: visual_spec.VisualFixtureSpec,
) -> Path:
    base = actuals_dir if actuals_dir.is_absolute() else repo_root / actuals_dir
    return base / fixture.surface / f"{fixture.entry}{fixture.golden_suffix}"


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


def run_capture(binary: Path, fixture_path: Path) -> bytes:
    proc = subprocess.run(
        [str(binary), "--fixture", str(fixture_path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        stderr = proc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(
            f"{binary} failed for {fixture_path} with exit {proc.returncode}\n"
            f"{stderr}"
        )
    return proc.stdout


def run_snapshot(binary: Path, fixture_path: Path) -> dict[str, Any]:
    payload = run_capture(binary, fixture_path)
    return json.loads(payload.decode("utf-8"))


def load_fixture(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def write_bytes(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def _capture_fixture(
    binary: Path,
    fixture: visual_spec.VisualFixtureSpec,
) -> dict[str, Any] | bytes:
    payload = run_capture(binary, fixture.source_path)
    if fixture.capture_format == "json":
        return json.loads(payload.decode("utf-8"))
    if fixture.capture_format == "png":
        return payload
    raise ValueError(f"unsupported visual capture format {fixture.capture_format!r}")


def _write_capture(
    path: Path,
    fixture: visual_spec.VisualFixtureSpec,
    payload: dict[str, Any] | bytes,
) -> None:
    if fixture.capture_format == "json":
        if not isinstance(payload, dict):
            raise TypeError(f"expected JSON object payload for {fixture.id}")
        write_json(path, payload)
        return
    if fixture.capture_format == "png":
        if not isinstance(payload, bytes):
            raise TypeError(f"expected byte payload for {fixture.id}")
        write_bytes(path, payload)
        return
    raise ValueError(f"unsupported visual capture format {fixture.capture_format!r}")


def _write_actual(
    repo_root: Path,
    actuals_dir: Path | None,
    fixture: visual_spec.VisualFixtureSpec,
    payload: dict[str, Any] | bytes,
) -> Path | None:
    if actuals_dir is None:
        return None
    out = actual_path_for(repo_root, actuals_dir, fixture)
    _write_capture(out, fixture, payload)
    return out


def generate(binary: Path, repo_root: Path, surface: str, fixtures: list[Path]) -> int:
    for fixture_path in fixtures:
        fixture = visual_spec.fixture_spec_from_file(fixture_path, default_surface=surface)
        payload = _capture_fixture(binary, fixture)
        out = visual_spec.golden_path(repo_root, fixture)
        _write_capture(out, fixture, payload)
        print(f"generated {out.relative_to(repo_root)}")
    return 0


def verify(
    binary: Path,
    repo_root: Path,
    surface: str,
    fixtures: list[Path],
    *,
    actuals_dir: Path | None = None,
) -> int:
    failures: list[str] = []
    for fixture_path in fixtures:
        fixture = visual_spec.fixture_spec_from_file(fixture_path, default_surface=surface)
        golden = visual_spec.golden_path(repo_root, fixture)
        actual = _capture_fixture(binary, fixture)
        if not golden.exists():
            actual_path = _write_actual(repo_root, actuals_dir, fixture, actual)
            actual_note = f"\nactual written to {actual_path}" if actual_path else ""
            failures.append(
                f"{fixture.id}: missing golden {golden.relative_to(repo_root)}"
                f"{actual_note}"
            )
            continue

        if fixture.capture_format == "json":
            if not isinstance(actual, dict):
                raise TypeError(f"expected JSON object payload for {fixture.id}")
            expected = json.loads(golden.read_text(encoding="utf-8"))
            tolerance = differ.tolerance_from_fixture(load_fixture(fixture_path))
            diffs = differ.compare(expected, actual, tolerance=tolerance)
            if diffs:
                actual_path = _write_actual(repo_root, actuals_dir, fixture, actual)
                actual_note = f"\nactual written to {actual_path}" if actual_path else ""
                failures.append(
                    f"{fixture.id} failed semantic diff:\n"
                    f"{differ.format_differences(diffs)}"
                    f"{actual_note}"
                )
            else:
                print(f"ok {fixture.id}")
        elif fixture.capture_format == "png":
            if not isinstance(actual, bytes):
                raise TypeError(f"expected byte payload for {fixture.id}")
            expected_bytes = golden.read_bytes()
            byte_diff = differ.format_byte_difference(expected_bytes, actual)
            if byte_diff:
                actual_path = _write_actual(repo_root, actuals_dir, fixture, actual)
                actual_note = f"\nactual written to {actual_path}" if actual_path else ""
                failures.append(f"{fixture.id} failed PNG byte diff: {byte_diff}{actual_note}")
            else:
                print(f"ok {fixture.id}")
        else:
            raise ValueError(f"unsupported visual capture format {fixture.capture_format!r}")

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
    fixture_ids = visual_spec.fixture_ids_with_goldens(repo_root, surface_list)
    out: dict[str, dict[str, Any]] = {}
    for surface in surface_list:
        covered = 0
        total = 0
        missing_refs: list[str] = []
        bucket = compat.get(surface, {})
        if not isinstance(bucket, dict):
            bucket = {}
        for entry, payload in bucket.items():
            if not isinstance(payload, dict) or _is_catalog_note(entry):
                continue
            tests = list(payload.get("tests") or [])
            refs = visual_spec.validation_refs(tests)
            if any(ref.excludes_from_visual_denominator for ref in refs):
                continue
            if payload.get("status") != "supported":
                continue

            total += 1
            runtime_refs = [ref for ref in refs if ref.is_runtime_fixture]
            if any(ref.target in fixture_ids for ref in runtime_refs):
                covered += 1
            for ref in runtime_refs:
                if ref.target not in fixture_ids:
                    missing_refs.append(f"{entry}: {ref.raw}")
        out[surface] = {
            "pass": covered,
            "total": total,
            "label": f"{covered}/{total}" if total else f"{covered}/0",
            "missing_refs": missing_refs,
        }
    return out


def validation_route_counts(repo_root: Path, surfaces: Iterable[str] | None = None) -> dict[str, dict[str, Any]]:
    compat_path = repo_root / "compat.json"
    compat = json.loads(compat_path.read_text(encoding="utf-8")) if compat_path.exists() else {}
    surface_list = list(surfaces or sorted(compat))
    fixture_ids = visual_spec.fixture_ids_with_goldens(repo_root, surface_list)
    out: dict[str, dict[str, Any]] = {}
    for surface in surface_list:
        total = 0
        routed = 0
        excluded = 0
        legacy = 0
        missing_refs: list[str] = []
        unvalidated_entries: list[str] = []
        bucket = compat.get(surface, {})
        if not isinstance(bucket, dict):
            bucket = {}
        for entry, payload in bucket.items():
            if not isinstance(payload, dict) or _is_catalog_note(entry):
                continue
            if payload.get("status") != "supported":
                continue

            total += 1
            tests = list(payload.get("tests") or [])
            refs = visual_spec.validation_refs(tests)
            legacy_refs = [
                test
                for test in tests
                if isinstance(test, str) and visual_spec.parse_validation_ref(test) is None
            ]
            if any(ref.excludes_from_visual_denominator for ref in refs):
                excluded += 1
                routed += 1
                continue

            has_route = False
            for ref in refs:
                if ref.is_runtime_fixture:
                    if ref.target in fixture_ids:
                        has_route = True
                    else:
                        missing_refs.append(f"{entry}: {ref.raw}")
                elif ref.kind == "unit":
                    has_route = True
            if legacy_refs:
                legacy += 1
                has_route = True

            if has_route:
                routed += 1
            else:
                unvalidated_entries.append(entry)
        out[surface] = {
            "pass": routed,
            "total": total,
            "label": f"{routed}/{total}" if total else f"{routed}/0",
            "excluded": excluded,
            "legacy": legacy,
            "missing_refs": missing_refs,
            "unvalidated_entries": unvalidated_entries,
        }
    return out


def _is_catalog_note(entry_name: str) -> bool:
    short_name = entry_name.split("/", 1)[-1]
    return short_name.startswith("__")


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="pulp harness visual",
        description="Generate or verify deterministic JSON/PNG visual snapshots.",
    )
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--generate", action="store_true", help="Regenerate checked-in JSON/PNG goldens.")
    mode.add_argument("--verify", action="store_true", help="Verify fixtures against checked-in JSON/PNG goldens.")
    p.add_argument("--surface", action="append", default=None, help="Surface to run, default: yoga.")
    p.add_argument("--entry", action="append", default=None, help="Fixture entry name. May be repeated.")
    p.add_argument(
        "--all",
        action="store_true",
        help=(
            "Run all fixtures for the selected surface. REQUIRED for "
            "--generate (so accidental bulk golden rewrites need an "
            "explicit opt-in); a no-op for --verify (kept for parity "
            "with --generate; emits a deprecation warning when used "
            "with --verify)."
        ),
    )
    p.add_argument("--binary", type=Path, default=None, help="Path to pulp-test-visual.")
    p.add_argument("--build-dir", type=Path, default=None, help="CMake build directory.")
    p.add_argument("--repo-root", type=Path, default=None, help="Override repo root discovery.")
    p.add_argument(
        "--actuals-dir",
        type=Path,
        default=None,
        help="Directory for failed actual JSON/PNG captures, relative to repo root unless absolute.",
    )
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

    # Codex P2 on PR #1598 — `--all` was previously redundant: omitting
    # `--entry` ALSO returned every fixture, so `--generate` could
    # silently rewrite all goldens for a surface without an explicit
    # opt-in. Tighten the contract:
    #
    #   --generate REQUIRES --all OR --entry. Bulk golden rewrites
    #     are now explicit; a forgotten `--entry` no longer triggers
    #     a quiet "regenerate everything" path.
    #
    #   --verify continues to default to all fixtures when --entry is
    #     omitted (matching the existing `pulp harness visual --verify`
    #     CI ergonomic). Passing `--all` to --verify is harmless but
    #     redundant; warn to discourage it.
    if mode_generate and not args.all and not args.entry:
        print(
            "error: --generate requires either --all (regenerate every "
            "fixture for the selected surface) or --entry NAME [--entry NAME ...]"
            " (regenerate the listed fixtures). Refusing to silently "
            "rewrite every golden — Codex P2 on PR #1598.",
            file=sys.stderr,
        )
        return 2
    if mode_verify and args.all:
        print(
            "warning: --all is redundant with --verify (omitting --entry "
            "already runs every fixture); ignored.",
            file=sys.stderr,
        )

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
                rc = verify(binary, repo_root, surface, fixtures, actuals_dir=args.actuals_dir)
            else:
                rc = 2
            if rc != 0:
                return rc
    except (
        FileNotFoundError,
        RuntimeError,
        ValueError,
        UnicodeDecodeError,
        json.JSONDecodeError,
    ) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
