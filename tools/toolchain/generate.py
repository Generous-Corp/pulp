#!/usr/bin/env python3
"""Generate the toolchain-owned TOML fragments from one authority."""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/toolchain/manifest.toml"
BEGIN = "# BEGIN GENERATED PULP TOOLCHAIN (tools/toolchain/manifest.toml)"
END = "# END GENERATED PULP TOOLCHAIN"

# Exact versions only. A selector such as "homebrew", "stable", "latest" or a
# bare "3.12" names a moving target, so two hosts reading the same manifest can
# install different tools. Xcode versions are major.minor by Apple's scheme.
EXACT = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
EXACT_XCODE = re.compile(r"^[0-9]+\.[0-9]+(\.[0-9]+)?$")

# manifest key -> (mise tool name, lock backend, lock must carry a checksum,
# platforms as mise `os` selectors, None for all). Rust installs through
# rustup, which verifies its own signed channel manifest; mise records no
# download checksum for it, so its lock entry is version-only and the drift
# check says so rather than pretending otherwise. clang-format comes from
# conda-forge (the same LLVM 21.1.8 release CI installs from PyPI) because the
# PyPI route cannot be installed with `mise install --locked`; conda-forge has
# no windows-arm64 build, so that platform keeps its own clang-format.
MISE_TOOLS = {
    "cmake": ("cmake", "aqua:Kitware/CMake", True, None),
    "ninja": ("ninja", "aqua:ninja-build/ninja", True, None),
    "python": ("python", "core:python", True, None),
    "node": ("node", "core:node", True, None),
    "rust": ("rust", "core:rust", False, None),
    "tart": ("tart", "aqua:openai/tart", True, ("macos",)),  # Tart publishes from openai/tart
    "clang_format": ("conda:clang-format", "conda:clang-format", True, ("linux", "macos", "windows/x64")),
}
MISE_CONFIG = ".mise.toml"
MISE_LOCK = "mise.lock"

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
    _require_exact(data)
    return data, hashlib.sha256(source_bytes(path)).hexdigest()


def _require_exact(data: dict) -> None:
    tc = data["toolchain"]
    floating = [
        f"{key} = {tc[key]!r}"
        for key in MISE_TOOLS
        if not isinstance(tc.get(key), str) or not EXACT.match(tc[key])
    ]
    if not isinstance(tc.get("xcode"), str) or not EXACT_XCODE.match(tc["xcode"]):
        floating.append(f"xcode = {tc.get('xcode')!r}")
    mise = data.get("mise")
    if not isinstance(mise, dict):
        raise ValueError("toolchain authority must have a [mise] table")
    if not isinstance(mise.get("version"), str) or not re.match(r"^[0-9]{4}\.[0-9]+\.[0-9]+$", mise["version"]):
        floating.append(f"mise.version = {mise.get('version')!r}")
    if not isinstance(mise.get("source_commit"), str) or not re.match(r"^[0-9a-f]{40}$", mise["source_commit"]):
        floating.append(f"mise.source_commit = {mise.get('source_commit')!r}")
    if floating:
        raise ValueError("toolchain authority has non-exact pins: " + "; ".join(floating))
    platforms = mise.get("lock_platforms")
    if not isinstance(platforms, list) or not platforms or platforms != sorted(set(platforms)):
        raise ValueError("mise.lock_platforms must be a non-empty, sorted, unique list")
    if mise.get("enforcement") not in ("warn", "enforce"):
        raise ValueError("mise.enforcement must be 'warn' or 'enforce'")
    prefixes = data.get("npm", {}).get("prefixes")
    if not isinstance(prefixes, list) or not prefixes:
        raise ValueError("npm.prefixes must be a non-empty list of lockfile directories")


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

def _mise_tool_line(data: dict, key: str) -> str:
    tc = data["toolchain"]
    name, _backend, _checksum, oses = MISE_TOOLS[key]
    version = tc[key]
    options: list[str] = []
    if key == "rust":
        options += [
            f'targets = "{",".join(tc.get("rust_targets", []))}"',
            f'components = "{",".join(tc.get("rust_components", []))}"',
            'profile = "minimal"',
        ]
    if oses:
        options.append("os = " + toml_value(list(oses)))
    lhs = name if re.match(r"^[A-Za-z0-9_-]+$", name) else f'"{name}"'
    if not options:
        return f'{lhs} = "{version}"'
    return f'{lhs} = {{ version = "{version}", ' + ", ".join(options) + " }"


def render_mise_block(data: dict, digest: str, lock_digest: str) -> str:
    """The generated head of .mise.toml: min_version, [tools], the npm task."""
    mise = data["mise"]
    lines = [
        BEGIN,
        f"# source_sha256 = {digest}",
        f"# mise_lock_sha256 = {lock_digest}",
        "# Edit tools/toolchain/manifest.toml, then run",
        "#   python3 tools/toolchain/generate.py --lock --mise <pinned mise>",
        # warn: older mise prints an upgrade hint and carries on. enforce: it stops.
        (f'min_version = {{ soft = "{mise["version"]}" }}' if mise["enforcement"] == "warn"
         else f'min_version = "{mise["version"]}"'),
        "",
        "[tools]",
    ]
    lines += [_mise_tool_line(data, key) for key in MISE_TOOLS]
    lines += [
        "",
        "[tasks.npm-deps]",
        'description = "Install project npm dependencies from the committed lockfiles (npm ci)"',
        "run = [",
    ]
    lines += [
        f'  "npm ci --prefix {prefix} --no-audit --no-fund",'
        for prefix in data["npm"]["prefixes"]
    ]
    lines += ["]", END, ""]
    return "\n".join(lines)


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else "absent"


def update_mise_config(path: Path, data: dict, digest: str, lock_digest: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    marker = re.compile(re.escape(BEGIN) + r"\n.*?" + re.escape(END) + r"\n", re.S)
    block = render_mise_block(data, digest, lock_digest)
    if marker.search(text):
        result = marker.sub(lambda _m: block, text, count=1)
    else:
        # The block holds a top-level key (min_version), so it must come before
        # the first table header; the hand-written settings and tasks follow it.
        result = block + "\n" + text
    if check and result != text:
        raise ValueError(f"{path}: generated mise block is stale or hand-edited")
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text


def _platform_selected(platform: str, oses: tuple[str, ...]) -> bool:
    os_name, _, arch = platform.partition("-")
    return os_name in oses or f"{os_name}/{arch}" in oses


def check_mise_lock(path: Path, data: dict) -> None:
    """Offline proof that mise.lock pins exactly the manifest's versions."""
    if not path.exists():
        raise ValueError(f"{path}: missing; run generate.py --lock")
    try:
        lock = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as exc:
        raise ValueError(f"{path}: unreadable: {exc}") from exc
    tools = lock.get("tools", {})
    expected = {MISE_TOOLS[key][0]: key for key in MISE_TOOLS}
    extra = sorted(set(tools) - set(expected))
    if extra:
        raise ValueError(f"{path}: tools not in the manifest: {', '.join(extra)}")
    platforms = data["mise"]["lock_platforms"]
    for name, key in expected.items():
        entries = tools.get(name)
        if not isinstance(entries, list) or len(entries) != 1:
            raise ValueError(f"{path}: {name} must have exactly one locked version")
        entry = entries[0]
        _, backend, checksum, oses = MISE_TOOLS[key]
        want = data["toolchain"][key]
        if entry.get("version") != want:
            raise ValueError(f"{path}: {name} is locked at {entry.get('version')!r}, manifest pins {want!r}")
        if entry.get("backend") != backend:
            raise ValueError(f"{path}: {name} backend is {entry.get('backend')!r}, expected {backend!r}")
        if not checksum:
            continue
        for platform in platforms:
            if oses and not _platform_selected(platform, oses):
                continue
            row = entry.get(f"platforms.{platform}", {})
            if not str(row.get("checksum", "")).startswith(("sha256:", "blake3:")) or not row.get("url"):
                raise ValueError(f"{path}: {name}@{want} has no checksum and url for {platform}")


def require_pinned_mise(mise: Path, data: dict) -> None:
    try:
        out = subprocess.run([str(mise), "--version"], capture_output=True, text=True, check=False)
    except OSError as exc:
        raise ValueError(f"cannot run {mise}: {exc}") from exc
    words = out.stdout.split()
    have = words[0] if out.returncode == 0 and words else "unknown"
    if have != data["mise"]["version"]:
        raise ValueError(f"{mise} is mise {have}; the lock must be written by mise {data['mise']['version']}")


def write_mise_lock(pulp_root: Path, data: dict, mise: Path) -> None:
    """Regenerate mise.lock with the pinned mise. Network reads only.

    mise runs with throwaway data, cache, state and config directories so it
    cannot read or write the host's own mise state, and with the global config
    disabled so a host's ~/.config/mise cannot add tools to the lock.
    """
    require_pinned_mise(mise, data)
    lock = pulp_root / MISE_LOCK
    with tempfile.TemporaryDirectory(prefix="pulp-mise-lock-") as td:
        scratch = Path(td)
        env = {
            "HOME": str(scratch),
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "MISE_DATA_DIR": str(scratch / "data"),
            "MISE_CACHE_DIR": str(scratch / "cache"),
            "MISE_STATE_DIR": str(scratch / "state"),
            "MISE_CONFIG_DIR": str(scratch / "config"),
            "MISE_GLOBAL_CONFIG_FILE": str(scratch / "config/none.toml"),
            "MISE_TRUSTED_CONFIG_PATHS": str(pulp_root),
            "MISE_YES": "1",
        }
        for key in ("GITHUB_TOKEN", "MISE_GITHUB_TOKEN", "SSL_CERT_FILE"):
            if key in os.environ:
                env[key] = os.environ[key]
        if lock.exists():
            lock.unlink()  # a fresh resolve; `mise lock` otherwise keeps old rows
        # `mise lock` does not apply a tool's `os` selector, so a tool that has
        # no build for some platform is locked separately, for its platforms.
        groups: dict[tuple[str, ...], list[str]] = {}
        for key, (name, _backend, _checksum, oses) in MISE_TOOLS.items():
            wanted = tuple(
                p for p in data["mise"]["lock_platforms"] if not oses or _platform_selected(p, oses)
            )
            groups.setdefault(wanted, []).append(name)
        for platforms, names in groups.items():
            result = subprocess.run(
                [str(mise), "lock", "--platform", ",".join(platforms), *names],
                cwd=pulp_root, env=env, capture_output=True, text=True, check=False,
            )
            if result.returncode != 0:
                raise ValueError(f"mise lock {' '.join(names)} failed:\n" + result.stderr[-4000:])
    check_mise_lock(lock, data)


def generate(pulp_root: Path, tartci_root: Path | None, check: bool, mise: Path | None = None) -> list[Path]:
    data, digest = load(pulp_root / "tools/toolchain/manifest.toml")
    config = pulp_root / MISE_CONFIG
    lock = pulp_root / MISE_LOCK
    if mise is not None and not check:
        require_pinned_mise(mise, data)
        # mise reads [tools] from .mise.toml, so write the pins before locking,
        # then record the digest of the lock that the pins produced.
        update_mise_config(config, data, digest, _sha256_file(lock), False)
        write_mise_lock(pulp_root, data, mise)
    else:
        check_mise_lock(lock, data)
    update_mise_config(config, data, digest, _sha256_file(lock), check)
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
    parser.add_argument(
        "--lock", action="store_true",
        help="also regenerate mise.lock (network reads) with the mise named by --mise",
    )
    parser.add_argument("--mise", type=Path, help="the pinned mise binary (manifest [mise].version)")
    args = parser.parse_args(argv)
    if args.lock and args.check:
        parser.error("--lock writes; it cannot be combined with --check")
    if args.lock and args.mise is None:
        parser.error("--lock needs --mise <path to the pinned mise binary>")
    try:
        generate(args.pulp_root, args.tartci_root, args.check, args.mise if args.lock else None)
    except (OSError, ValueError) as exc:
        print(f"toolchain-generate: ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
