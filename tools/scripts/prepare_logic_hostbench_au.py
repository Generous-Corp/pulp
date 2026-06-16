#!/usr/bin/env python3
"""Prepare the HostBench AU v2 bundle for Logic DAW-bench validation."""

from __future__ import annotations

import argparse
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_COMPONENT = ROOT / "build" / "AU" / "PulpHostBench.component"
DEFAULT_INSTALL_DIR = pathlib.Path.home() / "Library" / "Audio" / "Plug-Ins" / "Components"
DEFAULT_EXPECTED = {
    "type": "aumf",
    "subtype": "PHBn",
    "manufacturer": "Pulp",
    "factory": "PulpHostBenchAUFactory",
    "symbol": "PulpHostBenchAUFactory",
}


def q(value: str | os.PathLike[str]) -> str:
    return shlex.quote(str(value))


def load_env_file(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        value = value.replace("$HOME", str(pathlib.Path.home()))
        if key:
            values[key] = value
    return values


def resolve_env(args: argparse.Namespace) -> dict[str, str]:
    values = dict(os.environ)
    for candidate in (
        pathlib.Path(args.env_file).expanduser() if args.env_file else None,
        ROOT / "secrets" / "notary.env",
        pathlib.Path.home() / ".config" / "pulp" / "secrets" / "notary.env",
    ):
        if candidate is None:
            continue
        for key, value in load_env_file(candidate).items():
            values.setdefault(key, value)
        if candidate.is_file():
            break
    return values


def run(cmd: list[str], *, dry_run: bool, timeout: float = 120.0) -> int:
    print("+ " + " ".join(q(part) for part in cmd))
    if dry_run:
        return 0
    return subprocess.run(cmd, check=False, timeout=timeout).returncode


def copy_component(source: pathlib.Path, install_dir: pathlib.Path, *, dry_run: bool) -> pathlib.Path:
    target = install_dir / source.name
    print(f"install: {source} -> {target}")
    if dry_run:
        return target
    if not source.is_dir():
        raise FileNotFoundError(f"missing HostBench AU component: {source}")
    install_dir.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(source, target, symlinks=True)
    return target


def preflight_command(component: pathlib.Path, *, identity: str | None, strict_auval: bool) -> list[str]:
    cmd = [
        sys.executable,
        str(ROOT / "tools" / "scripts" / "check_au_component_preflight.py"),
        str(component),
        "--expect-type", DEFAULT_EXPECTED["type"],
        "--expect-subtype", DEFAULT_EXPECTED["subtype"],
        "--expect-manufacturer", DEFAULT_EXPECTED["manufacturer"],
        "--expect-factory", DEFAULT_EXPECTED["factory"],
        "--expect-symbol", DEFAULT_EXPECTED["symbol"],
        "--check-permissions",
        "--check-codesign",
        "--check-gatekeeper",
    ]
    if identity:
        cmd += ["--check-signing-identity", identity]
    if strict_auval:
        cmd += ["--check-auval-list", "--run-auval", "--auval-repeat", "2"]
    return cmd


def notarize_component(component: pathlib.Path, env: dict[str, str], *, dry_run: bool) -> int:
    key = env.get("PULP_NOTARY_KEY_PATH", "")
    key_id = env.get("PULP_NOTARY_KEY_ID", "")
    issuer = env.get("PULP_NOTARY_ISSUER_ID", "")
    missing = [
        name for name, value in (
            ("PULP_NOTARY_KEY_PATH", key),
            ("PULP_NOTARY_KEY_ID", key_id),
            ("PULP_NOTARY_ISSUER_ID", issuer),
        )
        if not value
    ]
    if missing:
        print("prepare_logic_hostbench_au.py: missing notary env: " + ", ".join(missing),
              file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="pulp-hostbench-au-notary-") as tmp:
        zip_path = pathlib.Path(tmp) / "PulpHostBench.component.zip"
        if run(["ditto", "-c", "-k", "--keepParent", str(component), str(zip_path)],
               dry_run=dry_run) != 0:
            return 1
        if run([
            "xcrun", "notarytool", "submit", str(zip_path),
            "--key", key,
            "--key-id", key_id,
            "--issuer", issuer,
            "--wait",
        ], dry_run=dry_run, timeout=1200.0) != 0:
            return 1
    if run(["xcrun", "stapler", "staple", str(component)], dry_run=dry_run) != 0:
        return 1
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=pathlib.Path, default=DEFAULT_COMPONENT,
                        help="built PulpHostBench.component to install")
    parser.add_argument("--install-dir", type=pathlib.Path, default=DEFAULT_INSTALL_DIR,
                        help="AU component install directory")
    parser.add_argument("--identity", default=None,
                        help="Developer ID Application identity for codesign")
    parser.add_argument("--env-file", default=None,
                        help="notary.env path used for signing/notary defaults")
    parser.add_argument("--notarize", action="store_true",
                        help="submit a zipped component to Apple notarytool and staple it")
    parser.add_argument("--skip-install", action="store_true",
                        help="operate on the installed component without copying from build output")
    parser.add_argument("--skip-sign", action="store_true",
                        help="skip Developer ID signing")
    parser.add_argument("--skip-auval", action="store_true",
                        help="skip the final auval discovery/validation checks")
    parser.add_argument("--dry-run", action="store_true",
                        help="print commands without changing files or contacting Apple")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(list(argv or sys.argv[1:]))
    env = resolve_env(args)
    identity = args.identity or env.get("PULP_SIGN_IDENTITY") or env.get("PULP_SIGN_IDENTITY_SHA")

    source = args.component.expanduser()
    install_dir = args.install_dir.expanduser()
    component = install_dir / source.name if args.skip_install else copy_component(
        source, install_dir, dry_run=args.dry_run)

    if not args.skip_sign:
        if not identity:
            print("prepare_logic_hostbench_au.py: missing signing identity; pass --identity "
                  "or set PULP_SIGN_IDENTITY", file=sys.stderr)
            return 2
        if run([
            "codesign", "--force", "--deep", "--sign", identity,
            "--timestamp", "--options", "runtime", str(component),
        ], dry_run=args.dry_run) != 0:
            return 1

    if args.notarize:
        rc = notarize_component(component, env, dry_run=args.dry_run)
        if rc != 0:
            return rc

    run(["rm", "-rf",
         str(pathlib.Path.home() / "Library" / "Caches" / "AudioUnitCache"),
         str(pathlib.Path.home() / "Library" / "Caches" / "com.apple.audiounits.cache")],
        dry_run=args.dry_run)
    run(["killall", "-KILL", "AudioComponentRegistrar"], dry_run=args.dry_run)
    if not args.dry_run:
        time.sleep(5)

    return run(
        preflight_command(component, identity=identity, strict_auval=not args.skip_auval),
        dry_run=args.dry_run,
        timeout=60.0,
    )


if __name__ == "__main__":
    raise SystemExit(main())
