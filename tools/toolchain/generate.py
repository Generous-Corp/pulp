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


def _section_bounds(lines: list[str], section: str) -> tuple[int, int]:
    header = f"[{section}]"
    begin = next((i for i, line in enumerate(lines) if line.strip() == header), None)
    if begin is None:
        raise ValueError(f"missing [{section}] section")
    end = len(lines)
    for i in range(begin + 1, len(lines)):
        if lines[i].startswith("[") and lines[i].rstrip().endswith("]"):
            end = i
            break
    return begin, end


def _set_owned_keys(lines: list[str], section: str, values: dict[str, object], keys: tuple[str, ...]) -> None:
    begin, end = _section_bounds(lines, section)
    for key in keys:
        value = values.get(key)
        if value is None or value == "absent":
            continue
        replacement = f"{key} = {toml_value(value)}"
        found = False
        for i in range(begin + 1, end):
            if re.match(rf"^{re.escape(key)}\s*=", lines[i]):
                lines[i] = replacement + "\n"
                found = True
                break
        if not found:
            lines.insert(begin + 1, replacement + "\n")
            end += 1


def _set_packages(lines: list[str], manager: str, packages: list[str]) -> None:
    begin, end = _section_bounds(lines, manager)
    start = next((i for i in range(begin + 1, end) if re.match(r"^packages\s*=\s*\[", lines[i])), None)
    if start is None:
        lines.insert(begin + 1, "packages = [\n" + "".join(f'  "{item}",\n' for item in packages) + "]\n")
        return
    finish = next((i for i in range(start + 1, end) if lines[i].strip() == "]"), None)
    if finish is None:
        raise ValueError(f"{manager}.packages is unterminated")
    comments = [line for line in lines[start + 1 : finish] if line.lstrip().startswith("#")]
    generated = ["packages = [\n"] + [f'  "{item}",\n' for item in packages] + comments + ["]\n"]
    lines[start : finish + 1] = generated


def update_file(path: Path, data: dict, digest: str, platform: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    marker = re.compile(re.escape(BEGIN) + r"\n.*?" + re.escape(END) + r"\n", re.S)
    if not marker.search(text):
        raise ValueError(f"{path}: generated toolchain block is missing")
    marker_text = f"{BEGIN}\n# source_sha256 = {digest}\n# generated_platform = {platform}\n{END}\n"
    updated = marker.sub(marker_text, text, count=1)
    lines = updated.splitlines(keepends=True)
    target = data.get("platform", {}).get(platform, {})
    common = data["toolchain"]
    values = {key: target.get(key, common.get(key)) for key in TARGETS[platform]["keys"]}
    _set_owned_keys(lines, "toolchain", values, TARGETS[platform]["keys"])
    manager = TARGETS[platform]["manager"]
    if manager:
        packages = data.get("packages", {}).get(manager)
        if not isinstance(packages, list) or not packages:
            raise ValueError(f"packages.{manager} must be a non-empty list")
        _set_packages(lines, manager, packages)
    result = "".join(lines)
    if check and result != text:
        raise ValueError(f"{path}: generated toolchain block or owned fields are stale")
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text

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
        update_file(path, data, digest, platform, check)
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
