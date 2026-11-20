#!/usr/bin/env python3
"""Verify and stage the lock-pinned darwin-arm64 Vellum SDK.

Release wiring and the distribution guard come next. Nothing in Pulp's CMake
reads ``share/pulp/vellum-sdk`` yet.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path, PurePosixPath

SUPPORTED_PLATFORMS = {"darwin-arm64"}


def _lock(path: Path) -> tuple[str, str, str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    vellum = data["vellum"]
    return vellum["tag"], vellum["sdk_digest"], vellum["sdk_url"]


def archive_name(tag: str, platform: str) -> str:
    return f"vellum-sdk-{tag.removeprefix('v')}-{platform}.tar.gz"


def resolve_archive(lock_path: Path, platform: str, archive: Path | None,
                    archive_dir: Path | None, fetch: bool = False) -> tuple[Path | None, str]:
    tag, digest, url = _lock(lock_path)
    if platform not in SUPPORTED_PLATFORMS:
        raise ValueError(
            f"Vellum publishes no SDK for {platform} yet; only darwin-arm64 is lock-pinned"
        )
    candidate = archive
    if candidate is None and archive_dir is not None:
        candidate = archive_dir / archive_name(tag, platform)
    if fetch and (candidate is None or not candidate.is_file()):
        if archive_dir is None:
            return None, f"missing Vellum SDK archive for {tag}; expected sha256 vellum.sdk_digest {digest}"
        archive_dir.mkdir(parents=True, exist_ok=True)
        candidate = archive_dir / archive_name(tag, platform)
        try:
            urllib.request.urlretrieve(url, candidate)
        except OSError as exc:
            return None, f"could not fetch Vellum SDK archive ({url}): {exc}; expected sha256 vellum.sdk_digest {digest}"
    if candidate is None or not candidate.is_file():
        return None, f"missing Vellum SDK archive for {tag}; expected sha256 vellum.sdk_digest {digest}"
    actual = hashlib.sha256(candidate.read_bytes()).hexdigest()
    if actual != digest:
        raise ValueError(
            f"Vellum SDK archive digest mismatch for vellum.sdk_digest: lock {digest}, file {actual}"
        )
    return candidate, digest


PUBLISH_PENDING = ".vellum-sdk-publish-pending"


def _publish_step() -> None:
    """Between the two publication renames; a test hook for interruption."""


def _canonical(name: str) -> Path:
    """Archive member name without "." segments or a leading "/"."""
    return Path(*[part for part in PurePosixPath(name).parts if part not in (".", "/")])


def _safe_extract(archive: Path, destination: Path) -> None:
    with tarfile.open(archive, "r:gz") as bundle:
        root = destination.resolve()
        members = bundle.getmembers()
        symlinks: set[Path] = set()
        for member in members:
            target = (destination / member.name).resolve()
            if target != root and root not in target.parents:
                raise ValueError(f"archive member escapes destination: {member.name}")
            if ".." in PurePosixPath(member.name).parts:
                # A ".." segment can step back through a symlink member that
                # the ancestor check below compares by canonical name.
                raise ValueError(f"archive member name is not canonical: {member.name}")
            parts = _canonical(member.name).parts
            if any(Path(*parts[:index]) in symlinks for index in range(1, len(parts))):
                raise ValueError(f"archive member follows symlink: {member.name}")
            if not (member.isreg() or member.isdir() or member.issym() or member.islnk()):
                # Python < 3.12 has no extraction filter, so refuse device
                # files and FIFOs here on every interpreter.
                raise ValueError(f"archive member is not a regular file, directory or link: {member.name}")
            if member.islnk():
                # Tar hardlink names are rooted at the archive extraction root.
                link = (destination / member.linkname).resolve()
                if link != root and root not in link.parents:
                    raise ValueError(f"archive link escapes destination: {member.name}")
                if ".." in PurePosixPath(member.linkname).parts:
                    raise ValueError(f"archive hardlink name is not canonical: {member.name}")
                if _canonical(member.linkname) in symlinks:
                    raise ValueError(f"archive hardlink targets a symlink: {member.name}")
            elif member.issym():
                link = (target.parent / member.linkname).resolve()
                if root not in link.parents and link != root:
                    raise ValueError(f"archive link escapes destination: {member.name}")
                symlinks.add(_canonical(member.name))
            if not member.issym():
                # Match the "data" filter on every interpreter: no setuid,
                # setgid or sticky bits, and no group/other write.
                member.mode &= 0o755
        if sys.version_info >= (3, 12):
            bundle.extractall(destination, members=members, filter="data")
        else:
            bundle.extractall(destination, members=members)


def _merge_notice(prefix: Path, vellum_root: Path) -> None:
    source = vellum_root / "sdk" / "share" / "doc" / "Vellum" / "NOTICE.md"
    if not source.is_file():
        raise ValueError("Vellum SDK archive has no sdk/share/doc/Vellum/NOTICE.md")
    notice = prefix / "NOTICE.md"
    existing = notice.read_text(encoding="utf-8") if notice.exists() else ""
    text = source.read_text(encoding="utf-8").strip()
    marker = "## Vellum SDK"
    if marker not in existing:
        if existing and not existing.endswith("\n"):
            existing += "\n"
        existing += f"\n{marker}\n\n{text}\n"
        notice.write_text(existing, encoding="utf-8")


def bundle(*, lock_path: Path, prefix: Path, platform: str, archive: Path | None,
           archive_dir: Path | None, integrations: Path, fetch: bool = False) -> Path:
    resolved, digest = resolve_archive(lock_path, platform, archive, archive_dir, fetch)
    if resolved is None:
        raise FileNotFoundError(digest)
    prefix.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pulp-vellum-stage-", dir=prefix.parent) as raw:
        unpacked = Path(raw) / "vellum"
        unpacked.mkdir()
        _safe_extract(resolved, unpacked)
        sdk_root = prefix / "share" / "pulp" / "vellum-sdk" / _lock(lock_path)[0].removeprefix("v")
        sdk_root.parent.mkdir(parents=True, exist_ok=True)
        integration_dest = prefix / "share" / "pulp" / "integrations"
        # Publication renames the integrations, then the SDK, then writes the
        # SDK digest marker that setup.sh treats as "staged". The pending
        # marker spans that window, so a run interrupted inside it is redone
        # here instead of being accepted (or refused) as a staged SDK.
        pending = prefix / "share" / "pulp" / PUBLISH_PENDING
        digest_marker = sdk_root / "pulp-sdk-digest.txt"
        with tempfile.TemporaryDirectory(prefix=f".{sdk_root.name}-", dir=sdk_root.parent) as staging:
            staging_root = Path(staging)
            if pending.exists() and not digest_marker.exists():
                for leftover, name in ((sdk_root, "stale-sdk"), (integration_dest, "stale-integrations")):
                    if leftover.exists() or leftover.is_symlink():
                        leftover.rename(staging_root / name)
                pending.unlink()
            if sdk_root.exists():
                raise FileExistsError(f"refusing to overwrite staged Vellum SDK: {sdk_root}")
            if integration_dest.exists():
                raise FileExistsError(f"refusing to overwrite staged integrations: {integration_dest}")
            staged_sdk = staging_root / sdk_root.name
            staged_integrations = staging_root / "integrations"
            shutil.copytree(unpacked, staged_sdk, symlinks=True)
            shutil.copytree(integrations, staged_integrations, symlinks=True)
            if (prefix / "NOTICE.md").exists():
                shutil.copy2(prefix / "NOTICE.md", staging_root / "NOTICE.md")
            _merge_notice(staging_root, staged_sdk)
            staged_notice = staging_root / "NOTICE.md"
            staged_digest = staging_root / "pulp-sdk-digest.txt"
            staged_digest.write_text(digest + "\n", encoding="utf-8")
            pending.write_text(sdk_root.name + "\n", encoding="utf-8")
            staged_integrations.rename(integration_dest)
            _publish_step()
            staged_sdk.rename(sdk_root)
            if staged_notice.exists():
                os.replace(staged_notice, prefix / "NOTICE.md")
            os.replace(staged_digest, digest_marker)
            pending.unlink()
    return sdk_root


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--archive-dir", type=Path)
    parser.add_argument("--fetch", action="store_true", help="fetch the lock-named release archive when absent")
    parser.add_argument("--integrations", type=Path, required=True)
    args = parser.parse_args()
    try:
        staged = bundle(lock_path=args.lock, prefix=args.prefix, platform=args.platform,
                        archive=args.archive, archive_dir=args.archive_dir,
                        integrations=args.integrations, fetch=args.fetch)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"bundle-vellum-sdk: {exc}", flush=True)
        return 1
    print(f"bundle-vellum-sdk: staged {staged}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
