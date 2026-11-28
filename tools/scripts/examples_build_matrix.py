#!/usr/bin/env python3
"""Build one retained example at a time from a configured build tree.

The manifest (docs/status/examples-manifest.json) lists which examples stay.
This tool reads a configured build directory's CMake file-API codemodel, picks
each retained example's primary targets, builds them one example per
``cmake --build`` call, and writes a receipt per example.

An example that created no target in this configuration is recorded as
``not-configured`` with the reason the manifest gives (its ``gate``), never as
built. A receipt that says ``built`` always names the targets and the exit code.

The build directory needs a file-API query before it is configured:

    mkdir -p <build>/.cmake/api/v1/query && touch <build>/.cmake/api/v1/query/codemodel-v2
    cmake <build>

Check that the committed record covers the manifest:

    python3 tools/scripts/examples_build_matrix.py --check-record

Usage (run under the host build lease so the job count is a share, not the
machine):

    ~/bin/leased-build -- python3 tools/scripts/examples_build_matrix.py \\
        --build-dir <build> --out receipts.json [--only NAME ...] [--plan]
"""

from __future__ import annotations

import argparse
import glob
import json
import subprocess
import sys
import time
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path("docs/status/examples-manifest.json")
RECORD = Path("docs/status/examples-build-record.json")
RETAINED = ("audio-only", "moves-to-vellum")
BUILT_TYPES = ("EXECUTABLE", "MODULE_LIBRARY", "SHARED_LIBRARY", "STATIC_LIBRARY")
FORMAT_PREFERENCE = ("_CLAP", "_Standalone", "_VST3", "_AU")


def example_targets(build_dir: Path) -> dict[str, list[dict]]:
    """Map example directory name to its codemodel targets."""
    reply = build_dir / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(glob.glob(str(reply / "index-*.json")))
    if not indexes:
        raise SystemExit(f"no file-API reply under {reply}; see the module docstring")
    index = json.loads(Path(indexes[-1]).read_text(encoding="utf-8"))
    model_file = next(o["jsonFile"] for o in index["objects"] if o["kind"] == "codemodel")
    model = json.loads((reply / model_file).read_text(encoding="utf-8"))
    source_root = model.get("paths", {}).get("source", "")
    by_dir: dict[str, list[dict]] = {}
    for target in model["configurations"][0]["targets"]:
        detail = json.loads((reply / target["jsonFile"]).read_text(encoding="utf-8"))
        example = example_dir_of(detail.get("paths", {}).get("source", ""), source_root)
        if example:
            by_dir.setdefault(example, []).append(
                {"name": detail["name"], "type": detail["type"]})
    return by_dir


def example_dir_of(source: str, source_root: str) -> str | None:
    """The examples/<name> directory name of a target's source path, if any.

    The file API gives a target's source directory relative to the top source
    directory when it lies inside it and as an absolute path otherwise, so an
    absolute path is first made relative to the codemodel's own source root.
    """
    path = PurePosixPath(source.replace("\\", "/"))
    if path.is_absolute():
        try:
            path = path.relative_to(PurePosixPath(source_root.replace("\\", "/")))
        except ValueError:
            return None
    parts = path.parts
    if len(parts) >= 2 and parts[0] == "examples":
        return parts[1]
    return None


def primary_targets(targets: list[dict]) -> list[str]:
    """One target set that proves the example compiles and links.

    A plug-in builds one target per format; the first format in FORMAT_PREFERENCE
    that exists stands for it. Executables and libraries that are not part of a
    plug-in format are built as they are. Utility targets (build info, shipping
    scans, installs) are left out.
    """
    real = [t for t in targets if t["type"] in BUILT_TYPES]
    names = [t["name"] for t in real]
    chosen: list[str] = []
    for suffix in FORMAT_PREFERENCE:
        hits = [n for n in names if n.endswith(suffix)]
        if hits:
            chosen.extend(hits)
            break
    plugin_cores = {n.rsplit("_", 1)[0] for n in chosen}
    for name in names:
        if name in chosen or name.endswith(("_Core", "_Standalone", "_CLAP", "_VST3", "_AU", "_AUv3", "_AUv3Framework")):
            continue
        if any(name.startswith(core + "_") for core in plugin_cores):
            continue
        chosen.append(name)
    return chosen


def selected_examples(manifest: dict, only: list[str]) -> list[tuple[str, dict]]:
    out = []
    for name, entry in sorted(manifest["examples"].items()):
        if entry["class"] not in RETAINED:
            continue
        if entry["platform"] in ("ios", "android", "web"):
            continue
        if only and name not in only:
            continue
        out.append((name, entry))
    return out


def check_record(manifest: dict, record: dict) -> list[str]:
    """Problems that stop the committed record from covering the manifest."""
    problems: list[str] = []
    expected = {name for name, _ in selected_examples(manifest, [])}
    entries = record.get("examples", {})
    for name in sorted(expected - set(entries)):
        problems.append(f"{name}: retained example has no build receipt")
    for name in sorted(set(entries) - expected):
        problems.append(f"{name}: receipt for an example that is not a retained macOS example")
    for name in sorted(expected & set(entries)):
        receipt = entries[name]
        status = receipt.get("status")
        if status == "built":
            if not receipt.get("targets") or receipt.get("exit_code") != 0:
                problems.append(f"{name}: 'built' needs targets and exit_code 0")
        elif status == "not-configured":
            if not str(receipt.get("reason", "")).strip():
                problems.append(f"{name}: 'not-configured' needs a reason")
        else:
            problems.append(f"{name}: status must be built or not-configured (got {status!r})")
    return problems


def run(args: argparse.Namespace) -> int:
    if args.check_record:
        manifest = json.loads((args.repo_root / MANIFEST).read_text(encoding="utf-8"))
        record_path = args.repo_root / RECORD
        if not record_path.is_file():
            print(f"examples-build-record: {RECORD} does not exist", file=sys.stderr)
            return 1
        record = json.loads(record_path.read_text(encoding="utf-8"))
        problems = check_record(manifest, record)
        for problem in problems:
            print(f"examples-build-record: {problem}", file=sys.stderr)
        if not problems:
            print("examples-build-record: ok")
        return 1 if problems else 0

    manifest = json.loads((args.repo_root / MANIFEST).read_text(encoding="utf-8"))
    by_dir = example_targets(args.build_dir)
    receipts: dict[str, dict] = {}
    for name, entry in selected_examples(manifest, args.only):
        targets = primary_targets(by_dir.get(name, []))
        receipt: dict = {"class": entry["class"]}
        if not targets:
            receipt.update(status="not-configured",
                           reason=entry.get("gate") or "no CMake target in this configuration (SDK project or source-only)")
        elif args.plan:
            receipt.update(status="planned", targets=targets)
        else:
            started = time.monotonic()
            proc = subprocess.run(
                ["cmake", "--build", str(args.build_dir), "--target", *targets],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=args.timeout)
            receipt.update(status="built" if proc.returncode == 0 else "failed",
                           targets=targets, exit_code=proc.returncode,
                           seconds=round(time.monotonic() - started))
            if proc.returncode != 0:
                receipt["tail"] = proc.stdout.splitlines()[-12:] + proc.stderr.splitlines()[-12:]
        receipts[name] = receipt
        print(f"{name}: {receipt['status']}", flush=True)
        args.out.write_text(json.dumps({"build_dir": str(args.build_dir.name), "examples": receipts},
                                       indent=1) + "\n", encoding="utf-8")
    failed = [n for n, r in receipts.items() if r["status"] == "failed"]
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--check-record", action="store_true",
                        help="check docs/status/examples-build-record.json against the manifest")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--plan", action="store_true", help="list targets without building")
    parser.add_argument("--timeout", type=int, default=3000, help="seconds per example")
    args = parser.parse_args(argv)
    if not args.check_record and (args.build_dir is None or args.out is None):
        parser.error("--build-dir and --out are required unless --check-record is given")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
