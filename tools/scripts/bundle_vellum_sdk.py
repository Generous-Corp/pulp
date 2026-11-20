#!/usr/bin/env python3
"""Verify and stage the lock-pinned darwin-arm64 Vellum SDK.

Release wiring and the distribution guard come next. Nothing in Pulp's CMake
reads ``share/pulp/vellum-sdk`` yet.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tarfile
import tempfile
import urllib.request
from pathlib import Path

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


def _safe_extract(archive: Path, destination: Path) -> None:
    with tarfile.open(archive, "r:gz") as bundle:
        root = destination.resolve()
        for member in bundle.getmembers():
            target = (destination / member.name).resolve()
            if target != root and root not in target.parents:
                raise ValueError(f"archive member escapes destination: {member.name}")
            if member.issym() or member.islnk():
                link = (target.parent / member.linkname).resolve()
                if root not in link.parents and link != root:
                    raise ValueError(f"archive link escapes destination: {member.name}")
        bundle.extractall(destination)


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
        if sdk_root.exists():
            raise FileExistsError(f"refusing to overwrite staged Vellum SDK: {sdk_root}")
        sdk_root.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(unpacked, sdk_root, symlinks=True)
        integration_dest = prefix / "share" / "pulp" / "integrations"
        if integration_dest.exists():
            raise FileExistsError(f"refusing to overwrite staged integrations: {integration_dest}")
        shutil.copytree(integrations, integration_dest, symlinks=True)
        _merge_notice(prefix, sdk_root)
        (sdk_root / "pulp-sdk-digest.txt").write_text(digest + "\n", encoding="utf-8")
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
