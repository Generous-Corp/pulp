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
    python3 tools/scripts/fetch_skia_for_release.py <matrix-platform> \
        --cache-root ~/.cache/pulp/skia --cache-lock-timeout 300

`--cache-root` is the host/worktree mode: it resolves an immutable
`<platform>-<asset-sha256>` generation, downloads into private sibling staging,
and atomically renames the validated generation into place. Release workflows
continue to use the default or explicit `--dest` mode for isolated matrix trees.

Where `<matrix-platform>` is one of the release-cli.yml matrix values:
    darwin-arm64, darwin-x64, darwin-universal, linux-x64, linux-arm64,
    windows-x64, windows-arm64, ios-device-arm64, ios-simulator-arm64-x86_64,
    wasm

iOS slices intentionally keep the per-arch subdir under
`build/ios-gpu/lib/Release/` (device-arm64, simulator-arm64,
simulator-x86_64) so the device and simulator zips can co-exist in one
unpack tree. FindSkia.cmake selects the right subdir based on the
active SDK.

If the manifest has no asset for the requested platform (e.g. windows-*),
the script exits 0 with a message — those platforms keep their current
CG-only behavior until skia-builder publishes assets for them. Platforms
that DO have assets must succeed (sha256 verified + expected library on
disk) or the script exits non-zero.

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

from contextlib import contextmanager
from contextvars import ContextVar
import hashlib
import json
import math
import os
import re
import shutil
import socket
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath, PureWindowsPath

# Matrix platform (release-cli.yml) → manifest release_assets key.
# Matrix uses `darwin-*` and `windows-*`; manifest uses `mac-*` and `win-*`.
MATRIX_TO_MANIFEST = {
    "darwin-arm64": "mac-arm64",
    "darwin-x64": "mac-x86_64",  # Intel-thin mac slice (G3 macOS universal support)
    "darwin-universal": "mac-universal",  # fat arm64+x86_64 mac slice (G3)
    "linux-x64": "linux-x64",
    "linux-arm64": "linux-arm64",
    "windows-x64": "win-x64",
    "windows-arm64": "win-arm64",
    # iOS device + simulator slices share the build/ios-gpu/lib/Release/
    # tree but keep their arch subdir intact (see expected_library_path
    # and the post-unpack flatten guard).
    "ios-device-arm64": "ios-device-arm64",
    "ios-simulator-arm64-x86_64": "ios-simulator-arm64-x86_64",
    # Emscripten/wasm slice. Ganesh + WebGL2 (no Dawn, no Graphite); libs ship
    # flat under build/wasm-gpu/lib/Release/, so the flatten step is a no-op.
    "wasm": "wasm-wasm32",
}

# iOS keeps its per-arch subdir under Release/ (device-arm64,
# simulator-arm64, simulator-x86_64) because flattening would collide
# library names across slices. The flatten step below is skipped for
# any matrix entry in this set.
_IOS_PRESERVE_ARCH_SUBDIR = {
    "ios-device-arm64",
    "ios-simulator-arm64-x86_64",
}

# Each main() invocation owns only the unique archives it creates. This keeps
# recursive keyed-cache publication and concurrent processes isolated while
# guaranteeing cleanup from one outer finally on every return or exception.
_ACTIVE_ARCHIVES: ContextVar[list[Path] | None] = ContextVar(
    "skia_active_archives", default=None
)


@contextmanager
def cache_lock(dest_root: str, timeout_secs: float):
    """Serialize publication into one cache root with bounded stale recovery."""
    dest = Path(dest_root).absolute()
    dest.parent.mkdir(parents=True, exist_ok=True)
    lock_dir = dest.parent / f".{dest.name}.fetch.lock"
    owner_path = lock_dir / "owner.json"
    deadline = time.monotonic() + timeout_secs
    owner = {"pid": os.getpid(), "host": socket.gethostname(), "dest": str(dest)}
    while True:
        try:
            lock_dir.mkdir()
            try:
                owner_path.write_text(json.dumps(owner, sort_keys=True) + "\n", encoding="utf-8")
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
                        # A live owner under another uid is still live.
                        pass
            except (OSError, ValueError, KeyError, json.JSONDecodeError):
                # The owner may still be writing. Only treat an unreadable lock
                # as stale after the entire bounded wait has elapsed.
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
                    f"timed out after {timeout_secs:g}s waiting for Skia cache lock {lock_dir}"
                )
            time.sleep(0.1)
    try:
        yield
    finally:
        try:
            owner_path.unlink(missing_ok=True)
            lock_dir.rmdir()
        except OSError as error:
            print(f"WARNING: could not release Skia cache lock {lock_dir}: {error}", file=sys.stderr)


def manifest_asset(matrix_platform: str) -> tuple[str, dict[str, str] | None]:
    """Return the manifest key and exact release asset for one matrix platform."""
    manifest_key = MATRIX_TO_MANIFEST[matrix_platform]
    manifest_path = Path("tools/deps/manifest.json")
    if not manifest_path.is_file():
        raise ValueError(f"{manifest_path} not found (run from repo root)")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    skia_entry = next(
        (entry for entry in manifest.get("dependencies", [])
         if isinstance(entry, dict) and entry.get("name", "").lower() == "skia"),
        None,
    )
    if skia_entry is None:
        raise ValueError("no 'Skia' dependency entry in manifest.json")
    return manifest_key, skia_entry.get("determinism", {}).get("release_assets", {}).get(manifest_key)


def keyed_cache_dest(cache_root: str, matrix_platform: str, asset_sha: str) -> Path:
    """Immutable cache generation selected by platform and exact asset identity."""
    if not re.fullmatch(r"[0-9a-fA-F]{64}", asset_sha):
        raise ValueError("cache asset SHA-256 must contain exactly 64 hexadecimal characters")
    return Path(cache_root).absolute() / f"{matrix_platform}-{asset_sha.lower()}"


def cache_generation_valid(dest: Path, matrix_platform: str, expected_sha: str) -> bool:
    stamp = dest / ".skia-asset-sha256"
    try:
        return (cache_generation_materialized(dest, matrix_platform) and stamp.is_file()
                and stamp.read_text(encoding="utf-8").strip() == expected_sha)
    except OSError:
        return False


def cache_generation_materialized(dest: Path, matrix_platform: str) -> bool:
    expected_lib = expected_library_path(matrix_platform, str(dest))
    expected_dawn = expected_dawn_library_path(matrix_platform, str(dest))
    required = [expected_lib] + ([] if expected_dawn is None else [expected_dawn])
    return all(_materialized_archive(path) for path in required)


def _materialized_archive(path: Path) -> bool:
    """Reject absent/empty archives and Git LFS pointer placeholders."""
    if not path.is_file() or path.stat().st_size == 0:
        return False
    with path.open("rb") as archive:
        return not archive.read(128).startswith(
            b"version https://git-lfs.github.com/spec/v1"
        )


def _archive_member_target(dest: Path, member_name: str) -> Path:
    """Resolve one ZIP member beneath dest or reject path traversal."""
    posix_name = member_name.replace("\\", "/")
    relative = PurePosixPath(posix_name)
    if (not posix_name or relative.is_absolute() or ".." in relative.parts
            or PureWindowsPath(member_name).is_absolute()):
        raise ValueError(f"unsafe archive member path: {member_name!r}")
    target = dest.joinpath(*relative.parts)
    dest_resolved = dest.resolve()
    target_resolved = target.resolve(strict=False)
    try:
        target_resolved.relative_to(dest_resolved)
    except ValueError as error:
        raise ValueError(f"archive member escapes destination: {member_name!r}") from error
    return target


def publish_keyed_cache(matrix_platform: str, cache_root: str,
                        timeout_secs: float, expected_sha: str) -> int:
    """Build privately, validate, then atomically publish one immutable generation."""
    dest = keyed_cache_dest(cache_root, matrix_platform, expected_sha)
    try:
        with cache_lock(str(dest), timeout_secs):
            if cache_generation_valid(dest, matrix_platform, expected_sha):
                print(f"OK: immutable Skia cache generation ready at {dest}")
                return 0
            if dest.exists():
                print(
                    f"ERROR: immutable Skia cache generation exists but is invalid: {dest}; "
                    "refusing in-place mutation while consumers may be bound to it",
                    file=sys.stderr,
                )
                return 1
            dest.parent.mkdir(parents=True, exist_ok=True)
            stage = Path(tempfile.mkdtemp(prefix=f".{dest.name}.staging-", dir=dest.parent))
            try:
                baked = os.environ.pop("PULP_USE_BAKED_SKIA", None)
                inherited_skia = os.environ.pop("SKIA_DIR", None)
                try:
                    rc = main([sys.argv[0], matrix_platform, "--dest", str(stage)])
                finally:
                    if baked is not None:
                        os.environ["PULP_USE_BAKED_SKIA"] = baked
                    if inherited_skia is not None:
                        os.environ["SKIA_DIR"] = inherited_skia
                if rc != 0 or not cache_generation_valid(stage, matrix_platform, expected_sha):
                    print("ERROR: private Skia cache staging failed validation", file=sys.stderr)
                    return 1
                # Same-parent rename is the publication boundary. Readers see
                # either no generation or the complete, validated generation.
                try:
                    stage.rename(dest)
                except FileExistsError:
                    if cache_generation_valid(dest, matrix_platform, expected_sha):
                        print(f"OK: concurrent publisher completed immutable cache at {dest}")
                        return 0
                    print(
                        f"ERROR: publication destination appeared invalid during rename: {dest}",
                        file=sys.stderr,
                    )
                    return 1
                print(f"OK: atomically published immutable Skia cache at {dest}")
                return 0
            finally:
                if stage.exists():
                    shutil.rmtree(stage)
    except TimeoutError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

# Expected on-disk relative library path under external/skia-build/.
# Matches the layout produced by skia-builder release zips and probed by
# FindSkia.cmake's `${SKIA_DIR}/build/<plat>-gpu/lib/Release/` branch.
def expected_library_path(matrix_platform: str, dest_root: str = "external/skia-build") -> Path:
    if matrix_platform.startswith("darwin"):
        plat_dir = "mac-gpu"
        lib_name = "libskia.a"
        arch_subdir = ""
    elif matrix_platform.startswith("linux"):
        plat_dir = "linux-gpu"
        lib_name = "libskia.a"
        arch_subdir = ""
    elif matrix_platform.startswith("windows"):
        plat_dir = "win-gpu"
        lib_name = "skia.lib"
        arch_subdir = ""
    elif matrix_platform == "wasm":
        plat_dir = "wasm-gpu"
        lib_name = "libskia.a"
        arch_subdir = ""
    elif matrix_platform == "ios-device-arm64":
        # Device + simulator share build/ios-gpu/ and keep
        # their arch subdir under Release/ to avoid filename collisions.
        plat_dir = "ios-gpu"
        lib_name = "libskia.a"
        arch_subdir = "device-arm64"
    elif matrix_platform == "ios-simulator-arm64-x86_64":
        # Fat simulator zip contains both simulator-arm64/ and
        # simulator-x86_64/. The arm64 slice is what runs on the
        # local-CI Apple Silicon runners and is the canonical sanity
        # check; the x86_64 slice ships alongside it untouched.
        plat_dir = "ios-gpu"
        lib_name = "libskia.a"
        arch_subdir = "simulator-arm64"
    else:
        raise SystemExit(f"unknown matrix platform: {matrix_platform!r}")
    parts = [dest_root, "build", plat_dir, "lib", "Release"]
    if arch_subdir:
        parts.append(arch_subdir)
    parts.append(lib_name)
    return Path(*parts)


def expected_dawn_library_path(
    matrix_platform: str, dest_root: str = "external/skia-build"
) -> Path | None:
    if matrix_platform == "wasm":
        return None
    skia = expected_library_path(matrix_platform, dest_root)
    dawn_name = "dawn_combined.lib" if matrix_platform.startswith("windows") else "libdawn_combined.a"
    return skia.with_name(dawn_name)


def _version_doc_has_asset_digest(
    version_path: Path, asset_name: str, expected_sha: str
) -> bool:
    if not asset_name or not version_path.is_file():
        return False
    expected_sha = expected_sha.lower()
    try:
        lines = version_path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return False
    return any(asset_name in line and expected_sha in line.lower() for line in lines)


def _main(argv: list[str]) -> int:
    dest_root = "external/skia-build"
    args = argv[1:]
    cache_root = None
    print_cache_dest = False
    validate_only = False
    if "--dest" in args:
        di = args.index("--dest")
        if di + 1 >= len(args):
            print(f"usage: {argv[0]} <matrix-platform> [--dest DIR]", file=sys.stderr)
            return 2
        dest_root = args[di + 1]
        del args[di:di + 2]
    if "--cache-root" in args:
        ci = args.index("--cache-root")
        if ci + 1 >= len(args):
            print(f"usage: {argv[0]} <matrix-platform> [--cache-root DIR]", file=sys.stderr)
            return 2
        cache_root = args[ci + 1]
        del args[ci:ci + 2]
    if "--print-cache-dest" in args:
        print_cache_dest = True
        args.remove("--print-cache-dest")
    if "--validate-only" in args:
        validate_only = True
        args.remove("--validate-only")
    if cache_root is not None and "--dest" in argv:
        print("ERROR: --cache-root and --dest are mutually exclusive", file=sys.stderr)
        return 2
    lock_timeout = None
    if "--cache-lock-timeout" in args:
        li = args.index("--cache-lock-timeout")
        if li + 1 >= len(args):
            print(f"usage: {argv[0]} <matrix-platform> [--dest DIR] [--cache-lock-timeout SECS]", file=sys.stderr)
            return 2
        try:
            lock_timeout = float(args[li + 1])
        except ValueError:
            print("ERROR: --cache-lock-timeout must be numeric", file=sys.stderr)
            return 2
        del args[li:li + 2]
        if not math.isfinite(lock_timeout) or lock_timeout <= 0:
            print("ERROR: --cache-lock-timeout must be finite and positive", file=sys.stderr)
            return 2
    if len(args) != 1:
        print(f"usage: {argv[0]} <matrix-platform> [--dest DIR]", file=sys.stderr)
        return 2
    matrix_platform = args[0]

    if validate_only and cache_root is not None:
        print("ERROR: --validate-only requires an explicit --dest", file=sys.stderr)
        return 2

    if cache_root is not None:
        if matrix_platform not in MATRIX_TO_MANIFEST:
            print(f"ERROR: keyed cache requires a known platform: {matrix_platform}", file=sys.stderr)
            return 2
        try:
            _, keyed_asset = manifest_asset(matrix_platform)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            print(f"ERROR: {error}", file=sys.stderr)
            return 1
        if keyed_asset is None:
            print(f"ERROR: keyed cache has no asset for {matrix_platform}", file=sys.stderr)
            return 1
        try:
            resolved = keyed_cache_dest(cache_root, matrix_platform, keyed_asset["sha256"])
        except (KeyError, ValueError) as error:
            print(f"ERROR: {error}", file=sys.stderr)
            return 1
        if print_cache_dest:
            print(resolved)
            return 0
        return publish_keyed_cache(
            matrix_platform, cache_root, lock_timeout or 300.0, keyed_asset["sha256"]
        )

    if lock_timeout is not None:
        try:
            with cache_lock(dest_root, lock_timeout):
                # Re-enter without the lock option. The current cache is
                # rechecked only after ownership is acquired, so a waiter sees
                # the first publisher's exact stamp/library and skips download.
                return main([argv[0], matrix_platform, "--dest", dest_root])
        except TimeoutError as error:
            print(f"ERROR: {error}", file=sys.stderr)
            return 1

    if matrix_platform not in MATRIX_TO_MANIFEST:
        print(
            f"WARNING: unknown matrix platform {matrix_platform!r}; "
            f"known values are {sorted(MATRIX_TO_MANIFEST)}",
            file=sys.stderr,
        )
        return 0

    try:
        manifest_key, asset = manifest_asset(matrix_platform)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if asset is None:
        # Not every release-cli matrix platform has a published skia-builder
        # asset. windows-* is not currently published — those platforms
        # keep their existing CG-only behavior. Exit 0 so the workflow
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
    expected_lib = expected_library_path(matrix_platform, dest_root)

    if validate_only:
        if cache_generation_valid(Path(dest_root), matrix_platform, expected_sha):
            print(f"OK: complete pinned Skia/Dawn generation validated at {dest_root}")
            return 0
        print(
            f"ERROR: {dest_root} is not a complete materialized generation for "
            f"{matrix_platform} sha256 {expected_sha}",
            file=sys.stderr,
        )
        return 1

    print(f"Skia fetch: matrix={matrix_platform}, manifest={manifest_key}")
    print(f"  url: {url}")
    print(f"  sha256: {expected_sha}")
    print(f"  expected lib: {expected_lib}")

    # Opt-in baked-Skia short-circuit (PULP_USE_BAKED_SKIA — set ONLY by the
    # Tart VM runner golden, never by releases or clean GitHub runners). The
    # golden bakes Skia at $SKIA_DIR, so the workflow's fetch is redundant:
    # FindSkia.cmake reads $SKIA_DIR before the checkout's external/skia-build.
    # We skip the ~250-500 MiB download ONLY when the baked Skia's stamp matches
    # the current manifest pin — so a pin bump on a not-yet-rebaked golden falls
    # through to a normal fetch and is NEVER stuck on stale baked Skia. The same
    # stamp invariant as the dest guard below, applied to $SKIA_DIR.
    baked_dir = os.environ.get("SKIA_DIR", "").strip()
    if os.environ.get("PULP_USE_BAKED_SKIA") and baked_dir:
        baked_root = Path(baked_dir)
        if cache_generation_valid(baked_root, matrix_platform, expected_sha):
            print(
                f"OK: using baked Skia at {baked_dir} "
                f"(complete Skia/Dawn generation matches sha256 {expected_sha}); "
                "skipping fetch "
                f"(PULP_USE_BAKED_SKIA)"
            )
            return 0
        print(
            "PULP_USE_BAKED_SKIA set but baked Skia is missing or stale "
            "(stamp != current pin) — re-fetching the pinned asset."
        )

    # Idempotency stamp. A self-hosted CI runner checks out with
    # `clean: false`, so `external/skia-build/` persists between jobs;
    # re-downloading the ~250-500 MiB asset every build is wasteful. But a
    # naive "is libskia.a present?" guard is *wrong*: when
    # tools/deps/manifest.json bumps the pinned Skia asset, a stale local
    # library would silently shadow the new pin and CI would build against
    # the wrong Skia. The stamp records the sha256
    # actually unpacked here, so the download is skipped only when that
    # sha matches the current pin — a pin bump changes expected_sha, the
    # stamp no longer matches, and the asset is re-fetched.
    stamp_path = Path(dest_root) / ".skia-asset-sha256"
    if expected_lib.is_file():
        if cache_generation_valid(Path(dest_root), matrix_platform, expected_sha):
            print(
                f"OK: complete Skia/Dawn generation already unpacked from the pinned asset "
                f"(sha256 {expected_sha}); skipping download"
            )
            return 0
        if stamp_path.is_file():
            print(
                "Skia stamp does not match the pinned asset — the manifest "
                "pin changed since the last fetch; re-downloading."
            )
        else:
            asset_name = Path(urllib.parse.urlparse(url).path).name
            version_path = Path(dest_root) / "VERSION.md"
            if (cache_generation_materialized(Path(dest_root), matrix_platform)
                    and _version_doc_has_asset_digest(version_path, asset_name, expected_sha)):
                stamp_path.write_text(expected_sha + "\n", encoding="utf-8")
                print(
                    "OK: Skia already present and VERSION.md records the "
                    f"pinned asset digest (sha256 {expected_sha}); seeded "
                    "asset stamp and skipping download"
                )
                return 0

    zip_fd, zip_name = tempfile.mkstemp(prefix=".skia-release-asset-", suffix=".zip", dir=".")
    os.close(zip_fd)
    zip_path = Path(zip_name)
    archives = _ACTIVE_ARCHIVES.get()
    if archives is None:
        raise RuntimeError("Skia archive created outside the main() cleanup scope")
    archives.append(zip_path)
    print(f"Downloading -> {zip_path}")
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

    dest = Path(dest_root)
    dest.mkdir(parents=True, exist_ok=True)
    # Self-heal a corrupted warm cache: a dangling `build` symlink (its target
    # gone) makes mkdir see the path as both present (the link exists) and
    # unusable (a child cannot be created because the target is missing), which
    # aborts unpacking. Remove ONLY a broken link — a healthy cache is left
    # intact, so this costs nothing on a good runner.
    build_entry = dest / "build"
    if build_entry.is_symlink() and not build_entry.exists():
        build_entry.unlink()
    print(f"Unpacking -> {dest}")
    # Idempotent extract. A prior cache restore may already have populated the
    # destination tree; zipfile.extractall raises FileExistsError on a directory
    # member whose target already exists. Create directories with exist_ok and
    # overwrite files so unpacking over a warm/cached checkout succeeds.
    with zipfile.ZipFile(zip_path) as zf:
        try:
            validated_members = [
                (member, _archive_member_target(dest, member.filename))
                for member in zf.infolist()
            ]
        except ValueError as error:
            print(f"ERROR: {error}", file=sys.stderr)
            return 1
        for member, target in validated_members:
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member) as src, target.open("wb") as out:
                while True:
                    chunk = src.read(1024 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)

    # Normalize skia-builder chrome/m144+ layout (pulp #1962).
    #
    # The expected library lives at:
    #   external/skia-build/build/<plat>-gpu/lib/Release/<lib>
    # but chrome/m144 zips place it one directory deeper, under an arch
    # subdir:
    #   external/skia-build/build/<plat>-gpu/lib/Release/<arch>/<lib>
    # Flatten that arch directory into Release/ so FindSkia.cmake's
    # existing layout probe matches without further changes.
    #
    # iOS is the deliberate exception: device-arm64, simulator-arm64,
    # and simulator-x86_64 all ship libskia.a / libdawn_combined.a with
    # the same names and CANNOT be flattened into one Release/ dir
    # without collision. For iOS, expected_lib already points inside
    # the arch subdir, so the flatten loop must be skipped entirely.
    if matrix_platform in _IOS_PRESERVE_ARCH_SUBDIR:
        # Nothing to do — the upstream layout is exactly what FindSkia
        # expects for iOS. Drop into the sanity check below.
        pass
    elif not expected_lib.is_file():
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
                        f"{moved} file(s) from {arch_dir} -> {release_dir}"
                    )
                # rmdir succeeds only if empty; that's exactly what we want.
                try:
                    arch_dir.rmdir()
                except OSError:
                    # Non-empty (e.g. obj/ subfolder) — leave in place.
                    pass

    # Sanity check the complete generation, not merely libskia. A missing,
    # empty, or LFS-placeholder Dawn archive is equally unusable and must not
    # receive a valid pin stamp.
    if not cache_generation_materialized(dest, matrix_platform):
        print(
            f"ERROR: expected materialized Skia/Dawn archives not found under "
            f"{dest_root} after unpack",
            file=sys.stderr,
        )
        # Help the human debug.
        print(f"Contents of {dest_root}/ (depth 3):", file=sys.stderr)
        for p in sorted(dest.rglob("*"))[:50]:
            print(f"  {p}", file=sys.stderr)
        return 1

    # Record the asset identity so the next run on a `clean: false` runner
    # can skip the download — and so a future manifest pin bump forces a
    # re-fetch (the stamp will no longer match the new expected_sha).
    stamp_path.write_text(expected_sha + "\n", encoding="utf-8")

    print(f"OK: {expected_lib} present ({expected_lib.stat().st_size:,} bytes)")
    return 0


def main(argv: list[str]) -> int:
    """Run one fetch and unlink only its exact private archives on every exit."""
    archives: list[Path] = []
    token = _ACTIVE_ARCHIVES.set(archives)
    try:
        return _main(argv)
    finally:
        for archive in archives:
            archive.unlink(missing_ok=True)
        _ACTIVE_ARCHIVES.reset(token)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
