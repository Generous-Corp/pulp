#!/usr/bin/env python3
"""Move one pin in tools/toolchain/manifest.toml and show exactly what changed.

    python3 tools/toolchain/update.py cmake --to 4.3.4 --mise <pinned mise>
    python3 tools/toolchain/update.py rust --to 1.98.0
    python3 tools/toolchain/update.py shipyard --to v0.270.0

It edits the manifest (the version, and the tool's [evidence] row, which
becomes a `target` until the gate is seen running it), regenerates every
derived file through generate.py (for a mise tool that includes
mise.toolchain.lock, written by the pinned mise), re-runs the drift check, and
prints a Markdown table of every version and digest that moved, for the PR
body. It works on the checkout it is run in and never commits, pushes, or
installs anything; review the diff and open the PR yourself, unarmed.
"""
from __future__ import annotations

import argparse
import datetime
import re
import sys
import tomllib
from pathlib import Path

import generate

ROOT = generate.ROOT
# Tools whose pin lives in [toolchain]; rust has no lock rows (rustup).
TOOLCHAIN_KEYS = ("cmake", "ninja", "python", "node", "rust", "tart", "clang_format")


def lock_rows(lock_text: str) -> dict[tuple[str, str], tuple[str, str]]:
    """(lock name, platform) -> (version, checksum) for every locked tool row."""
    rows: dict[tuple[str, str], tuple[str, str]] = {}
    if not lock_text:
        return rows
    lock = tomllib.loads(lock_text)
    for name, entries in lock.get("tools", {}).items():
        for entry in entries:
            for key, row in entry.items():
                if key.startswith("platforms."):
                    rows[(name, key.split(".", 1)[1])] = (entry.get("version", ""), row.get("checksum", ""))
    return rows


def short(checksum: str) -> str:
    algo, _, digest = checksum.partition(":")
    return f"{algo}:{digest[:12]}" if digest else checksum


def diff_table(before: dict, after: dict, lock_before: str, lock_after: str) -> list[str]:
    """Markdown rows for every pin, lock row and lock digest that changed."""
    lines = ["| What | Platform | Before | After |", "|---|---|---|---|"]
    for key in TOOLCHAIN_KEYS:
        old, new = before["toolchain"].get(key), after["toolchain"].get(key)
        if old != new:
            lines.append(f"| {key} pin | all | {old} | {new} |")
    old_sy, new_sy = before["shipyard"], after["shipyard"]
    if old_sy["version"] != new_sy["version"]:
        lines.append(f"| shipyard pin | all | {old_sy['version']} | {new_sy['version']} |")
    if old_sy["installer_sha256"] != new_sy["installer_sha256"]:
        lines.append(
            f"| shipyard install.sh | all | {old_sy['installer_sha256'][:12]} | {new_sy['installer_sha256'][:12]} |"
        )
    for name in sorted(set(old_sy["assets"]) | set(new_sy["assets"])):
        old, new = old_sy["assets"].get(name, "absent"), new_sy["assets"].get(name, "absent")
        if old != new:
            lines.append(f"| shipyard asset {name} | {name} | {old[:12]} | {new[:12]} |")
    rows_before, rows_after = lock_rows(lock_before), lock_rows(lock_after)
    for name, platform in sorted(set(rows_before) | set(rows_after)):
        old = rows_before.get((name, platform), ("absent", ""))
        new = rows_after.get((name, platform), ("absent", ""))
        if old != new:
            lines.append(
                f"| {name} lock | {platform} | {old[0]} {short(old[1])} | {new[0]} {short(new[1])} |"
            )
    old_lock, new_lock = before["mise"]["lock_sha256"], after["mise"]["lock_sha256"]
    if old_lock != new_lock:
        lines.append(f"| mise.toolchain.lock sha256 | all | {old_lock[:12]} | {new_lock[:12]} |")
    return lines if len(lines) > 2 else []


def set_pin(manifest: Path, key: str, version: str, today: str) -> None:
    """Rewrite `key = "..."` in [toolchain] and mark its evidence a target."""
    lines = manifest.read_text(encoding="utf-8").splitlines(keepends=True)
    begin, end = generate._section_bounds(lines, "toolchain")
    index = next((i for i in range(begin + 1, end) if re.match(rf"^{key}\s*=", lines[i])), None)
    if index is None:
        raise ValueError(f"{manifest}: [toolchain] has no {key}")
    lines[index] = f'{key} = "{version}"\n'
    e_begin, e_end = generate._section_bounds(lines, "evidence")
    e_index = next((i for i in range(e_begin + 1, e_end) if re.match(rf"^{key}\s*=", lines[i])), None)
    if e_index is None:
        raise ValueError(f"{manifest}: [evidence] has no {key}")
    source = f"pin moved to {version} by tools/toolchain/update.py on {today}; the gate is not yet seen running it"
    lines[e_index] = f'{key} = {{ status = "target", source = "{source}" }}\n'
    manifest.write_text("".join(lines), encoding="utf-8")


def owned_files(root: Path) -> list[Path]:
    """Every file update() may write: restored together when it fails."""
    names = [
        "tools/toolchain/manifest.toml", generate.MISE_LOCK, generate.MISE_TOOLS_CONFIG, generate.MISE_CONFIG,
        generate.EXPECTED, generate.SHIPYARD_PIN, generate.SHIPYARD_SUMS,
        ".shipyard/vm-image.toml", ".shipyard/vm-image.intel.toml",
    ]
    return [root / name for name in names]


def update(root: Path, tool: str, version: str, mise: Path | None, today: str) -> list[str]:
    snapshot = {path: path.read_bytes() for path in owned_files(root) if path.exists()}
    try:
        return _update(root, tool, version, mise, today)
    except (OSError, ValueError):
        for path, data in snapshot.items():
            path.write_bytes(data)
        raise


def _update(root: Path, tool: str, version: str, mise: Path | None, today: str) -> list[str]:
    manifest = root / "tools/toolchain/manifest.toml"
    lock = root / generate.MISE_LOCK
    before, _ = generate.load(manifest)
    lock_before = lock.read_text(encoding="utf-8") if lock.exists() else ""
    if tool == "shipyard":
        generate.generate(root, None, False, shipyard=version)
    elif tool == "mise":
        raise ValueError(
            "mise itself moves with a reviewed source build: build the tag in a disposable VM, "
            "record [mise] version, source_commit and [mise.binary] sha256 by hand, then "
            "regenerate the lock with generate.py --lock --mise <the new binary>"
        )
    elif tool in TOOLCHAIN_KEYS:
        if before["toolchain"][tool] == version:
            raise ValueError(f"{tool} is already pinned at {version}")
        locked = tool in generate.mise_tools(before)
        if locked and mise is None:
            raise ValueError(f"{tool} is in mise.toolchain.lock; pass --mise <the pinned mise {before['mise']['version']}>")
        set_pin(manifest, tool, version, today)
        generate.generate(root, None, False, mise if locked else None)
    else:
        raise ValueError(f"unknown tool {tool!r}; one of: {', '.join(TOOLCHAIN_KEYS + ('shipyard',))}")
    generate.generate(root, None, True)
    after, _ = generate.load(manifest)
    return diff_table(before, after, lock_before, lock.read_text(encoding="utf-8") if lock.exists() else "")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("tool", help=f"one of: {', '.join(TOOLCHAIN_KEYS + ('shipyard',))}")
    parser.add_argument("--to", required=True, help="the exact version (a release tag for shipyard)")
    parser.add_argument("--mise", type=Path, help="the pinned mise binary, needed for a tool in the lock")
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        table = update(args.root, args.tool, args.to, args.mise, datetime.date.today().isoformat())
    except (OSError, ValueError) as exc:
        print(f"toolchain-update: ERROR: {exc}", file=sys.stderr)
        return 2
    if not table:
        print("toolchain-update: nothing changed")
        return 0
    print("\n".join(table))
    print(
        "\ntoolchain-update: the manifest and every generated file are updated and the drift check passes. "
        "Review `git diff`, then open an unarmed PR with this table in its body."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
