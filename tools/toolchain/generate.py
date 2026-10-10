#!/usr/bin/env python3
"""Generate the toolchain-owned TOML fragments from one authority."""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/toolchain/manifest.toml"
BEGIN = "# BEGIN GENERATED PULP TOOLCHAIN (tools/toolchain/manifest.toml)"
END = "# END GENERATED PULP TOOLCHAIN"

TARGETS = {
    "macos": {"keys": ("xcode", "cmake", "ninja", "python", "node", "rust", "rust_targets", "tart", "clang_format"), "manager": "brew"},
    "intel": {"keys": ("rust", "rust_targets"), "manager": None},
    "linux": {"keys": ("cmake", "python", "node"), "manager": "apt"},
    "windows": {"keys": ("cmake", "python", "node"), "manager": "choco"},
}


def source_bytes(path: Path = SOURCE) -> bytes:
    return path.read_bytes()


def load(path: Path = SOURCE) -> tuple[dict, str]:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ValueError(f"cannot read toolchain authority: {exc}") from exc
    if data.get("schema") != 1 or not isinstance(data.get("toolchain"), dict):
        raise ValueError("toolchain authority must have schema=1 and [toolchain]")
    required = ("xcode", "cmake", "ninja", "python", "node", "rust", "tart", "clang_format")
    missing = [key for key in required if key not in data["toolchain"]]
    if missing:
        raise ValueError("toolchain authority missing: " + ", ".join(missing))
    return data, hashlib.sha256(source_bytes(path)).hexdigest()


def toml_value(value: object) -> str:
    if isinstance(value, list):
        return "[" + ", ".join('"' + str(item) + '"' for item in value) + "]"
    if isinstance(value, bool):
        return "true" if value else "false"
    return '"' + str(value) + '"'


def block(data: dict, digest: str, platform: str) -> str:
    if platform not in TARGETS:
        raise ValueError(f"unsupported generation target: {platform}")
    target = data.get("platform", {}).get(platform, {})
    common = data["toolchain"]
    config = TARGETS[platform]

    def value_for(key: str) -> object:
        return target.get(key, common.get(key))

    lines = [BEGIN, f"# source_sha256 = {digest}", f"# generated_platform = {platform}"]
    for key in config["keys"]:
        value = value_for(key)
        if value is not None and value != "absent":
            lines.append(f"{key} = {toml_value(value)}")
    manager = config["manager"]
    if manager:
        packages = data.get("packages", {}).get(manager)
        if not isinstance(packages, list) or not packages:
            raise ValueError(f"packages.{manager} must be a non-empty list")
        lines.extend(["", f"[{manager}]", "packages = ["])
        lines.extend(f'  "{package}",' for package in packages)
        lines.append("]")
    lines.append(END)
    return "\n".join(lines) + "\n"


def replace_block(path: Path, generated: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(re.escape(BEGIN) + r"\n.*?" + re.escape(END) + r"\n", re.S)
    if not pattern.search(text):
        raise ValueError(f"{path}: generated toolchain block is missing")
    updated = pattern.sub(generated, text, count=1)
    if check and updated != text:
        raise ValueError(f"{path}: generated toolchain block is stale or hand-edited")
    if not check:
        path.write_text(updated, encoding="utf-8")
    return updated != text


def generate(pulp_root: Path, tartci_root: Path | None, check: bool) -> list[Path]:
    data, digest = load(pulp_root / "tools/toolchain/manifest.toml")
    targets: list[tuple[Path, str]] = [
        (pulp_root / ".shipyard/vm-image.toml", "macos"),
        (pulp_root / ".shipyard/vm-image.intel.toml", "intel"),
    ]
    if tartci_root:
        targets.extend(
            [
                (tartci_root / "manifests/pulp.macos.toml", "macos"),
                (tartci_root / "manifests/pulp.linux.toml", "linux"),
                (tartci_root / "manifests/pulp.windows.toml", "windows"),
            ]
        )
    for path, platform in targets:
        replace_block(path, block(data, digest, platform), check)
    return [path for path, _ in targets]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pulp-root", type=Path, default=ROOT)
    parser.add_argument("--tartci-root", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        generate(args.pulp_root, args.tartci_root, args.check)
    except (OSError, ValueError) as exc:
        print(f"toolchain-generate: ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
