#!/usr/bin/env python3
"""Fetch the prebuilt, sealed V8 release asset for a given platform.

Reads the per-platform URL + sha256 from `tools/deps/manifest.json` (the
`V8` dependency's `determinism.release_assets`) and unpacks the archive
into `external/v8-build/<manifest-key>/` so that `FindV8.cmake` locates
the headers + library when `PULP_JS_ENGINE=v8` is selected.

This is the V8 analog of `fetch_skia_for_release.py`: V8 is an optional
JS engine backend (default is QuickJS; JSC is opt-in on Apple). When selected, the
provider is the pinned sealed `libv8` from the danielraffel/v8-builder
fork — NOT a developer's Homebrew `libnode`. Each platform zip contains
`include/` plus a platform-appropriate library:

    mac-*      lib/libv8.dylib        (@rpath/libv8.dylib)
    linux-*    lib/libv8.so
    win-*      lib/v8.dll + lib/v8.dll.lib (MSVC import lib)
    android-*  jniLibs/arm64-v8a/libv8.so
    ios-sim    V8.framework (provenance/header validation only; runtime forbidden)

Usage:
    python3 tools/scripts/fetch_v8_for_release.py <matrix-platform>

Where `<matrix-platform>` is a release/CI matrix value:
    darwin-arm64, darwin-x64, linux-x64, linux-arm64,
    windows-x64, windows-arm64, android-arm64, ios-simulator-arm64

If the manifest has no asset for the requested platform, the script
exits 0 with a message. The iOS simulator asset is `library: false`
The iOS simulator release contains a jitless framework, but Pulp deliberately
marks it `library: false`: it is fetched only to validate the published provider
and headers, and is never selectable as an iOS runtime backend.

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
import urllib.parse
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

PAIR_FIELD_MAP = {
    "pair_kind": "pair_kind",
    "milestone": "milestone",
    "built_revision": "built_revision",
    "chromium_revision": "chromium_revision",
    "chromium_deps_blob": "chromium_deps_blob",
    "chromium_branch": "chromium_branch",
    "chromium_skia": "skia",
    "paired_skia": "built_skia",
    "skia_matches_chromium": "skia_matches_chromium",
    "paired_dawn": "built_dawn",
    "chromium_dawn": "dawn",
    "dawn_matches_chromium": "dawn_matches_chromium",
    "skia_release_tag": "validated_skia_release",
}

EMBEDDED_ARTIFACT_IDENTITY = {
    "android-arm64": {"platform": "android", "arch": "arm64",
        "lib": "jniLibs/arm64-v8a/libv8.so", "shared": True, "sealed": True,
        "i18n": True, "abi": "arm64-v8a", "ndk_api_level": 29,
        "libcxx": "bundled-chromium-__Cr"},
    "ios-simulator-arm64": {"platform": "ios", "arch": "arm64",
        "lib": "V8.framework/V8", "shared": True, "sealed": True, "i18n": False,
        "form": "framework", "deployment_target": "16.4",
        "ios_environment": "simulator", "jitless": True, "wasm": False},
    "linux-arm64": {"platform": "linux", "arch": "arm64",
        "lib": "lib/libv8.so", "shared": True, "sealed": True, "i18n": True},
    "linux-x64": {"platform": "linux", "arch": "x64",
        "lib": "lib/libv8.so", "shared": True, "sealed": True, "i18n": True},
    "mac-arm64": {"platform": "mac", "arch": "arm64",
        "lib": "lib/libv8.dylib", "shared": True, "sealed": True, "i18n": True},
    "mac-x86_64": {"platform": "mac", "arch": "x86_64",
        "lib": "lib/libv8.dylib", "shared": True, "sealed": True, "i18n": True},
    "win-arm64": {"platform": "win", "arch": "arm64", "lib": "lib/v8.dll",
        "import_lib": "lib/v8.dll.lib", "shared": True, "sealed": True, "i18n": True,
        "libcxx": "bundled-chromium-__Cr", "libcxx_lib": "lib/libc++.lib"},
    "win-x64": {"platform": "win", "arch": "x64", "lib": "lib/v8.dll",
        "import_lib": "lib/v8.dll.lib", "shared": True, "sealed": True, "i18n": True,
        "libcxx": "bundled-chromium-__Cr", "libcxx_lib": "lib/libc++.lib"},
}


def validate_embedded_payload(embedded: dict, manifest: dict,
                              v8_entry: dict, manifest_key: str) -> None:
    """Bind the sealed archive's own provenance to Pulp's active provider.

    An outer archive digest proves identity, but without this check a future
    update can consistently pin the wrong milestone asset. Matched-milestone
    archives must carry the v8-builder manifest and agree with every recorded
    pair field plus Pulp's active Skia/built-Dawn generation.
    """
    determinism = v8_entry.get("determinism", {})
    if determinism.get("pair_kind") != "chromium-milestone":
        return
    pair = embedded.get("pair")
    if not isinstance(pair, dict):
        raise ValueError("matched V8 archive manifest has no pair object")
    mismatches = []
    for pinned_name, embedded_name in PAIR_FIELD_MAP.items():
        pinned = determinism.get(pinned_name)
        actual = pair.get(embedded_name)
        if pinned != actual:
            mismatches.append(f"{embedded_name}: embedded={actual!r}, pinned={pinned!r}")

    version_parts = str(v8_entry.get("version", "")).split("-")
    pinned_runtime = version_parts[2] if len(version_parts) >= 4 else None
    if embedded.get("v8_version") != pinned_runtime:
        mismatches.append(
            f"v8_version: embedded={embedded.get('v8_version')!r}, "
            f"pinned={pinned_runtime!r}"
        )
    for field, expected in EMBEDDED_ARTIFACT_IDENTITY[manifest_key].items():
        if embedded.get(field) != expected:
            mismatches.append(
                f"artifact {field}: embedded={embedded.get(field)!r}, expected={expected!r}"
            )

    skia_entry = next(
        (entry for entry in manifest.get("dependencies", [])
         if isinstance(entry, dict) and entry.get("name") == "Skia"),
        None,
    )
    if skia_entry is None:
        mismatches.append("active Pulp Skia entry is missing")
    else:
        skia_det = skia_entry.get("determinism", {})
        active_pairs = {
            "validated_skia_release": skia_entry.get("version"),
            "built_skia": skia_det.get("skia_commit"),
            "built_dawn": skia_det.get("built_dawn"),
        }
        for name, expected in active_pairs.items():
            if pair.get(name) != expected:
                mismatches.append(
                    f"active {name}: embedded={pair.get(name)!r}, Pulp={expected!r}"
                )

    expected_pair_sha = determinism.get("pair_manifest_sha256")
    actual_pair_sha = hashlib.sha256(
        json.dumps(pair, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    if actual_pair_sha != expected_pair_sha:
        mismatches.append(
            f"pair manifest SHA-256: embedded={actual_pair_sha}, "
            f"pinned={expected_pair_sha!r}"
        )
    if mismatches:
        raise ValueError("embedded V8 provenance mismatch: " + "; ".join(mismatches))


def validate_embedded_manifest(zf: zipfile.ZipFile, manifest: dict,
                               v8_entry: dict, manifest_key: str) -> None:
    try:
        embedded = json.loads(zf.read("manifest.json"))
    except KeyError as exc:
        raise ValueError("matched V8 archive has no embedded manifest.json") from exc
    except json.JSONDecodeError as exc:
        raise ValueError("matched V8 archive manifest.json is invalid JSON") from exc
    validate_embedded_payload(embedded, manifest, v8_entry, manifest_key)


def validate_materialized_manifest(root: Path, manifest: dict,
                                   v8_entry: dict, manifest_key: str) -> None:
    path = root / "manifest.json"
    try:
        embedded = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError("materialized matched V8 provider has no manifest.json") from exc
    except json.JSONDecodeError as exc:
        raise ValueError("materialized matched V8 manifest.json is invalid JSON") from exc
    validate_embedded_payload(embedded, manifest, v8_entry, manifest_key)


def expected_header_path(manifest_key: str) -> Path:
    base = DEST_ROOT / manifest_key
    if manifest_key == "ios-simulator-arm64":
        return base / "V8.framework" / "Headers" / "v8.h"
    return base / "include" / "v8.h"


def required_materialized_paths(root: Path, manifest_key: str,
                                matched_milestone: bool) -> list[Path]:
    header = (Path("V8.framework/Headers/v8.h")
              if manifest_key == "ios-simulator-arm64" else Path("include/v8.h"))
    paths = [root / header]
    if matched_milestone:
        identity = EMBEDDED_ARTIFACT_IDENTITY[manifest_key]
        paths.append(root / str(identity["lib"]))
        if manifest_key.startswith("win-"):
            paths.extend([root / "lib/v8.dll.lib", root / "lib/libc++.lib"])
    else:
        expected = expected_library_path(manifest_key)
        if expected is not None:
            paths.append(root / expected.relative_to(DEST_ROOT / manifest_key))
    return paths


def verify_release_metadata(v8_entry: dict, asset_url: str) -> str | None:
    determinism = v8_entry.get("determinism", {})
    if determinism.get("pair_kind") != "chromium-milestone":
        return None
    url = determinism.get("release_metadata_url")
    expected_sha = determinism.get("release_metadata_sha256")
    if not isinstance(url, str) or not isinstance(expected_sha, str):
        raise ValueError("matched V8 pin has no release metadata URL/SHA-256")
    with urllib.request.urlopen(url) as response:
        payload = response.read()
    actual_sha = hashlib.sha256(payload).hexdigest()
    if actual_sha != expected_sha:
        raise ValueError(
            f"V8 release metadata SHA-256 mismatch: actual={actual_sha}, "
            f"pinned={expected_sha}"
        )
    try:
        metadata = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise ValueError("V8 release metadata is invalid JSON") from exc
    asset_name = Path(urllib.parse.urlparse(asset_url).path).name
    expected_assets = {
        Path(urllib.parse.urlparse(str(info.get("url", ""))).path).name
        for info in determinism.get("release_assets", {}).values()
    }
    actual_assets = set(metadata.get("assets", []))
    if actual_assets != expected_assets:
        raise ValueError(
            f"V8 release metadata asset inventory mismatch: "
            f"actual={sorted(actual_assets)!r}, expected={sorted(expected_assets)!r}"
        )
    if asset_name not in actual_assets:
        raise ValueError(f"V8 release metadata does not list asset {asset_name!r}")
    pair_sha = hashlib.sha256(
        json.dumps(metadata.get("pair"), sort_keys=True,
                   separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    if pair_sha != determinism.get("pair_manifest_sha256"):
        raise ValueError(
            f"V8 release metadata pair mismatch: actual={pair_sha}, "
            f"pinned={determinism.get('pair_manifest_sha256')!r}"
        )
    if metadata.get("schema") != 1:
        raise ValueError(f"unsupported V8 release metadata schema {metadata.get('schema')!r}")
    expected_rows = {
        (identity["platform"], identity["arch"])
        for identity in EMBEDDED_ARTIFACT_IDENTITY.values()
    }
    manifests = metadata.get("manifests", [])
    actual_rows = {
        (row.get("platform"), row.get("arch"))
        for row in manifests if isinstance(row, dict)
    }
    if actual_rows != expected_rows or len(manifests) != len(expected_rows):
        raise ValueError(
            f"V8 release metadata manifest inventory mismatch: "
            f"actual={sorted(actual_rows)!r}, expected={sorted(expected_rows)!r}"
        )
    if any(row.get("pair") != metadata.get("pair") for row in manifests):
        raise ValueError("V8 release metadata manifest rows do not share one exact pair")
    return actual_sha


def expected_library_path(manifest_key: str) -> Path | None:
    """On-disk relative library path under external/v8-build/, or None for
    deliberately runtime-disabled assets (iOS simulator). Mirrors the per-platform layout the
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
        return None  # validate framework provenance/headers, but never select its runtime
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
    matched_milestone = (
        v8_entry.get("determinism", {}).get("pair_kind") == "chromium-milestone"
    )
    has_library = asset.get("library", True)
    expected_lib = expected_library_path(manifest_key) if has_library else None
    dest = DEST_ROOT / manifest_key
    stamp_path = dest / ".v8-asset-sha256"
    metadata_stamp_path = dest / ".v8-release-metadata-sha256"
    required_dest_paths = required_materialized_paths(
        dest, manifest_key, matched_milestone)

    print(f"V8 fetch: matrix={matrix_platform}, manifest={manifest_key}")
    print(f"  url: {url}")
    print(f"  sha256: {expected_sha}")
    print(f"  dest: {dest}")
    if expected_lib is not None:
        print(f"  expected lib: {expected_lib}")
    else:
        print("  (iOS framework runtime intentionally disabled; headers/provenance required)")

    # Opt-in baked-V8 short-circuit (PULP_USE_BAKED_V8 — set ONLY by the Tart
    # VM golden, never by releases/clean runners). The golden bakes V8 at
    # $V8_DIR; skip the download only when the baked stamp matches the pin, so
    # a pin bump on a not-yet-rebaked golden falls through to a normal fetch.
    baked_dir = os.environ.get("V8_DIR", "").strip()
    if os.environ.get("PULP_USE_BAKED_V8") and baked_dir:
        baked_root = Path(baked_dir) / manifest_key
        baked_stamp = baked_root / ".v8-asset-sha256"
        baked_metadata_stamp = baked_root / ".v8-release-metadata-sha256"
        baked_ok = (
            baked_stamp.is_file()
            and baked_stamp.read_text(encoding="utf-8").strip() == expected_sha
            and (not matched_milestone or (
                baked_metadata_stamp.is_file()
                and baked_metadata_stamp.read_text(encoding="utf-8").strip()
                    == v8_entry.get("determinism", {}).get("release_metadata_sha256")
            ))
            and all(path.is_file() for path in required_materialized_paths(
                baked_root, manifest_key, matched_milestone))
        )
        if baked_ok:
            try:
                if matched_milestone:
                    validate_materialized_manifest(
                        baked_root, manifest, v8_entry, manifest_key)
            except ValueError as error:
                print(f"Baked V8 provenance is stale: {error}; re-fetching.")
            else:
                print(
                    f"OK: using baked V8 at {baked_root} "
                    f"(sha256 {expected_sha} and embedded provenance match pin); "
                    f"skipping fetch (PULP_USE_BAKED_V8)"
                )
                return 0
        print(
            "PULP_USE_BAKED_V8 set but baked V8 is missing or stale "
            "(receipt, provenance, header, or required binary mismatch) — "
            "re-fetching the pinned asset."
        )

    # Idempotency stamp: skip the download when the already-unpacked asset
    # matches the current pin. A pin bump changes expected_sha, the stamp no
    # longer matches, and the asset is re-fetched — never silently stale.
    payload_present = all(path.is_file() for path in required_dest_paths)
    if payload_present and stamp_path.is_file():
        metadata_stamp_ok = (
            not matched_milestone
            or (
                metadata_stamp_path.is_file()
                and metadata_stamp_path.read_text(encoding="utf-8").strip()
                    == v8_entry.get("determinism", {}).get("release_metadata_sha256")
            )
        )
        if (stamp_path.read_text(encoding="utf-8").strip() == expected_sha
                and metadata_stamp_ok):
            try:
                if matched_milestone:
                    validate_materialized_manifest(dest, manifest, v8_entry, manifest_key)
            except ValueError as error:
                print(f"V8 materialized provenance is stale: {error}; re-downloading.")
            else:
                print(
                    f"OK: V8 already unpacked from the pinned asset "
                    f"(sha256 {expected_sha} and embedded provenance match); "
                    "skipping download"
                )
                return 0
        print(
            "V8 cached generation is incomplete or does not match the pinned "
            "asset/provenance receipts; re-downloading."
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
    try:
        metadata_sha = verify_release_metadata(v8_entry, url)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        zip_path.unlink(missing_ok=True)
        return 1
    if metadata_sha:
        print(f"release metadata verified: {metadata_sha}")

    # Unpack into a clean per-platform dir so a pin bump never leaves stale
    # files behind alongside the new ones.
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)
    print(f"Unpacking → {dest}")
    with zipfile.ZipFile(zip_path) as zf:
        try:
            if matched_milestone:
                validate_embedded_manifest(zf, manifest, v8_entry, manifest_key)
        except ValueError as error:
            print(f"ERROR: {error}", file=sys.stderr)
            zip_path.unlink(missing_ok=True)
            return 1
        zf.extractall(dest)
    zip_path.unlink(missing_ok=True)

    # Sanity check: the expected library MUST be present except when the
    # manifest deliberately disables runtime consumption (iOS simulator).
    if expected_lib is not None and not expected_lib.is_file():
        print(
            f"ERROR: expected library not found at {expected_lib} after unpack",
            file=sys.stderr,
        )
        print(f"Contents of {dest}/ (depth 3):", file=sys.stderr)
        for p in sorted(dest.rglob("*"))[:50]:
            print(f"  {p}", file=sys.stderr)
        return 1
    missing_payload = [str(path) for path in required_dest_paths if not path.is_file()]
    if missing_payload:
        print(f"ERROR: required V8 payload files missing after unpack: {missing_payload}",
              file=sys.stderr)
        return 1

    stamp_path.write_text(expected_sha + "\n", encoding="utf-8")
    if metadata_sha:
        metadata_stamp_path.write_text(metadata_sha + "\n", encoding="utf-8")

    if expected_lib is not None:
        print(f"OK: {expected_lib} present ({expected_lib.stat().st_size:,} bytes)")
    else:
        print(f"OK: headers unpacked at {expected_header_path(manifest_key).parent} "
              "(v8.h present); "
              "runtime remains disabled for iOS consumers")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
