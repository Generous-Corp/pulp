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

Same-platform publishers coordinate through a bounded lock. Each publisher
downloads and seals a unique private sibling generation, then swaps that fully
validated generation into the live path; FindV8 never considers private staging.

Avoids stderr-only output so the workflow log shows progress on stdout
for either bash or PowerShell.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
import shutil
import socket
import sys
import tempfile
import time
import urllib.request
import urllib.parse
import zipfile
from pathlib import Path, PurePosixPath, PureWindowsPath

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
GENERATION_RECEIPT = ".v8-generation-manifest.json"
GENERATION_SCHEMA = 1
GENERATION_METADATA = {
    GENERATION_RECEIPT, ".v8-asset-sha256", ".v8-release-metadata-sha256",
}
PUBLICATION_LOCK_TIMEOUT_SECS = 300.0


@contextmanager
def publication_lock(dest: Path, timeout_secs: float):
    """Serialize publishers for one platform destination.

    A directory creation is the cross-platform atomic lock operation. A waiter
    rechecks the live generation only after acquiring ownership, and a dead
    same-host owner can be recovered without trusting any private staging it
    may have left behind.
    """
    absolute_dest = dest.absolute()
    absolute_dest.parent.mkdir(parents=True, exist_ok=True)
    lock_dir = absolute_dest.parent / f".{absolute_dest.name}.fetch.lock"
    owner_path = lock_dir / "owner.json"
    deadline = time.monotonic() + timeout_secs
    owner = {
        "pid": os.getpid(),
        "host": socket.gethostname(),
        "dest": str(absolute_dest),
    }
    while True:
        try:
            lock_dir.mkdir()
            try:
                owner_path.write_text(
                    json.dumps(owner, sort_keys=True) + "\n", encoding="utf-8"
                )
            except OSError:
                lock_dir.rmdir()
                raise
            break
        except FileExistsError:
            stale = False
            try:
                current = json.loads(owner_path.read_text(encoding="utf-8"))
                if current.get("host") == socket.gethostname():
                    try:
                        os.kill(int(current["pid"]), 0)
                    except ProcessLookupError:
                        stale = True
                    except PermissionError:
                        pass
            except (OSError, ValueError, KeyError, json.JSONDecodeError):
                # A live owner may still be writing owner.json. Only recover an
                # unreadable lock after the bounded wait has elapsed.
                stale = time.monotonic() >= deadline
            if stale:
                try:
                    owner_path.unlink(missing_ok=True)
                    lock_dir.rmdir()
                    continue
                except OSError:
                    pass
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"timed out after {timeout_secs:g}s waiting for V8 publication "
                    f"lock {lock_dir}"
                )
            time.sleep(0.1)
    try:
        yield
    finally:
        try:
            owner_path.unlink(missing_ok=True)
            lock_dir.rmdir()
        except OSError as error:
            print(
                f"WARNING: could not release V8 publication lock {lock_dir}: {error}",
                file=sys.stderr,
            )


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


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def generation_files(root: Path) -> list[dict[str, object]]:
    files = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"V8 generation contains unsupported symlink: {path}")
        if not path.is_file() or path.name in GENERATION_METADATA:
            continue
        files.append({
            "path": path.relative_to(root).as_posix(),
            "size": path.stat().st_size,
            "sha256": file_sha256(path),
        })
    return files


def write_generation_receipt(root: Path, manifest_key: str, asset_sha: str,
                             metadata_sha: str | None) -> None:
    payload = {
        "schema": GENERATION_SCHEMA,
        "platform": manifest_key,
        "asset_sha256": asset_sha,
        "release_metadata_sha256": metadata_sha,
        "files": generation_files(root),
    }
    path = root / GENERATION_RECEIPT
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def validate_generation_receipt(root: Path, manifest_key: str, asset_sha: str,
                                metadata_sha: str | None) -> None:
    path = root / GENERATION_RECEIPT
    try:
        receipt = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValueError("V8 generation has no extracted-file receipt") from exc
    except json.JSONDecodeError as exc:
        raise ValueError("V8 extracted-file receipt is invalid JSON") from exc
    expected_header = {
        "schema": GENERATION_SCHEMA,
        "platform": manifest_key,
        "asset_sha256": asset_sha,
        "release_metadata_sha256": metadata_sha,
    }
    for field, expected in expected_header.items():
        if receipt.get(field) != expected:
            raise ValueError(
                f"V8 generation receipt {field} mismatch: "
                f"actual={receipt.get(field)!r}, expected={expected!r}"
            )
    recorded = receipt.get("files")
    if not isinstance(recorded, list) or not recorded:
        raise ValueError("V8 generation receipt has no extracted-file inventory")
    actual = generation_files(root)
    if actual != recorded:
        raise ValueError("V8 generation extracted-file content/inventory mismatch")


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
    verify_builder_tag_ref(determinism)
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


def verify_builder_tag_ref(determinism: dict) -> None:
    """Bind the release tag named by the asset URLs to the pinned builder SHA.

    Release metadata binds the V8/Skia/Dawn tuple but does not contain the
    v8-builder source revision. The Git ref check closes that otherwise-manual
    provenance edge and fails if the lightweight release tag is moved.
    """
    url = determinism.get("v8_builder_tag_ref_url")
    expected = determinism.get("v8_builder_ref")
    if not isinstance(url, str) or not isinstance(expected, str):
        raise ValueError("matched V8 pin has no builder tag-ref URL/SHA")
    with urllib.request.urlopen(url) as response:
        payload = response.read()
    try:
        ref = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise ValueError("V8 builder tag-ref response is invalid JSON") from exc
    obj = ref.get("object") if isinstance(ref, dict) else None
    actual = obj.get("sha") if isinstance(obj, dict) else None
    kind = obj.get("type") if isinstance(obj, dict) else None
    if kind != "commit" or actual != expected:
        raise ValueError(
            f"V8 builder release tag mismatch: type={kind!r}, sha={actual!r}, "
            f"pinned={expected!r}"
        )


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


def _archive_member_target(dest: Path, member_name: str) -> Path:
    """Resolve a ZIP member below private staging or reject path traversal."""
    posix_name = member_name.replace("\\", "/")
    relative = PurePosixPath(posix_name)
    if (not posix_name or relative.is_absolute() or ".." in relative.parts
            or PureWindowsPath(member_name).is_absolute()):
        raise ValueError(f"unsafe V8 archive member path: {member_name!r}")
    target = dest.joinpath(*relative.parts)
    try:
        target.resolve(strict=False).relative_to(dest.resolve())
    except ValueError as error:
        raise ValueError(
            f"V8 archive member escapes private staging: {member_name!r}"
        ) from error
    return target


def generation_valid(root: Path, manifest: dict, v8_entry: dict,
                     manifest_key: str, expected_sha: str) -> bool:
    """Deep-verify one complete materialized provider generation."""
    matched_milestone = (
        v8_entry.get("determinism", {}).get("pair_kind") == "chromium-milestone"
    )
    metadata_sha = (
        v8_entry.get("determinism", {}).get("release_metadata_sha256")
        if matched_milestone else None
    )
    required = required_materialized_paths(root, manifest_key, matched_milestone)
    asset_stamp = root / ".v8-asset-sha256"
    metadata_stamp = root / ".v8-release-metadata-sha256"
    try:
        if not all(path.is_file() for path in required):
            return False
        if (not asset_stamp.is_file()
                or asset_stamp.read_text(encoding="utf-8").strip() != expected_sha):
            return False
        if matched_milestone and (
                not metadata_stamp.is_file()
                or metadata_stamp.read_text(encoding="utf-8").strip() != metadata_sha):
            return False
        validate_generation_receipt(
            root, manifest_key, expected_sha, metadata_sha
        )
        if matched_milestone:
            validate_materialized_manifest(root, manifest, v8_entry, manifest_key)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError):
        return False
    return True


def _download_and_seal_generation(
    stage: Path,
    archive_path: Path,
    url: str,
    expected_sha: str,
    manifest: dict,
    v8_entry: dict,
    manifest_key: str,
    has_library: bool,
) -> None:
    """Populate and fully validate private staging without touching live state."""
    matched_milestone = (
        v8_entry.get("determinism", {}).get("pair_kind") == "chromium-milestone"
    )
    print(f"Downloading -> {archive_path}")
    with urllib.request.urlopen(url) as response, archive_path.open("wb") as output:
        for chunk in iter(lambda: response.read(1024 * 1024), b""):
            output.write(chunk)

    actual_sha = file_sha256(archive_path)
    if actual_sha != expected_sha:
        raise ValueError(
            f"sha256 mismatch\n  expected: {expected_sha}\n  actual:   {actual_sha}"
        )
    print(f"sha256 verified: {actual_sha}")

    metadata_sha = verify_release_metadata(v8_entry, url)
    if metadata_sha:
        print(f"release metadata verified: {metadata_sha}")

    print(f"Unpacking privately -> {stage}")
    with zipfile.ZipFile(archive_path) as archive:
        for member in archive.infolist():
            _archive_member_target(stage, member.filename)
        if matched_milestone:
            validate_embedded_manifest(archive, manifest, v8_entry, manifest_key)
        archive.extractall(stage)

    expected = expected_library_path(manifest_key) if has_library else None
    if expected is not None:
        stage_expected = stage / expected.relative_to(DEST_ROOT / manifest_key)
        if not stage_expected.is_file():
            raise ValueError(
                f"expected library not found at {stage_expected} after private unpack"
            )
    required = required_materialized_paths(stage, manifest_key, matched_milestone)
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise ValueError(
            f"required V8 payload files missing after private unpack: {missing}"
        )

    write_generation_receipt(stage, manifest_key, expected_sha, metadata_sha)
    if metadata_sha:
        (stage / ".v8-release-metadata-sha256").write_text(
            metadata_sha + "\n", encoding="utf-8"
        )
    # The asset stamp is the final trust marker. FindV8 will reject staging or
    # live state unless every prior receipt and payload check is also present.
    (stage / ".v8-asset-sha256").write_text(expected_sha + "\n", encoding="utf-8")
    if not generation_valid(stage, manifest, v8_entry, manifest_key, expected_sha):
        raise ValueError("private V8 generation failed final receipt validation")


def _remove_path(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink(missing_ok=True)
    elif path.exists():
        shutil.rmtree(path)


def _publish_generation(stage: Path, dest: Path) -> None:
    """Swap a sealed same-filesystem stage into the live provider path.

    Readers can see the previous generation, a temporarily absent provider, or
    the complete new generation; private staging is never a FindV8 candidate.
    If publication fails after retiring the old path, restore it when possible.
    """
    retired: Path | None = None
    if dest.exists() or dest.is_symlink():
        retired = dest.parent / (
            f".{dest.name}.retired-{os.getpid()}-{time.time_ns()}"
        )
        dest.rename(retired)
    try:
        stage.rename(dest)
    except OSError as publish_error:
        if retired is not None and retired.exists() and not dest.exists():
            try:
                retired.rename(dest)
            except OSError as rollback_error:
                raise RuntimeError(
                    f"V8 publication failed and the prior generation could not be "
                    f"restored; it remains at {retired}: {rollback_error}"
                ) from publish_error
        raise
    if retired is not None:
        try:
            _remove_path(retired)
        except OSError as error:
            print(
                f"WARNING: published V8 but could not remove retired generation "
                f"{retired}: {error}",
                file=sys.stderr,
            )


def _fetch_and_publish_generation(
    dest: Path,
    url: str,
    expected_sha: str,
    manifest: dict,
    v8_entry: dict,
    manifest_key: str,
    has_library: bool,
) -> None:
    """Download privately, seal completely, then publish under the caller's lock."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    stage: Path | None = None
    archive_path: Path | None = None
    archive_fd: int | None = None
    try:
        stage = Path(tempfile.mkdtemp(
            prefix=f".{dest.name}.staging-", dir=dest.parent
        ))
        archive_fd, archive_name = tempfile.mkstemp(
            prefix=f".{dest.name}.download-", suffix=".zip", dir=dest.parent
        )
        archive_path = Path(archive_name)
        os.close(archive_fd)
        archive_fd = None
        _download_and_seal_generation(
            stage, archive_path, url, expected_sha, manifest, v8_entry,
            manifest_key, has_library,
        )
        _publish_generation(stage, dest)
        print(f"OK: atomically published V8 generation at {dest}")
    finally:
        # Every ZipFile/response context above is closed before cleanup, which
        # keeps both happy and rejected paths valid on Windows.
        if archive_fd is not None:
            try:
                os.close(archive_fd)
            except OSError as error:
                print(f"WARNING: could not close private V8 archive: {error}",
                      file=sys.stderr)
        if archive_path is not None:
            try:
                archive_path.unlink(missing_ok=True)
            except OSError as error:
                print(f"WARNING: could not remove private V8 archive {archive_path}: {error}",
                      file=sys.stderr)
        if stage is not None and (stage.exists() or stage.is_symlink()):
            try:
                _remove_path(stage)
            except OSError as error:
                print(f"WARNING: could not remove private V8 staging {stage}: {error}",
                      file=sys.stderr)


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
            f"(manifest key {manifest_key!r}); skipping fetch; this "
            f"platform will not have the sealed V8 provider."
        )
        return 0

    url = asset["url"]
    expected_sha = asset["sha256"]
    has_library = asset.get("library", True)
    expected_lib = expected_library_path(manifest_key) if has_library else None
    dest = DEST_ROOT / manifest_key

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
        if generation_valid(
                baked_root, manifest, v8_entry, manifest_key, expected_sha):
            print(
                f"OK: using baked V8 at {baked_root} "
                f"(sha256 {expected_sha} and embedded provenance match pin); "
                f"skipping fetch (PULP_USE_BAKED_V8)"
            )
            return 0
        print(
            "PULP_USE_BAKED_V8 set but baked V8 is missing or stale "
            "(receipt, provenance, header, or required binary mismatch); "
            "re-fetching the pinned asset."
        )

    # Idempotency stamp: skip the download when the already-unpacked asset
    # matches the current pin. A pin bump changes expected_sha, the stamp no
    # longer matches, and the asset is re-fetched — never silently stale.
    if generation_valid(dest, manifest, v8_entry, manifest_key, expected_sha):
        print(
            f"OK: V8 already unpacked from the pinned asset "
            f"(sha256 {expected_sha} and embedded provenance match); "
            "skipping download"
        )
        return 0
    if dest.exists() or dest.is_symlink():
        print(
            "V8 cached generation is incomplete or does not match the pinned "
            "asset/provenance receipts; re-downloading."
        )

    try:
        with publication_lock(dest, PUBLICATION_LOCK_TIMEOUT_SECS):
            # Another same-platform publisher may have completed while this
            # process waited. Deep-verify its exact generation before skipping.
            if generation_valid(
                    dest, manifest, v8_entry, manifest_key, expected_sha):
                print(
                    f"OK: concurrent V8 publisher completed the pinned generation "
                    f"at {dest}; skipping download"
                )
                return 0
            _fetch_and_publish_generation(
                dest, url, expected_sha, manifest, v8_entry, manifest_key,
                has_library,
            )
    except (OSError, RuntimeError, TimeoutError, ValueError,
            zipfile.BadZipFile) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    if expected_lib is not None:
        print(f"OK: {expected_lib} present ({expected_lib.stat().st_size:,} bytes)")
    else:
        print(f"OK: headers unpacked at {expected_header_path(manifest_key).parent} "
              "(v8.h present); "
              "runtime remains disabled for iOS consumers")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
