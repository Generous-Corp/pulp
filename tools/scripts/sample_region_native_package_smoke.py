#!/usr/bin/env python3
"""Stage, sign, and validate the three F4 native plugin bundles.

This is the package half of PUB-04.  It deliberately operates on bundles
already produced by the D4 example/build lane; it does not build, modify, or
discover a Processor.  By default installation is into a temporary directory.
``--system-install`` is an explicit, reversible opt-in for host validation:
existing bundles are moved to a private backup and restored on exit.

The script writes one JSON receipt containing bundle hashes, install paths,
codesign results, and validator stdout/stderr.  A receipt with skipped tools is
useful diagnostic evidence, but is not a passing native-proof receipt.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import plistlib
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


FORMATS = ("au", "vst3", "clap")
EXPECTED_BUNDLE_ID = "com.pulp.sample-region-allpass"
INSTALL_DIRS = {
    "au": Path("Library/Audio/Plug-Ins/Components"),
    "vst3": Path("Library/Audio/Plug-Ins/VST3"),
    "clap": Path("Library/Audio/Plug-Ins/CLAP"),
}
TOOLS = {"au": "AUVAL", "vst3": "PLUGINVAL", "clap": "CLAP_VALIDATOR"}


def log(message: str) -> None:
    print(f"[sample-region-package] {message}", flush=True)


def sha256_tree(path: Path) -> str:
    digest = hashlib.sha256()
    if path.is_file():
        digest.update(path.read_bytes())
        return digest.hexdigest()
    for child in sorted(path.rglob("*")):
        if child.is_symlink():
            raise ValueError(f"bundle contains symlink: {child}")
        if child.is_file():
            digest.update(child.relative_to(path).as_posix().encode())
            digest.update(child.read_bytes())
    return digest.hexdigest()


def resolve_bundle(path: Path, fmt: str) -> Path:
    candidate = path.expanduser()
    if not candidate.is_absolute():
        candidate = Path.cwd() / candidate
    if candidate.is_symlink() or not candidate.is_dir():
        raise ValueError(f"{fmt} bundle must be a real directory: {candidate}")
    expected_suffix = {"au": ".component", "vst3": ".vst3", "clap": ".clap"}[fmt]
    if candidate.suffix != expected_suffix:
        raise ValueError(f"{fmt} bundle must end in {expected_suffix}: {candidate}")
    # Reject symlinks anywhere in a copied input.  This keeps staging bounded
    # to the D4-produced artifact and avoids copying an arbitrary host path.
    for child in candidate.rglob("*"):
        if child.is_symlink():
            raise ValueError(f"{fmt} bundle contains symlink: {child}")
    return candidate.resolve()


def copy_bundle(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"refusing to overwrite installed bundle: {destination}")
    shutil.copytree(source, destination, symlinks=False)


def run_command(command: list[str]) -> dict[str, Any]:
    try:
        result = subprocess.run(command, text=True, capture_output=True, check=False)
    except OSError as exc:
        return {"command": command, "returncode": None, "stdout": "", "stderr": str(exc)}
    return {
        "command": command,
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def sign_bundle(bundle: Path, *, identity: str, required: bool) -> dict[str, Any]:
    codesign = os.environ.get("CODESIGN", "codesign")
    if platform.system() != "Darwin" and not shutil.which(codesign):
        return {"status": "skipped", "reason": "codesign unavailable on this host"}
    if not shutil.which(codesign):
        result = {"status": "failed", "reason": f"missing {codesign}"}
        if required:
            raise RuntimeError(result["reason"])
        return result
    command = [codesign, "--force", "--deep", "--sign", identity, str(bundle)]
    result = run_command(command)
    if result["returncode"] != 0:
        result["status"] = "failed"
        if required:
            raise RuntimeError(f"codesign failed for {bundle}: {result['stderr']}")
    else:
        verify = run_command([codesign, "--verify", "--deep", "--strict", str(bundle)])
        result["verify"] = verify
        if verify["returncode"] != 0:
            result["status"] = "failed"
            if required:
                raise RuntimeError(f"codesign verification failed for {bundle}: {verify["stderr"]}")
        else:
            result["status"] = "passed"
    return result


def assert_bundle_identity(bundle: Path, fmt: str) -> dict[str, Any]:
    """Prove the staged artifact carries the packet's stable bundle identity."""
    expected = EXPECTED_BUNDLE_ID.encode("ascii")
    matches = []
    for child in bundle.rglob("*"):
        if child.is_file() and not child.is_symlink():
            try:
                if expected in child.read_bytes():
                    matches.append(child.relative_to(bundle).as_posix())
            except OSError:
                continue
    if not matches:
        raise ValueError(f"{fmt} bundle does not contain bundle id {EXPECTED_BUNDLE_ID}")
    plist_ids = []
    for plist_path in bundle.rglob("*.plist"):
        try:
            with plist_path.open("rb") as stream:
                plist = plistlib.load(stream)
            value = plist.get("CFBundleIdentifier")
            if value is not None:
                plist_ids.append(value)
                if value != EXPECTED_BUNDLE_ID:
                    raise ValueError(f"{fmt} plist has wrong CFBundleIdentifier: {value!r}")
        except plistlib.InvalidFileException:
            continue
    return {"bundle_id": EXPECTED_BUNDLE_ID, "binary_matches": matches, "plist_ids": plist_ids}

def au_identity(bundle: Path) -> tuple[str, str, str]:
    plist_path = bundle / "Contents" / "Info.plist"
    with plist_path.open("rb") as stream:
        plist = plistlib.load(stream)
    components = plist.get("AudioComponents")
    if not isinstance(components, list) or not components or not isinstance(components[0], dict):
        raise ValueError(f"{plist_path} has no AudioComponents entry")
    component = components[0]
    values = tuple(component.get(key) for key in ("type", "subtype", "manufacturer"))
    if not all(isinstance(value, (str, bytes)) for value in values):
        raise ValueError(f"{plist_path} has incomplete AudioComponents identity")
    decoded = tuple(value.decode("ascii") if isinstance(value, bytes) else value for value in values)
    return decoded  # type: ignore[return-value]


def validator_command(fmt: str, bundle: Path) -> list[str]:
    tool_name = TOOLS[fmt]
    tool = os.environ.get(tool_name, tool_name.lower().replace("_", "-"))
    if fmt == "au":
        return [tool, "-v", *au_identity(bundle)]
    if fmt == "vst3":
        return [tool, "--validate", str(bundle), "--strictness-level", "3", "--skip-gui-tests"]
    return [tool, "validate", str(bundle)]


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--au", type=Path, required=True, help="D4-produced .component bundle")
    parser.add_argument("--vst3", type=Path, required=True, help="D4-produced .vst3 bundle")
    parser.add_argument("--clap", type=Path, required=True, help="D4-produced .clap bundle")
    parser.add_argument("--evidence", type=Path, required=True, help="JSON receipt output path")
    parser.add_argument("--install-root", type=Path, help="staging root (default: temporary directory)")
    parser.add_argument("--system-install", action="store_true", help="install under ~/Library/Audio/Plug-Ins and restore on exit")
    parser.add_argument("--replace-existing", action="store_true", help="with --system-install, back up an existing same-name bundle")
    parser.add_argument("--identity", default="-", help="codesign identity (default: ad-hoc)")
    parser.add_argument("--allow-unsigned", action="store_true", help="record signing skips/failures instead of failing")
    parser.add_argument("--allow-missing-tools", action="store_true", help="record unavailable validators and return success only if none failed")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    evidence: dict[str, Any] = {
        "packet": "PKT-F4-01",
        "acceptance": "PUB-04",
        "started_at": datetime.now(timezone.utc).isoformat(),
        "host": {"system": platform.system(), "machine": platform.machine()},
        "bundles": {},
        "validators": {},
        "status": "failed",
    }
    restored: list[tuple[Path, Path]] = []
    temp_root: Path | None = None
    try:
        sources = {fmt: resolve_bundle(getattr(args, fmt), fmt) for fmt in FORMATS}
        for fmt, source in sources.items():
            evidence["bundles"][fmt] = {"source": str(source), "sha256": sha256_tree(source)}
        if args.system_install:
            if platform.system() != "Darwin":
                raise RuntimeError("--system-install is supported only on macOS")
            root = Path.home()
        elif args.install_root:
            root = args.install_root.expanduser().resolve()
            root.mkdir(parents=True, exist_ok=True)
        else:
            temp_root = Path(tempfile.mkdtemp(prefix="pulp-sample-region-native-"))
            root = temp_root
        installed: dict[str, Path] = {}
        for fmt, source in sources.items():
            destination = root / INSTALL_DIRS[fmt] / source.name if args.system_install else root / source.name
            if destination.exists() or destination.is_symlink():
                if not args.system_install or not args.replace_existing:
                    raise RuntimeError(f"refusing to overwrite {destination}; use --replace-existing only for system install")
                backup = Path(tempfile.mkdtemp(prefix="pulp-sample-region-backup-")) / destination.name
                backup.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(destination), str(backup))
                restored.append((backup, destination))
            copy_bundle(source, destination)
            installed[fmt] = destination
            evidence["bundles"][fmt]["installed"] = str(destination)
            evidence["bundles"][fmt]["identity"] = assert_bundle_identity(destination, fmt)
            evidence["bundles"][fmt]["signing"] = sign_bundle(destination, identity=args.identity, required=not args.allow_unsigned)
        for fmt, bundle in installed.items():
            command = validator_command(fmt, bundle)
            tool = command[0]
            if not shutil.which(tool):
                result = {"command": command, "status": "unavailable", "reason": f"missing {tool}"}
                evidence["validators"][fmt] = result
                if not args.allow_missing_tools:
                    raise RuntimeError(f"missing validator {tool} for {fmt}")
                continue
            result = run_command(command)
            result["status"] = "passed" if result["returncode"] == 0 else "failed"
            evidence["validators"][fmt] = result
            if result["returncode"] != 0:
                raise RuntimeError(f"{fmt} validator failed (exit {result['returncode']})")
        evidence["status"] = "passed"
    except (OSError, ValueError, RuntimeError) as exc:
        evidence["error"] = str(exc)
        log(f"FAIL: {exc}")
    finally:
        for backup, destination in reversed(restored):
            if destination.exists() or destination.is_symlink():
                shutil.rmtree(destination) if destination.is_dir() and not destination.is_symlink() else destination.unlink()
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(backup), str(destination))
        if temp_root:
            shutil.rmtree(temp_root, ignore_errors=True)
        evidence["finished_at"] = datetime.now(timezone.utc).isoformat()
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if evidence["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
