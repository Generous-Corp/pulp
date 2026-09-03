#!/usr/bin/env python3
"""Build a sealed exact-revision Pulp trace analyzer for an A3 campaign."""

from __future__ import annotations

import hashlib
import json
import os
import pwd
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Any

GIT_REVISION = re.compile(r"^[0-9a-f]{40}$")
RECEIPT_SCHEMA = "pulp.gpu-first-visible-prepared-trace-analyzer.v1"
SOURCE_PREFIXES = (
    "experimental/pulp-rs",
    ".agents/skills/trace-sql",
    "tools/packages/tool-registry.json",
    "tools/scripts/release_product_matrix.json",
    "tools/import-design/browser_capture/runtime_manifest.txt",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def atomic_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8",
    )
    os.replace(temporary, path)


def tool_candidate(name: str) -> Path:
    account_home = Path(pwd.getpwuid(os.getuid()).pw_dir)
    candidates = (
        account_home / ".cargo" / "bin" / name,
        Path("/opt/homebrew/bin") / name,
        Path("/usr/local/bin") / name,
        Path("/usr/bin") / name,
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise RuntimeError(f"sealed analyzer could not locate {name} in fixed toolchain roots")


def tool_record(
    path: Path, retained: Path, environment: dict[str, str], *, cwd: Path,
) -> dict[str, str]:
    resolved = path.resolve()
    version = subprocess.run(
        [str(path), "--version"], env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        timeout=30, check=True, cwd=cwd,
    ).stdout.strip()
    if not version or "\n" in version:
        raise RuntimeError(f"{path.name} returned an invalid version")
    return {
        "command_path": str(path),
        "resolved_path": str(resolved),
        "sha256": sha256(resolved),
        "version": version,
        "retained_path": str(retained),
        "retained_sha256": sha256(retained),
    }


def source_files(root: Path, revision: str) -> list[str]:
    completed = subprocess.run(
        ["/usr/bin/git", "ls-tree", "-r", "--name-only", revision, "--", *SOURCE_PREFIXES],
        cwd=root, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
        timeout=30, check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError("cannot enumerate exact-revision analyzer inputs")
    files = sorted(line for line in completed.stdout.splitlines() if line)
    if not files or "experimental/pulp-rs/Cargo.toml" not in files:
        raise RuntimeError("exact-revision analyzer input set is incomplete")
    return files


def source_digests(root: Path, revision: str, files: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for relative in files:
        completed = subprocess.run(
            ["/usr/bin/git", "show", f"{revision}:{relative}"], cwd=root,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=30, check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(f"cannot resolve {relative} at the requested Pulp revision")
        result[relative] = hashlib.sha256(completed.stdout).hexdigest()
    return result


def export_source_snapshot(
    root: Path, revision: str, workspace: Path,
) -> tuple[Path, Path, dict[str, str]]:
    files = source_files(root, revision)
    digests = source_digests(root, revision, files)
    archive = workspace / "source-snapshot.tar"
    completed = subprocess.run(
        ["/usr/bin/git", "archive", "--format=tar", "--output", str(archive),
         revision, "--", *SOURCE_PREFIXES],
        cwd=root, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, timeout=120, check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError("cannot export exact-revision analyzer input snapshot")
    snapshot = workspace / "source"
    snapshot.mkdir(mode=0o700)
    with tarfile.open(archive, "r:") as handle:
        members = handle.getmembers()
        if any(
            member.issym() or member.islnk() or member.name.startswith("/")
            or ".." in Path(member.name).parts
            for member in members
        ):
            raise RuntimeError("analyzer input snapshot contains an unsafe member")
        handle.extractall(snapshot, filter="data")
    observed = {
        relative: sha256(snapshot / relative)
        for relative in files
        if (snapshot / relative).is_file()
    }
    if observed != digests:
        raise RuntimeError("exported analyzer inputs differ from the exact revision")
    return archive, snapshot, digests


def actual_toolchain(
    rustc_selector: Path, *, cwd: Path, environment: dict[str, str],
) -> tuple[Path, Path]:
    completed = subprocess.run(
        [str(rustc_selector), "--print", "sysroot"], cwd=cwd, env=environment,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, timeout=30, check=False,
    )
    if completed.returncode != 0 or not completed.stdout.strip():
        raise RuntimeError("sealed analyzer could not resolve the selected Rust sysroot")
    tool_bin = Path(completed.stdout.strip()).resolve() / "bin"
    cargo = tool_bin / ("cargo.exe" if os.name == "nt" else "cargo")
    rustc = tool_bin / ("rustc.exe" if os.name == "nt" else "rustc")
    if any(not path.is_file() or not os.access(path, os.X_OK) for path in (cargo, rustc)):
        raise RuntimeError("selected Rust sysroot lacks executable Cargo/rustc binaries")
    return cargo.resolve(), rustc.resolve()


def link_cache(source_home: Path, cargo_home: Path) -> None:
    cargo_home.mkdir(mode=0o700)
    for name in ("registry", "git"):
        source = source_home / name
        if source.is_dir():
            (cargo_home / name).symlink_to(source, target_is_directory=True)


def prepare(argv: list[str]) -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--workspace", required=True, type=Path)
    args = parser.parse_args(argv)
    root_value = os.environ.get("PULP_A3_PULP_ROOT")
    revision = os.environ.get("PULP_A3_PULP_REVISION")
    if not root_value or not revision or GIT_REVISION.fullmatch(revision) is None:
        raise RuntimeError("sealed analyzer requires an exact Pulp root and revision")
    root = Path(root_value).resolve()
    workspace = args.workspace.resolve()
    output = args.output.resolve()
    receipt = args.receipt.resolve()
    if (
        workspace.exists() or output.exists() or receipt.exists()
        or root == workspace or root in workspace.parents
        or workspace not in output.parents or workspace not in receipt.parents
    ):
        raise RuntimeError("sealed analyzer requires a fresh confined workspace")
    workspace.mkdir(parents=True, mode=0o700)
    source_archive, source_root, source_files = export_source_snapshot(
        root, revision, workspace,
    )
    manifest = source_root / "experimental/pulp-rs/Cargo.toml"
    cargo_home = workspace / "cargo-home"
    target = workspace / "target"
    temporary = workspace / "tmp"
    temporary.mkdir(mode=0o700)
    source_home = Path(pwd.getpwuid(os.getuid()).pw_dir) / ".cargo"
    link_cache(source_home.resolve(), cargo_home)
    cargo_selector = tool_candidate("cargo")
    rustc_selector = tool_candidate("rustc")
    selector_path = ":".join(dict.fromkeys((
        str(cargo_selector.parent), str(rustc_selector.parent),
        "/usr/bin", "/bin", "/usr/sbin", "/sbin",
    )))
    selector_environment = {
        "HOME": str(workspace), "LC_ALL": "C", "PATH": selector_path,
        "RUSTUP_HOME": str(Path(pwd.getpwuid(os.getuid()).pw_dir) / ".rustup"),
        "TMPDIR": str(temporary),
    }
    cargo, rustc = actual_toolchain(
        rustc_selector, cwd=manifest.parent, environment=selector_environment,
    )
    path_entries = []
    for entry in (cargo.parent, rustc.parent, Path("/usr/bin"), Path("/bin"),
                  Path("/usr/sbin"), Path("/sbin")):
        value = str(entry)
        if value not in path_entries:
            path_entries.append(value)
    environment = {
        "CARGO_HOME": str(cargo_home),
        "CARGO_NET_OFFLINE": "true",
        "CARGO_TARGET_DIR": str(target),
        "HOME": str(workspace),
        "LC_ALL": "C",
        "PATH": ":".join(path_entries),
        "PULP_RS_BUILD_VERSION": revision[:12],
        "RUSTC": str(rustc),
        "TMPDIR": str(temporary),
    }
    retained_tools = workspace / "retained-tools"
    retained_tools.mkdir(mode=0o700)
    retained_cargo = retained_tools / cargo.name
    retained_rustc = retained_tools / rustc.name
    shutil.copyfile(cargo, retained_cargo)
    shutil.copyfile(rustc, retained_rustc)
    retained_cargo.chmod(0o500)
    retained_rustc.chmod(0o500)
    cargo_record = tool_record(
        cargo, retained_cargo, environment, cwd=manifest.parent,
    )
    rustc_record = tool_record(
        rustc, retained_rustc, environment, cwd=manifest.parent,
    )
    completed = subprocess.run(
        [str(cargo), "build", "--quiet", "--release", "--locked", "--offline",
         "--manifest-path", str(manifest), "--bin", "pulp"],
        cwd=manifest.parent, env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600, check=False,
    )
    if completed.returncode != 0:
        sys.stderr.buffer.write(completed.stderr[-1024 * 1024:])
        return completed.returncode
    built = target / "release" / ("pulp.exe" if os.name == "nt" else "pulp")
    if not built.is_file() or not os.access(built, os.X_OK):
        raise RuntimeError("sealed Cargo build omitted the Pulp analyzer")
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(built, output)
    output.chmod(0o500)
    # Retain the prepared binary and provenance, not a role-local duplicate of
    # Cargo's potentially multi-gigabyte target/cache state.
    shutil.rmtree(target)
    shutil.rmtree(cargo_home)
    shutil.rmtree(temporary)
    shutil.rmtree(source_root)
    atomic_json(receipt, {
        "schema": RECEIPT_SCHEMA,
        "version": 1,
        "pulp_revision": revision,
        "source_files": source_files,
        "source_snapshot_sha256": sha256(source_archive),
        "cargo": cargo_record,
        "rustc": rustc_record,
        "cargo_home_mode": "fresh-config-free-linked-locked-cache",
        "target_directory_fresh": True,
        "analyzer_sha256": sha256(output),
    })
    return 0


def main(argv: list[str]) -> int:
    if not argv or argv[0] != "prepare":
        print("A3 analyzer wrapper supports only the sealed prepare protocol", file=sys.stderr)
        return 2
    try:
        return prepare(argv[1:])
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"A3 analyzer prepare failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
