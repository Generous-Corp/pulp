#!/usr/bin/env python3
"""Fetch the prebuilt Skia release asset for the release-cli workflow.

Reads the per-platform URL + sha256 from `tools/deps/manifest.json` and
unpacks the archive into `external/skia-build/` so that `FindSkia.cmake`
locates `libskia.a` at `external/skia-build/build/<platform>-gpu/lib/Release/`.

This step is the root fix for pulp #1817: prior to this script, the
release workflow shipped SDK tarballs with **zero** Skia binaries
because `.gitattributes` declared them LFS-tracked but they were never
committed. CMake's `FindSkia.cmake` therefore set `PULP_HAS_SKIA=FALSE`,
the entire `MacGpuWindowHost` translation unit was `#ifdef`'d out, and
every SDK consumer passing `use_gpu=true` silently fell back to the
CoreGraphics CPU path.

Usage:
    python3 tools/scripts/fetch_skia_for_release.py <matrix-platform>

Where `<matrix-platform>` is one of the release-cli.yml matrix values:
    darwin-arm64, linux-x64, linux-arm64, windows-x64, windows-arm64

If the manifest has no asset for the requested platform (e.g. linux-arm64,
windows-*), the script exits 0 with a message — those platforms keep
their current CG-only behavior until skia-builder publishes assets for
them. Platforms that DO have assets must succeed (sha256 verified +
expected library on disk) or the script exits non-zero.

This script intentionally avoids stderr-only output so the workflow log
shows progress on stdout for either bash or PowerShell.

### Skia-builder zip layout (pulp #1962)

Earlier skia-builder chrome/* releases unpacked libs flat under
`build/<plat>-gpu/lib/Release/libskia.a`. Starting in the chrome/m144
series (the pinned release for Pulp), libs ship under an **arch
subdirectory** — e.g. `build/mac-gpu/lib/Release/arm64/libskia.a` and
`build/linux-gpu/lib/Release/x64/libskia.a`. This regression caused
release-cli to fail for v0.95.0..v0.97.0 with `expected library not
found at <flat path> after unpack`, leaving every SDK/CLI release after
v0.94.0 unpublished.

Fix: after extracting, if the libraries are in an arch subdir, move
them up to the flat `Release/` directory so `FindSkia.cmake`'s existing
`${SKIA_DIR}/build/<plat>-gpu/lib/Release` probe (line 61) finds them
without further changes. This keeps the FindSkia surface stable for
both fresh release-cli unpacks AND existing local checkouts that were
arranged manually.
"""
from __future__ import annotations

import hashlib
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

# Matrix platform (release-cli.yml) → manifest release_assets key.
# Matrix uses `darwin-*` and `windows-*`; manifest uses `mac-*` and `win-*`.
MATRIX_TO_MANIFEST = {
    "darwin-arm64": "mac-arm64",
    "darwin-x64": "mac-x64",  # not currently in matrix; documented for parity
    "linux-x64": "linux-x64",
    "linux-arm64": "linux-arm64",
    "windows-x64": "win-x64",
    "windows-arm64": "win-arm64",
}

# Expected on-disk relative library path under external/skia-build/.
# Matches the layout produced by skia-builder release zips and probed by
# FindSkia.cmake's `${SKIA_DIR}/build/<plat>-gpu/lib/Release/` branch.
def expected_library_path(matrix_platform: str) -> Path:
    if matrix_platform.startswith("darwin"):
        plat_dir = "mac-gpu"
        lib_name = "libskia.a"
    elif matrix_platform.startswith("linux"):
        plat_dir = "linux-gpu"
        lib_name = "libskia.a"
    elif matrix_platform.startswith("windows"):
        plat_dir = "win-gpu"
        lib_name = "skia.lib"
    else:
        raise SystemExit(f"unknown matrix platform: {matrix_platform!r}")
    return Path("external/skia-build/build") / plat_dir / "lib" / "Release" / lib_name


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

    skia_entry = None
    for entry in manifest.get("dependencies", []):
        if isinstance(entry, dict) and entry.get("name", "").lower() == "skia":
            skia_entry = entry
            break
    if skia_entry is None:
        print("ERROR: no 'Skia' dependency entry in manifest.json", file=sys.stderr)
        return 1

    assets = (
        skia_entry.get("determinism", {})
        .get("release_assets", {})
    )
    asset = assets.get(manifest_key)
    if asset is None:
        # Not every release-cli matrix platform has a published skia-builder
        # asset. linux-arm64 and windows-* are not currently published —
        # they keep their existing CG-only behavior. Exit 0 so the workflow
        # step succeeds; PULP_REQUIRE_GPU_FOR_SDK must NOT be set for these
        # platforms (release-cli.yml gates it appropriately).
        print(
            f"INFO: no Skia release asset for matrix={matrix_platform} "
            f"(manifest key {manifest_key!r}); skipping fetch — this "
            f"platform will continue to build without GPU support."
        )
        return 0

    url = asset["url"]
    expected_sha = asset["sha256"]
    expected_lib = expected_library_path(matrix_platform)

    print(f"Skia fetch: matrix={matrix_platform}, manifest={manifest_key}")
    print(f"  url: {url}")
    print(f"  sha256: {expected_sha}")
    print(f"  expected lib: {expected_lib}")

    zip_path = Path("skia-release-asset.zip")
    print(f"Downloading → {zip_path}")
    with urllib.request.urlopen(url) as resp, zip_path.open("wb") as fp:
        # 1 MiB chunks; skia zips are ~250-500 MiB
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
        return 1
    print(f"sha256 verified: {actual_sha}")

    dest = Path("external/skia-build")
    dest.mkdir(parents=True, exist_ok=True)
    print(f"Unpacking → {dest}")
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(dest)

    # Free the ~hundreds-of-MiB download once unpacked.
    zip_path.unlink(missing_ok=True)

    # Normalize skia-builder chrome/m144+ layout (pulp #1962).
    #
    # The expected library lives at:
    #   external/skia-build/build/<plat>-gpu/lib/Release/<lib>
    # but chrome/m144 zips place it one directory deeper, under an arch
    # subdir:
    #   external/skia-build/build/<plat>-gpu/lib/Release/<arch>/<lib>
    # Flatten that arch directory into Release/ so FindSkia.cmake's
    # existing layout probe matches without further changes.
    if not expected_lib.is_file():
        release_dir = expected_lib.parent
        if release_dir.is_dir():
            arch_subdirs = [p for p in release_dir.iterdir() if p.is_dir()]
            # Walk only direct subdirs of Release/ — the upstream layout
            # is exactly one arch level deep. Move every regular file
            # inside up to Release/, then drop the now-empty arch dir.
            for arch_dir in arch_subdirs:
                moved = 0
                for item in arch_dir.iterdir():
                    if item.is_file():
                        target = release_dir / item.name
                        # Refuse to clobber an already-flat duplicate.
                        if target.exists():
                            continue
                        item.rename(target)
                        moved += 1
                if moved:
                    print(
                        f"Flattened skia-builder arch layout: moved "
                        f"{moved} file(s) from {arch_dir} → {release_dir}"
                    )
                # rmdir succeeds only if empty; that's exactly what we want.
                try:
                    arch_dir.rmdir()
                except OSError:
                    # Non-empty (e.g. obj/ subfolder) — leave in place.
                    pass

    # Sanity check: the expected library MUST be on disk now. If not, the
    # zip layout drifted from FindSkia.cmake's expectations and the SDK
    # build will silently fall back to no-Skia again — exactly the bug
    # this script exists to prevent.
    if not expected_lib.is_file():
        print(
            f"ERROR: expected library not found at {expected_lib} after unpack",
            file=sys.stderr,
        )
        # Help the human debug.
        print("Contents of external/skia-build/ (depth 3):", file=sys.stderr)
        for p in sorted(dest.rglob("*"))[:50]:
            print(f"  {p}", file=sys.stderr)
        return 1

    print(f"OK: {expected_lib} present ({expected_lib.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
