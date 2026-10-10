#!/usr/bin/env python3
"""Generate Pulp and TartCI tool sections from one TOML authority."""
from __future__ import annotations
import argparse, hashlib, re, sys, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/toolchain/manifest.toml"
BEGIN = "# BEGIN GENERATED PULP TOOLCHAIN (tools/toolchain/manifest.toml)"
END = "# END GENERATED PULP TOOLCHAIN"

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
    missing = [k for k in required if k not in data["toolchain"]]
    if missing:
        raise ValueError("toolchain authority missing: " + ", ".join(missing))
    digest = hashlib.sha256(source_bytes(path)).hexdigest()
    return data, digest

def toml_value(value):
    if isinstance(value, list):
        return "[" + ", ".join('"' + str(x) + '"' for x in value) + "]"
    return '"' + str(value) + '"'

def block(data: dict, digest: str, platform: str = "macos") -> str:
    t = data["toolchain"] if platform == "macos" else data["platform"][platform]
    common = data["toolchain"]
    def get(k): return t.get(k, common.get(k))
    package_key = "brew" if platform == "macos" else ("apt" if platform == "linux" else "choco")
    packages = data.get("packages", {}).get(package_key)
    if not isinstance(packages, list) or not packages: raise ValueError(f"packages.{package_key} must be a non-empty list")
    lines = [BEGIN, f"# source_sha256 = {digest}", f"# generated_platform = {platform}", "[toolchain]"]
    keys = ("xcode", "cmake", "ninja", "python", "node", "rust", "tart", "clang_format") if platform == "macos" else ("cmake", "python", "node", "rust")
    for key in keys:
        value = get(key)
        if value is not None and value != "absent": lines.append(f"{key} = {toml_value(value)}")
    targets = get("rust_targets")
    if targets: lines.append(f"rust_targets = {toml_value(targets)}")
    manager = "brew" if platform == "macos" else ("apt" if platform == "linux" else "choco")
    lines += ["", f"[{manager}]", "packages = ["]
    for package in packages: lines.append(f'  "{package}",')
    lines += ["]", END]
    return "\n".join(lines) + "\n"

def replace_block(path: Path, generated: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(re.escape(BEGIN) + r"\n.*?" + re.escape(END) + r"\n", re.S)
    if not pattern.search(text):
        raise ValueError(f"{path}: generated toolchain block is missing")
    updated = pattern.sub(generated, text, count=1)
    if check:
        if updated != text: raise ValueError(f"{path}: generated toolchain block is stale or hand-edited")
    else:
        path.write_text(updated, encoding="utf-8")
    return updated != text

def generate(pulp_root: Path, tartci_root: Path | None, check: bool) -> list[Path]:
    data, digest = load(pulp_root / "tools/toolchain/manifest.toml")
    targets = [pulp_root / ".shipyard/vm-image.toml"]
    for path, platform in ((pulp_root / ".shipyard/vm-image.intel.toml", "macos"),):
        targets.append(path)
    if tartci_root:
        targets += [tartci_root / "manifests/pulp.macos.toml", tartci_root / "manifests/pulp.linux.toml", tartci_root / "manifests/pulp.windows.toml"]
    for path in targets:
        platform = "macos" if "macos" in path.name or "vm-image" in path.name else ("linux" if "linux" in path.name else "windows")
        replace_block(path, block(data, digest, platform), check)
    return targets

def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--pulp-root", type=Path, default=ROOT)
    ap.add_argument("--tartci-root", type=Path)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)
    try: generate(args.pulp_root, args.tartci_root, args.check)
    except (OSError, ValueError) as exc:
        print(f"toolchain-generate: ERROR: {exc}", file=sys.stderr); return 2
    return 0
if __name__ == "__main__": raise SystemExit(main())
