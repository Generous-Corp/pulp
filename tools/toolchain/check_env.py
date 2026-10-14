#!/usr/bin/env python3
"""Report every host tool whose version differs from tools/toolchain/manifest.toml.

This runs before the pinned toolchain exists, on whatever python3 a host has
(3.9 on a stock Mac), so it uses only the standard library, keeps to Python 3.9
syntax, and reads the generated tools/toolchain/expected.json rather than the
TOML manifest. It never installs or changes anything.

One line per mismatch: tool, expected version, found version, resolved path,
and the single command that fixes it. Exit status:

  0  the environment matches, or it differs and the manifest is in warn mode
  1  it differs and the manifest is in enforce mode
  3  the expected versions could not be read, so nothing was compared

PULP_TOOLCHAIN_UNPINNED=1 lets a contributor without mise run an enforced check
anyway, with a banner on every run. A fleet host refuses it: a host that has
~/.config/pulp/fleet-host or PULP_FLEET_HOST=1 is one CI results come from.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = ROOT / "tools/toolchain/expected.json"
RUST_DIR = "experimental/pulp-rs"

EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_NO_BASELINE = 3
PROBE_TIMEOUT_S = 5

ACTIVATE_FIX = 'MISE_ENV=toolchain mise install --locked && eval "$(MISE_ENV=toolchain mise activate bash)"'
NPM_FIX = "MISE_ENV=toolchain mise run npm-deps"
RUST_FIX = f"rustup toolchain install <version> (cargo reads {RUST_DIR}/rust-toolchain.toml)"
MISE_FIX = "install the pinned mise by digest (tartci host installer); never brew or mise self-update"

# (manifest key, display name, executables to try in order, version pattern)
TOOLS: Tuple[Tuple[str, str, Tuple[str, ...], str], ...] = (
    ("python", "python3", ("python3", "python"), r"Python ([0-9]+\.[0-9]+\.[0-9]+)"),
    ("cmake", "cmake", ("cmake",), r"cmake version ([0-9]+\.[0-9]+\.[0-9]+)"),
    ("ninja", "ninja", ("ninja",), r"^([0-9]+\.[0-9]+\.[0-9]+)"),
    ("node", "node", ("node",), r"^v([0-9]+\.[0-9]+\.[0-9]+)"),
    ("rust", "rustc", ("rustc",), r"rustc ([0-9]+\.[0-9]+\.[0-9]+)"),
    ("clang_format", "clang-format", ("clang-format",), r"clang-format version ([0-9]+\.[0-9]+\.[0-9]+)"),
)


class Finding:
    """One tool that does not match. `note` says why it is expected to differ."""

    def __init__(self, tool: str, expected: str, found: str, path: str, fix: str, note: str = "") -> None:
        self.tool = tool
        self.expected = expected
        self.found = found
        self.path = path
        self.fix = fix
        self.note = note

    def line(self) -> str:
        note = f" [{self.note}]" if self.note else ""
        return (
            f"{self.tool}: expected {self.expected} (tools/toolchain/manifest.toml), "
            f"found {self.found} at {self.path}{note}; fix: {self.fix}"
        )

    def as_dict(self) -> Dict[str, str]:
        return {
            "tool": self.tool, "expected": self.expected, "found": self.found,
            "path": self.path, "fix": self.fix, "note": self.note,
        }


Runner = Callable[[List[str], Optional[Path]], Optional[str]]


def run_version(argv: List[str], cwd: Optional[Path]) -> Optional[str]:
    """stdout+stderr of `argv`, or None when it cannot run within 5 s.

    The bound keeps a hung tool from stalling a CI leg: at most 5 s per probe.

    RUSTUP_AUTO_INSTALL=0: a rustup proxy otherwise downloads the toolchain a
    rust-toolchain.toml names when it is missing, and this check never fetches.
    """
    env = dict(os.environ, RUSTUP_AUTO_INSTALL="0")
    try:
        out = subprocess.run(
            argv, cwd=str(cwd) if cwd else None, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=PROBE_TIMEOUT_S, check=False, env=env,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return (out.stdout or "") + (out.stderr or "")


def host_platform() -> str:
    """The mise lock platform name of this host (macos-arm64, linux-x64, ...)."""
    system = {"darwin": "macos", "linux": "linux", "windows": "windows"}.get(sys.platform, sys.platform)
    if sys.platform.startswith("win"):
        system = "windows"
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x64" if machine in ("x86_64", "amd64") else machine
    return f"{system}-{arch}"


def is_fleet_host(env: Dict[str, str], home: Path) -> bool:
    return env.get("PULP_FLEET_HOST") == "1" or (home / ".config/pulp/fleet-host").exists()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_expected(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or not isinstance(data.get("tools"), dict):
        raise ValueError(f"{path}: not a schema-1 toolchain expectation")
    return data


def check_tools(
    expected: dict, plat: str, which: Callable[[str], Optional[str]], run: Runner, root: Path,
) -> List[Finding]:
    findings: List[Finding] = []
    os_name = plat.split("-", 1)[0]
    overrides = expected.get("platform_overrides", {}).get(os_name, {})
    for key, display, exes, pattern in TOOLS:
        want = expected["tools"][key]
        override = overrides.get(key)
        fix = RUST_FIX.replace("<version>", want) if key == "rust" else ACTIVATE_FIX
        path = next((p for p in (which(e) for e in exes) if p), None)
        if path is None:
            if override == "absent":
                continue
            findings.append(Finding(display, want, "nothing", "PATH", fix, "not installed"))
            continue
        # rustup picks the toolchain from the rust-toolchain.toml nearest the
        # working directory, so ask in the directory cargo builds from.
        cwd = root / RUST_DIR if key == "rust" and (root / RUST_DIR).is_dir() else None
        text = run([path, "--version"], cwd)
        match = re.search(pattern, text or "", re.M)
        found = match.group(1) if match else "unknown"
        if key == "rust" and not match and "not installed" in (text or ""):
            found = "a rust-toolchain.toml toolchain that is not installed"
        if found == want:
            continue
        note = ""
        if override == "system":
            note = f"declared exception: platform.{os_name} {key} = system"
        elif override == "absent":
            note = f"platform.{os_name} {key} = absent, but one is on PATH"
        findings.append(Finding(display, want, found, path, fix, note))
    return findings


def check_mise(
    expected: dict, plat: str, which: Callable[[str], Optional[str]], run: Runner, home: Path,
) -> List[Finding]:
    pin = expected["mise"]
    want = pin["version"]
    pinned_path = Path(pin["path"].replace("~", str(home), 1))
    digest = pin.get("sha256", {}).get(plat)
    resolved = which("mise")
    if resolved is None:
        if pinned_path.is_file():
            return [Finding("mise", want, "the pinned binary, installed but not on PATH", str(pinned_path), f"put {pinned_path.parent} first on PATH")]
        return [Finding("mise", want, "nothing", "PATH", MISE_FIX, "not installed")]
    text = run([resolved, "--version"], None) or ""
    words = text.split()
    found = words[0] if words else "unknown"
    if found != want:
        return [Finding("mise", want, found, resolved, MISE_FIX)]
    real = Path(os.path.realpath(resolved))
    if digest is None:
        return [Finding("mise", want, f"{found} (no pinned digest for {plat})", str(real), MISE_FIX, "unpinned platform")]
    have = sha256_file(real)
    if have != digest:
        return [Finding("mise", f"{want} sha256 {digest}", f"{found} sha256 {have}", str(real), MISE_FIX, "same version, different binary")]
    return []


def check_npm(expected: dict, root: Path) -> List[Finding]:
    """Each lockfile directory's node_modules must be exactly its package-lock.json.

    npm ci writes node_modules/.package-lock.json listing what it installed.
    Every installed package must carry the locked version, and every locked
    package that is not optional (a platform-specific binary such as
    @esbuild/linux-x64 is optional elsewhere) must be installed.
    """
    findings: List[Finding] = []
    for prefix in expected.get("npm_prefixes", []):
        lock_path = root / prefix / "package-lock.json"
        hidden_path = root / prefix / "node_modules/.package-lock.json"
        if not lock_path.is_file():
            findings.append(Finding(f"npm {prefix}", "package-lock.json", "nothing", str(lock_path), "restore the committed lockfile"))
            continue
        if not hidden_path.is_file():
            findings.append(Finding(f"npm {prefix}", "npm ci from package-lock.json", "no node_modules", str(root / prefix), NPM_FIX, "not installed"))
            continue
        try:
            locked = json.loads(lock_path.read_text(encoding="utf-8")).get("packages", {})
            installed = json.loads(hidden_path.read_text(encoding="utf-8")).get("packages", {})
        except (OSError, ValueError) as exc:
            findings.append(Finding(f"npm {prefix}", "readable lockfiles", f"unreadable ({exc})", str(hidden_path), NPM_FIX))
            continue
        drift: List[str] = []
        for name, row in sorted(locked.items()):
            if not name:
                continue  # the root project itself
            have = installed.get(name)
            if have is None:
                # npm skips an optional package another platform needs; a
                # package both dev and optional is marked devOptional.
                if not (row.get("optional") or row.get("devOptional")):
                    drift.append(f"{name} missing")
            elif have.get("version") != row.get("version"):
                drift.append(f"{name} {have.get('version')} != {row.get('version')}")
        drift += [f"{name} not in the lockfile" for name in sorted(set(installed) - set(locked))]
        if drift:
            shown = "; ".join(drift[:5]) + (f"; and {len(drift) - 5} more" if len(drift) > 5 else "")
            findings.append(Finding(f"npm {prefix}", "package-lock.json", shown, str(root / prefix / "node_modules"), NPM_FIX))
    return findings


def collect(
    expected: dict, root: Path, home: Path, plat: str,
    which: Callable[[str], Optional[str]] = shutil.which, run: Runner = run_version,
) -> List[Finding]:
    return (
        check_tools(expected, plat, which, run, root)
        + check_mise(expected, plat, which, run, home)
        + check_npm(expected, root)
    )


def verdict(findings: List[Finding], enforcement: str, env: Dict[str, str], home: Path) -> Tuple[int, List[str]]:
    """Exit status and banner lines for a set of findings."""
    banners: List[str] = []
    if not findings:
        return EXIT_OK, banners
    unpinned = env.get("PULP_TOOLCHAIN_UNPINNED") == "1"
    fleet = is_fleet_host(env, home)
    if unpinned:
        found = ", ".join(f"{f.tool} {f.found}" for f in findings)
        banners.append(f"UNPINNED TOOLCHAIN: results may not reproduce on CI; found {found}")
        if fleet:
            banners.append("PULP_TOOLCHAIN_UNPINNED is refused on a fleet host (PULP_FLEET_HOST or ~/.config/pulp/fleet-host)")
    if enforcement != "enforce":
        return EXIT_OK, banners
    if unpinned and not fleet:
        return EXIT_OK, banners
    return EXIT_MISMATCH, banners


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="the Pulp checkout (default: this one)")
    parser.add_argument("--expected", type=Path, help="default: <root>/tools/toolchain/expected.json")
    parser.add_argument("--format", choices=("text", "github", "json"), default="text",
                        help="github prints ::warning:: annotations for Actions")
    parser.add_argument("--quiet", action="store_true", help="print nothing when everything matches")
    args = parser.parse_args(argv)
    expected_path = args.expected or args.root / "tools/toolchain/expected.json"
    try:
        expected = load_expected(expected_path)
    except (OSError, ValueError) as exc:
        print(
            f"toolchain-check: cannot read the expected versions: {exc}; "
            "fix: python3 tools/toolchain/generate.py",
            file=sys.stderr,
        )
        return EXIT_NO_BASELINE
    home = Path(os.path.expanduser("~"))
    env = dict(os.environ)
    findings = collect(expected, args.root, home, host_platform())
    status, banners = verdict(findings, expected.get("enforcement", "warn"), env, home)
    mode = expected.get("enforcement", "warn")
    if args.format == "json":
        print(json.dumps({
            "enforcement": mode, "platform": host_platform(), "exit": status,
            "banners": banners, "findings": [f.as_dict() for f in findings],
        }, indent=2))
        return status
    if not findings:
        if not args.quiet:
            print(f"toolchain-check: matches tools/toolchain/manifest.toml ({mode} mode)")
        return status
    for banner in banners:
        print(f"::warning title=toolchain::{banner}" if args.format == "github" else f"toolchain-check: {banner}")
    for finding in findings:
        if args.format == "github":
            print(f"::warning title=toolchain {finding.tool}::{finding.line()}")
        else:
            print(f"toolchain-check: {finding.line()}")
    tail = "refusing (enforce mode)" if status == EXIT_MISMATCH else f"continuing ({mode} mode)"
    print(f"toolchain-check: {len(findings)} difference(s) from the manifest; {tail}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
