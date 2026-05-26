#!/usr/bin/env python3
"""Download + sha256-verify the third-party plugin fixtures used by the
real-plugin integration suite (item 4.2 of the macOS plugin authoring
plan).

Reads the manifest at `test/integration/real_plugins.toml` (the same one
the runner reads), then for each entry whose `platforms.<host_os>` block
has a real sha256 (i.e. not `"TBD"`):

    1. Download the URL to a tmp file
    2. Verify sha256 — abort if mismatched (refuse to unpack)
    3. Unpack into `~/.cache/pulp/real-plugins/<id>/`
    4. Verify the expected `bundle_relpath` ended up on disk

Skips entries whose sha256 is still `TBD` with a clear log line so it's
obvious the manifest needs filling in. **Never** writes anything to the
system plugin folders — the cache root is intentionally separate so
running this script doesn't pollute a developer's DAW scan path.

Usage:
    python3 tools/scripts/fetch_real_plugins.py            # all entries
    python3 tools/scripts/fetch_real_plugins.py surge-xt   # one entry
    python3 tools/scripts/fetch_real_plugins.py --check    # don't fetch,
                                                           # just report
                                                           # cache status

This script is a SCAFFOLD: the `_unpack_*` helpers cover .zip / .tar.gz
/ raw archives. .dmg unpack on macOS uses `hdiutil`, but Linux/Windows
will skip a .dmg with a clear message. Vendor-installer .pkg/.exe are
intentionally unsupported — those plugins should publish a .zip lane
for CI, or be loaded from a developer-installed location via the env
override the runner already respects.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "test" / "integration" / "real_plugins.toml"

# Try Python 3.11+ tomllib first, then fall back to tomli for older Pythons.
try:
    import tomllib  # type: ignore[import-not-found]
except ModuleNotFoundError:  # pragma: no cover — only on Python <3.11
    try:
        import tomli as tomllib  # type: ignore[import-not-found,no-redef]
    except ModuleNotFoundError:
        sys.exit(
            "fetch_real_plugins: neither `tomllib` (Python 3.11+) nor `tomli` "
            "is available. Run on Python 3.11+ or `pip install tomli`."
        )


def host_os() -> str:
    """Return the manifest's per-OS key for the running platform."""
    sys_name = platform.system()
    if sys_name == "Darwin":
        return "macos"
    if sys_name == "Windows":
        return "windows"
    return "linux"


def cache_root() -> Path:
    """Mirror `cache_root()` in real_plugin_runner.cpp — both must agree."""
    if env := os.environ.get("PULP_REAL_PLUGIN_CACHE"):
        return Path(env)
    if host_os() == "windows":
        if appdata := os.environ.get("LOCALAPPDATA"):
            return Path(appdata) / "pulp" / "real-plugins"
    if home := os.environ.get("HOME"):
        return Path(home) / ".cache" / "pulp" / "real-plugins"
    return Path(tempfile.gettempdir()) / "pulp-real-plugins"


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: Path) -> None:
    print(f"  fetching {url}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Plain urllib — no third-party HTTP client to keep the scaffold
    # self-contained. The sha256 verification step is the security gate;
    # if the URL is hijacked, the hash mismatch refuses the unpack.
    with urllib.request.urlopen(url) as resp, dest.open("wb") as out:  # noqa: S310
        shutil.copyfileobj(resp, out)


def unpack_zip(archive: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(target)


def unpack_tar_gz(archive: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as t:
        # Python 3.12 deprecates the default extraction filter; honor the
        # safer 'data' filter when it's available.
        kwargs = {}
        if hasattr(tarfile, "data_filter"):
            kwargs["filter"] = "data"
        t.extractall(target, **kwargs)


def unpack_dmg(archive: Path, target: Path) -> None:
    if host_os() != "macos":
        raise RuntimeError(".dmg unpack is only supported on macOS")
    target.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pulp-dmg-") as mnt:
        subprocess.run(
            ["hdiutil", "attach", "-quiet", "-nobrowse",
             "-mountpoint", mnt, str(archive)],
            check=True,
        )
        try:
            # Copy everything inside the dmg into the cache. Caller's
            # bundle_relpath narrows down to the actual plugin afterwards.
            for entry in Path(mnt).iterdir():
                d = target / entry.name
                if entry.is_dir():
                    shutil.copytree(entry, d, dirs_exist_ok=True)
                else:
                    shutil.copy2(entry, d)
        finally:
            subprocess.run(["hdiutil", "detach", "-quiet", mnt], check=False)


def unpack_raw(archive: Path, target: Path, dest_name: str) -> None:
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(archive, target / dest_name)


def unpack(archive: Path, kind: str, target: Path, dest_name: str) -> None:
    if kind == "zip":
        unpack_zip(archive, target)
    elif kind in ("tar.gz", "tgz"):
        unpack_tar_gz(archive, target)
    elif kind == "dmg":
        unpack_dmg(archive, target)
    elif kind == "raw":
        unpack_raw(archive, target, dest_name)
    else:
        raise RuntimeError(f"unsupported archive_kind={kind!r}")


def process_entry(entry: dict, check_only: bool) -> int:
    plugin_id = entry["id"]
    platforms = entry.get("platforms", {})
    plat = platforms.get(host_os())
    if not plat:
        print(f"[{plugin_id}] no platform entry for {host_os()} — skipping")
        return 0

    sha = plat.get("sha256", "")
    if sha in ("", "TBD"):
        print(f"[{plugin_id}] sha256 placeholder — fixture not pinned yet")
        return 0

    target_dir = cache_root() / plugin_id
    bundle_path = target_dir / entry["bundle_relpath"]

    if bundle_path.exists():
        print(f"[{plugin_id}] already present at {bundle_path}")
        return 0

    if check_only:
        print(f"[{plugin_id}] MISSING — would fetch {plat['url']}")
        return 0

    print(f"[{plugin_id}] downloading…")
    with tempfile.TemporaryDirectory(prefix=f"pulp-{plugin_id}-") as tmp:
        tmp_path = Path(tmp) / "archive"
        try:
            download(plat["url"], tmp_path)
        except Exception as e:
            print(f"[{plugin_id}] download FAILED: {e}", file=sys.stderr)
            return 1

        got = sha256_of(tmp_path)
        if got != sha:
            print(
                f"[{plugin_id}] sha256 MISMATCH\n"
                f"  expected: {sha}\n"
                f"  got:      {got}\n"
                f"  refusing to unpack",
                file=sys.stderr,
            )
            return 1

        try:
            unpack(tmp_path, plat.get("archive_kind", "zip"), target_dir,
                   dest_name=Path(entry["bundle_relpath"]).name)
        except Exception as e:
            print(f"[{plugin_id}] unpack FAILED: {e}", file=sys.stderr)
            return 1

    if not bundle_path.exists():
        print(
            f"[{plugin_id}] unpacked archive does not contain expected bundle "
            f"at {entry['bundle_relpath']}",
            file=sys.stderr,
        )
        return 1

    print(f"[{plugin_id}] OK → {bundle_path}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ids", nargs="*",
                    help="Plugin ids to fetch (default: all in manifest).")
    ap.add_argument("--check", action="store_true",
                    help="Report cache status without downloading.")
    ap.add_argument("--manifest", type=Path, default=MANIFEST,
                    help=f"Manifest path (default: {MANIFEST}).")
    args = ap.parse_args()

    if not args.manifest.exists():
        print(f"manifest not found: {args.manifest}", file=sys.stderr)
        return 2

    with args.manifest.open("rb") as f:
        data = tomllib.load(f)

    entries: list[dict] = data.get("plugins", [])
    if args.ids:
        wanted = set(args.ids)
        entries = [e for e in entries if e.get("id") in wanted]
        missing = wanted - {e.get("id") for e in entries}
        if missing:
            print(f"unknown ids: {sorted(missing)}", file=sys.stderr)
            return 2

    rc = 0
    for e in entries:
        rc |= process_entry(e, check_only=args.check)
    return rc


if __name__ == "__main__":
    sys.exit(main())
