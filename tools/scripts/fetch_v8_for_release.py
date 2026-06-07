#!/usr/bin/env python3
"""Fetch the prebuilt, sealed V8 release asset for a given platform.

Reads the per-platform URL + sha256 from `tools/deps/manifest.json` (the
`V8` dependency's `determinism.release_assets`) and unpacks the archive
into `external/v8-build/<manifest-key>/` so that `FindV8.cmake` locates
the headers + library when `PULP_JS_ENGINE=v8` is selected.

This is the V8 analog of `fetch_skia_for_release.py`: V8 is an optional
JS engine backend (default is QuickJS; JSC on Apple). When selected, the
provider is the pinned sealed `libv8` from the danielraffel/v8-builder
fork — NOT a developer's Homebrew `libnode`. Each platform zip contains
`include/` plus a platform-appropriate library:

    mac-*      lib/libv8.dylib        (@rpath/libv8.dylib)
    linux-*    lib/libv8.so
    win-*      lib/v8.dll + lib/v8.dll.lib (MSVC import lib)
    android-*  jniLibs/arm64-v8a/libv8.so
    ios-sim    headers only (no library — iOS is JSC-only; V8 needs JIT)

Usage:
    python3 tools/scripts/fetch_v8_for_release.py <matrix-platform>

Where `<matrix-platform>` is a release/CI matrix value:
    darwin-arm64, darwin-x64, linux-x64, linux-arm64,
    windows-x64, windows-arm64, android-arm64, ios-simulator-arm64

If the manifest has no asset for the requested platform, the script
exits 0 with a message. The iOS simulator asset is `library: false`
(headers only) — it extracts and succeeds without a library check.

Avoids stderr-only output so the workflow log shows progress on stdout
for either bash or PowerShell.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

# Matrix platform → manifest release_assets key. Matrix uses `darwin-*`
# and `windows-*`; the manifest uses `mac-*` and `win-*`.
MATRIX_TO_MANIFEST = {
    "darwin-arm64": "mac-arm64",
    "darwin-x64": "mac-x86_64",
    "linux-x64": "linux-x64",
    "linux-arm64": "linux-arm64",
    "windows-x64": "win-x64",
    "windows-arm64": "win-arm64",
    "android-arm64": "android-arm64",
    "ios-simulator-arm64": "ios-simulator-arm64",
}

DEST_ROOT = Path("external/v8-build")


def expected_library_path(manifest_key: str) -> Path | None:
    """On-disk relative library path under external/v8-build/, or None for
    header-only assets (iOS simulator). Mirrors the per-platform layout the
    v8-builder zips ship and `FindV8.cmake` probes."""
    base = DEST_ROOT / manifest_key
    if manifest_key.startswith("mac-"):
        return base / "lib" / "libv8.dylib"
    if manifest_key.startswith("linux-"):
        return base / "lib" / "libv8.so"
    if manifest_key.startswith("win-"):
        # The DLL is the runtime; the import lib (v8.dll.lib) is what the
        # MSVC linker consumes. Check the import lib — its absence is the
        # failure that actually breaks a Windows link.
        return base / "lib" / "v8.dll.lib"
    if manifest_key.startswith("android-"):
        return base / "jniLibs" / "arm64-v8a" / "libv8.so"
    if manifest_key.startswith("ios-"):
        return None  # headers only — iOS is JSC-only (V8 needs JIT)
    raise SystemExit(f"unknown manifest key: {manifest_key!r}")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <matrix-platform>", file=sys.stderr)
        return 2
    matrix_platform = argv[1]

    if matrix_platform not in MATRIX_TO_MANIFEST:
        print(
            f"WARNING: unknown matrix platform {matrix_platform!r}; "
            f"known values are {sorted(MATRIX_TO_MANIFEST)}",
            file=sys.stderr,
        )
        return 0

    manifest_key = MATRIX_TO_MANIFEST[matrix_platform]
    manifest_path = Path("tools/deps/manifest.json")
    if not manifest_path.is_file():
        print(f"ERROR: {manifest_path} not found (run from repo root)", file=sys.stderr)
        return 1
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    v8_entry = None
    for entry in manifest.get("dependencies", []):
        if isinstance(entry, dict) and entry.get("name", "").lower() == "v8":
            v8_entry = entry
            break
    if v8_entry is None:
        print("ERROR: no 'V8' dependency entry in manifest.json", file=sys.stderr)
        return 1

    assets = v8_entry.get("determinism", {}).get("release_assets", {})
    asset = assets.get(manifest_key)
    if asset is None:
        print(
            f"INFO: no V8 release asset for matrix={matrix_platform} "
            f"(manifest key {manifest_key!r}); skipping fetch — this "
            f"platform will not have the sealed V8 provider."
        )
        return 0

    url = asset["url"]
    expected_sha = asset["sha256"]
    has_library = asset.get("library", True)
    expected_lib = expected_library_path(manifest_key) if has_library else None
    dest = DEST_ROOT / manifest_key
    stamp_path = dest / ".v8-asset-sha256"

    print(f"V8 fetch: matrix={matrix_platform}, manifest={manifest_key}")
    print(f"  url: {url}")
    print(f"  sha256: {expected_sha}")
    print(f"  dest: {dest}")
    if expected_lib is not None:
        print(f"  expected lib: {expected_lib}")
    else:
        print("  (header-only asset — no library expected)")

    # Opt-in baked-V8 short-circuit (PULP_USE_BAKED_V8 — set ONLY by the Tart
    # VM golden, never by releases/clean runners). The golden bakes V8 at
    # $V8_DIR; skip the download only when the baked stamp matches the pin, so
    # a pin bump on a not-yet-rebaked golden falls through to a normal fetch.
    baked_dir = os.environ.get("V8_DIR", "").strip()
    if os.environ.get("PULP_USE_BAKED_V8") and baked_dir:
        baked_root = Path(baked_dir) / manifest_key
        baked_stamp = baked_root / ".v8-asset-sha256"
        baked_ok = (
            baked_stamp.is_file()
            and baked_stamp.read_text(encoding="utf-8").strip() == expected_sha
            and (expected_lib is None
                 or (baked_root / expected_lib.relative_to(dest)).is_file())
        )
        if baked_ok:
            print(
                f"OK: using baked V8 at {baked_root} "
                f"(sha256 {expected_sha} matches pin); skipping fetch "
                f"(PULP_USE_BAKED_V8)"
            )
            return 0
        print(
            "PULP_USE_BAKED_V8 set but baked V8 is missing or stale "
            "(stamp != current pin) — re-fetching the pinned asset."
        )

    # Idempotency stamp: skip the download when the already-unpacked asset
    # matches the current pin. A pin bump changes expected_sha, the stamp no
    # longer matches, and the asset is re-fetched — never silently stale.
    lib_present = expected_lib is None or expected_lib.is_file()
    if lib_present and stamp_path.is_file():
        if stamp_path.read_text(encoding="utf-8").strip() == expected_sha:
            print(
                f"OK: V8 already unpacked from the pinned asset "
                f"(sha256 {expected_sha}); skipping download"
            )
            return 0
        print(
            "V8 stamp does not match the pinned asset — the manifest pin "
            "changed since the last fetch; re-downloading."
        )

    zip_path = Path(f"v8-release-asset-{manifest_key}.zip")
    print(f"Downloading → {zip_path}")
    with urllib.request.urlopen(url) as resp, zip_path.open("wb") as fp:
        while True:
            chunk = resp.read(1024 * 1024)
            if not chunk:
                break
            fp.write(chunk)

    # Verify sha256 BEFORE unpacking.
    h = hashlib.sha256()
    with zip_path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    actual_sha = h.hexdigest()
    if actual_sha != expected_sha:
        print(
            f"ERROR: sha256 mismatch\n  expected: {expected_sha}\n  actual:   {actual_sha}",
            file=sys.stderr,
        )
        zip_path.unlink(missing_ok=True)
        return 1
    print(f"sha256 verified: {actual_sha}")

    # Unpack into a clean per-platform dir so a pin bump never leaves stale
    # files behind alongside the new ones.
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)
    print(f"Unpacking → {dest}")
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(dest)
    zip_path.unlink(missing_ok=True)

    # Sanity check: the expected library MUST be present (except header-only).
    if expected_lib is not None and not expected_lib.is_file():
        print(
            f"ERROR: expected library not found at {expected_lib} after unpack",
            file=sys.stderr,
        )
        print(f"Contents of {dest}/ (depth 3):", file=sys.stderr)
        for p in sorted(dest.rglob("*"))[:50]:
            print(f"  {p}", file=sys.stderr)
        return 1

    stamp_path.write_text(expected_sha + "\n", encoding="utf-8")

    if expected_lib is not None:
        print(f"OK: {expected_lib} present ({expected_lib.stat().st_size:,} bytes)")
    else:
        hdr = dest / "include" / "v8.h"
        print(f"OK: headers unpacked at {dest}/include "
              f"(v8.h {'present' if hdr.is_file() else 'MISSING'})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
