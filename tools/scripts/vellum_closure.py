#!/usr/bin/env python3
"""Generate, gate, and export Vellum extraction closures.

The manifest is derived from CMake's file-api codemodel after a clean configure.
It is intentionally a receipt generator, not a source-authority switch: exports
must land on a protected mirror ref and never rewrite Vellum main.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable

from include_boundary import include_edges

FORBIDDEN_TARGET = re.compile(r"(?:^|[-_:])(audio|signal|host|midi|music|format|content|gpu_audio)(?:$|[-_:])", re.I)
FORBIDDEN_INCLUDE = re.compile(r"^pulp/(?:audio|signal|host|midi|music|format|content)(?:/|$)", re.I)
FORBIDDEN_ROOTS = tuple(f"core/{name}/" for name in ("audio", "signal", "host", "midi", "music", "format", "content"))
TEXT_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm", ".cmake"}
RESOURCE_EXTS = {".sksl", ".glsl", ".frag", ".vert", ".ttf", ".otf", ".woff", ".woff2"}
FULL_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
# The Pulp-built Skia support library; the D3 unit that carries it is where
# Skia enters the unit graph.
SKIA_SUPPORT_TARGET = "pulp-skia-chromium-raw-ptr-compat"
# A CMake reference to a Pulp target the extraction must not carry.
FORBIDDEN_CMAKE_TARGET = re.compile(r"\bpulp(?:-|::)(?:audio|signal|host|midi|music|format|content|gpu[-_]audio)\b", re.I)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
EXTERNAL_RE = re.compile(r"^@external/(?P<slug>[^/]+)/(?P<pin>[^/]+)(?:/|$)")
BUILD_DEPS_RE = re.compile(r"^@build/_deps/(?:pulp-mutable-)?(?P<name>[^/]+?)-(?:src|build)(?:/|$)")
SKIA_CACHE_RE = re.compile(r"[A-Za-z0-9_]+(?:-[A-Za-z0-9_]+)*-(?P<asset>[0-9a-f]{64})-receipt-v\d+")
SKIA_RECEIPT = ".skia-generation-manifest.json"
SKIA_ASSET_STAMP = ".skia-asset-sha256"
SYSTEM_RE = re.compile(r"^@system/(?P<package>[^/]+)/(?P<version>[^/]+)(?:/|$)")
SYSTEM_PROVIDERS = {"dpkg", "rpm", "pkg-config", "windows-sdk", "msvc", "apple-sdk"}
# A host location at the start of a value or after a delimiter (``=``, quote,
# whitespace, ``:`` not opening a URL, or a compiler ``-I``/``-isystem`` flag).
HOST_PATH_RE = re.compile(r"(?:^|[=\"'\s]|:(?!//)|-I|-isystem)(?:/|~[/\\]|[A-Za-z]:[/\\]|\\\\)")
# A host location embedded after a doubled separator or named through a
# home-directory variable.
HOST_EMBEDDED_RE = re.compile(r"(?<!:)//+(?:Users|home|Volumes|private|tmp|var|root|mnt|opt)/|\$\{?HOME\b|%USERPROFILE%|%HOMEPATH%", re.I)
PARENT_SEGMENT_RE = re.compile(r"(?:^|[/\\])\.\.(?:[/\\]|$)")


def run(*cmd: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(cmd, cwd=cwd, text=True, encoding="utf-8", stderr=subprocess.STDOUT).strip()


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def find_reply(build: Path, prefix: str) -> Path:
    replies = sorted((build / ".cmake/api/v1/reply").glob(prefix + "*.json"))
    if not replies:
        raise SystemExit(f"missing CMake file-api reply {prefix}*.json; configure after creating query")
    return replies[-1]


def codemodel(build: Path) -> tuple[dict, dict]:
    index = json.loads(find_reply(build, "index-").read_text(encoding="utf-8"))
    cm = next((x for x in index["reply"].values() if x.get("kind") == "codemodel"), None)
    if not cm:
        raise SystemExit("CMake file-api index has no codemodel reply")
    model = json.loads((build / ".cmake/api/v1/reply" / cm["jsonFile"]).read_text(encoding="utf-8"))
    configurations = model.get("configurations", [])
    if not configurations:
        raise SystemExit("CMake codemodel has no configuration")
    config = configurations[0]
    targets: dict[str, dict] = {}
    for entry in config.get("targets", []):
        data = json.loads((build / ".cmake/api/v1/reply" / entry["jsonFile"]).read_text(encoding="utf-8"))
        targets[data["id"]] = data
    return config, targets


def norm_path(raw: str, source: Path, build: Path) -> str:
    p = Path(raw)
    if not p.is_absolute():
        # File-api paths are relative to the codemodel source root unless
        # explicitly rooted.  Resolving against cwd made the manifest depend
        # on where the script was launched.
        p = source / p
    # Build directories normally live below the source checkout.  Check the
    # build root first or generated/FetchContent files would be mislabeled as
    # repository paths and become export members.
    try:
        return "@build/" + p.resolve().relative_to(build.resolve()).as_posix()
    except ValueError:
        try:
            return p.resolve().relative_to(source.resolve()).as_posix()
        except ValueError:
            return str(p.resolve())


def resolve_include(name: str, origin: Path, include_dirs: list[Path], source: Path) -> Path | None:
    candidates = [origin.parent / name] + [d / name for d in include_dirs] + [source / name]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    return None


def collect_headers(files: set[Path], include_dirs: list[Path], source: Path) -> set[Path]:
    queue = list(files)
    seen = set(files)
    while queue:
        path = queue.pop()
        if path.suffix.lower() not in TEXT_EXTS or not path.is_file():
            continue
        try:
            edges = include_edges(path)
        except OSError:
            continue
        for _line, _delimiter, name in edges:
            found = resolve_include(name, path, include_dirs, source)
            if found and found not in seen:
                seen.add(found)
                queue.append(found)
    return seen


HEADER_EXTS = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}
UNIT_HEADERS_FILE = "vellum-unit-headers.txt"


def unit_header_manifests(build: Path) -> dict[str, tuple[Path, list[str]]]:
    """Each D3 unit target's PULP_UNIT_HEADERS, as configure wrote them.

    Returns target -> (source dir, headers relative to it). A target that
    declares no manifest maps to an empty list.
    """
    path = build / UNIT_HEADERS_FILE
    if not path.is_file():
        raise SystemExit(f"{path} is missing; reconfigure with PULP_BUILD_TESTS=ON to write the unit header manifests")
    manifests: dict[str, tuple[Path, list[str]]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        target, directory, headers = (line.split("\t") + ["", ""])[:3]
        manifests[target] = (Path(directory), [h for h in headers.split(";") if h])
    return manifests


def unit_public_headers(targets: dict, names: Iterable[str], source: Path,
                        manifests: dict[str, tuple[Path, list[str]]]) -> tuple[set[Path], dict[str, list[str]]]:
    """The public headers the selected targets declare, checked against disk.

    A unit's public surface is its reviewed PULP_UNIT_HEADERS list, not only
    the headers its own sources happen to include: a header-only API or an
    umbrella header no unit source includes would otherwise be missing from the
    export. Every listed header must exist. Where every configured target of a
    source directory is selected and at least one declares a manifest, the
    manifests must together list every header under that directory's
    ``include/``; a directory that also holds other units' targets
    (``core/view``) cannot be checked by directory and relies on the list.
    """
    selected = set(names)
    by_dir: dict[str, set[str]] = {}
    for data in targets.values():
        directory = (data.get("paths") or {}).get("source")
        if directory and data.get("name"):
            by_dir.setdefault(directory, set()).add(data["name"])
    files: set[Path] = set()
    declared: dict[str, list[str]] = {}
    covered: dict[Path, set[Path]] = {}
    problems: list[str] = []
    for name in sorted(selected):
        directory, headers = manifests.get(name, (Path(), []))
        if not headers:
            continue
        rels = []
        for header in headers:
            path = (directory / header).resolve()
            if not path.is_file():
                problems.append(f"{name}: PULP_UNIT_HEADERS lists {header}, which does not exist")
                continue
            files.add(path)
            covered.setdefault(directory.resolve(), set()).add(path)
            rels.append(path.relative_to(source).as_posix())
        declared[name] = sorted(rels)
    for directory, listed in sorted(covered.items()):
        try:
            rel_dir = directory.relative_to(source).as_posix()
        except ValueError:
            continue
        if by_dir.get(rel_dir, set()) - selected:
            continue
        include_root = directory / "include"
        present = {p.resolve() for p in include_root.rglob("*") if p.is_file() and p.suffix.lower() in HEADER_EXTS} \
            if include_root.is_dir() else set()
        unlisted = sorted(p.relative_to(source).as_posix() for p in present - listed)
        problems += [f"{rel_dir}: public header {rel} is not in any PULP_UNIT_HEADERS" for rel in unlisted]
    if problems:
        raise SystemExit("unit header manifests are inconsistent:\n  " + "\n  ".join(problems))
    return files, declared


def fetchcontent_pins(source: Path) -> dict[str, dict]:
    """Return repository and checked-in commit pins keyed by dependency name.

    The pin is the 40-hex commit the dependency must be built from: the
    manifest's ``upstream.commit`` when it records one, otherwise the full hash
    ``setup.sh`` seeds the shared cache with, otherwise a full hash in the
    manifest version. A dependency known only by tag keeps that tag, and the
    gate refuses any row that names it.
    """
    manifest = source / "tools" / "deps" / "manifest.json"
    try:
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        rows = payload.get("dependencies", []) if isinstance(payload, dict) else payload
    except (OSError, json.JSONDecodeError):
        return {}
    pins: dict[str, dict] = {}
    setup_text = ""
    try:
        setup_text = (source / "setup.sh").read_text(encoding="utf-8")
    except OSError:
        pass
    for row in rows:
        name = str(row.get("name", "")).lower()
        determinism = row.get("determinism") if isinstance(row.get("determinism"), dict) else {}
        if name == "skia" and FULL_SHA_RE.fullmatch(str(determinism.get("skia_commit", ""))):
            # The prebuilt toolchain is identified by the Skia commit it was
            # built from plus the sha256 of the release asset that was unpacked.
            assets = sorted({
                str(asset.get("sha256", "")).lower()
                for asset in (determinism.get("release_assets") or {}).values()
                if isinstance(asset, dict) and SHA256_RE.fullmatch(str(asset.get("sha256", "")).lower())
            })
            pins[name] = {"source": str(row.get("repository")), "pin": determinism["skia_commit"], "assets": assets}
            continue
        if row.get("source_kind") != "fetchcontent":
            continue
        repository = row.get("repository")
        upstream = row.get("upstream") if isinstance(row.get("upstream"), dict) else {}
        match = re.search(r"\b[0-9a-f]{40}\b", str(row.get("version", "")))
        pin = match.group(0) if match else str(upstream.get("ref", ""))
        setup_match = re.search(
            rf'ensure_shared_git_source_with_retry\s+"{re.escape(name)}"[^\n]*\n\s+"([0-9a-f]{{40}})"',
            setup_text,
            re.IGNORECASE,
        )
        if setup_match:
            pin = setup_match.group(1)
        if FULL_SHA_RE.fullmatch(str(upstream.get("commit", ""))):
            pin = str(upstream["commit"])
        if name and repository and pin:
            pins[name] = {"source": str(repository), "pin": pin}
    return pins


def _compact(value: str) -> str:
    return re.sub(r"[^a-z0-9]", "", value.lower())


def dependency_slug(name: str) -> str:
    """Stable path segment for a dependency name such as ``mbed tls``."""
    return re.sub(r"[^a-z0-9._-]+", "-", name.lower()).strip("-")


def dependency_key(name: str, pins: dict[str, dict]) -> str:
    """Map a FetchContent or cache directory name onto its manifest row."""
    wanted = _compact(name)
    if not wanted:
        return ""
    for candidate in pins:
        if _compact(candidate) == wanted or dependency_slug(candidate) == name.lower():
            return candidate
    prefixed = [candidate for candidate in pins if _compact(candidate).startswith(wanted)]
    return prefixed[0] if len(prefixed) == 1 else ""


def cmake_cache_entries(build: Path) -> dict[str, str]:
    """Return ``NAME:TYPE=value`` entries from a build's CMakeCache.txt."""
    try:
        text = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    return {m.group(1): m.group(2).strip() for m in re.finditer(r"^([^#/\s][^:=]*):[A-Z]+=(.*)$", text, re.M)}


def _text_sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _try_run(*cmd: str, env: dict | None = None) -> str:
    try:
        return subprocess.check_output(cmd, text=True, encoding="utf-8", stderr=subprocess.DEVNULL, env=env).strip()
    except (subprocess.CalledProcessError, OSError):
        return ""


def _dpkg_owner(path: str) -> tuple[str, str] | None:
    """The single dpkg package that owns ``path`` and its version."""
    owners = _try_run("dpkg-query", "-S", path)
    lines = [line for line in owners.splitlines() if line.endswith(": " + path) and not line.startswith("diversion")]
    if len(lines) != 1:
        return None
    packages = [p.strip() for p in lines[0].rsplit(": ", 1)[0].split(",")]
    if len(packages) != 1 or not packages[0]:
        return None
    version = _try_run("dpkg-query", "-W", "-f", "${Version}", packages[0])
    return (packages[0].split(":", 1)[0], version) if version else None


def _rpm_owner(path: str) -> tuple[str, str] | None:
    """The single rpm package that owns ``path`` and its version."""
    owners = _try_run("rpm", "-qf", "--queryformat", "%{NAME}\t%{VERSION}-%{RELEASE}\n", path).splitlines()
    rows = [line.split("\t") for line in owners if "\t" in line]
    return (rows[0][0], rows[0][1]) if len(rows) == 1 and all(rows[0]) else None


PKG_CONFIG_SYSTEM_DIRS = ("/usr/lib", "/usr/lib64", "/usr/share/pkgconfig", "/usr/local/lib", "/usr/local/share/pkgconfig")


def _pkg_config_owner(path: str, module: str) -> tuple[str, str] | None:
    """A pkg-config module that declares the directory holding ``path``.

    Only the system search path counts: ``PKG_CONFIG_PATH`` and friends are
    removed so an environment cannot supply a module, and the module's ``.pc``
    file must itself live in a system pkgconfig directory.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("PKG_CONFIG")}
    pc_dir = _try_run("pkg-config", "--variable=pcfiledir", module, env=env)
    if not pc_dir or not pc_dir.startswith(PKG_CONFIG_SYSTEM_DIRS):
        return None
    dirs = [flag[2:] for flag in _try_run("pkg-config", "--cflags-only-I", module, env=env).split() if flag.startswith("-I")]
    dirs.append(_try_run("pkg-config", "--variable=includedir", module, env=env))
    if not any(d and (path == d or path.startswith(d.rstrip("/") + "/")) for d in dirs):
        return None
    version = _try_run("pkg-config", "--modversion", module, env=env)
    return (module, version) if version else None


def system_identity(raw: str, probe=None) -> dict[str, str] | None:
    """Identify a toolchain or operating-system path by who owns it.

    System headers are not Pulp inputs and carry no Pulp pin, but a closure
    records the package that owns them: ``@system/<package>/<version>/<path>``.
    Ownership comes from the package database (dpkg, then rpm) for the exact
    path; a pkg-config module counts only when no package database owns the
    path and the module itself declares the directory. A Windows, MSVC or
    Apple SDK path is owned by the SDK it names. A path that does not exist,
    or that nothing owns, has no identity. ``probe`` replaces the host
    queries in tests.
    """
    value = raw.replace("\\", "/")
    probe = probe or {}
    exists = probe.get("exists", lambda p: Path(p).exists())
    windows_sdk = re.search(r"/Windows Kits/\d+/Include/(?P<version>[0-9.]+)(?:/(?P<rest>.*))?$", value, re.I)
    msvc = re.search(r"/VC/Tools/MSVC/(?P<version>[0-9.]+)(?:/(?P<rest>.*))?$", value, re.I)
    apple_sdk = re.search(r"/SDKs/(?P<platform>[A-Za-z]+?)(?P<version>[0-9.]+)\.sdk(?:/(?P<rest>.*))?$", value)
    unix = re.match(r"^/usr(?:/local)?/include(?:/(?P<rest>.*))?$", value)
    if not (windows_sdk or msvc or apple_sdk or unix) or not exists(raw):
        return None
    if windows_sdk:
        return _system_row("windows-sdk", windows_sdk.group("version"), windows_sdk.group("rest") or "", "windows-sdk")
    if msvc:
        return _system_row("msvc", msvc.group("version"), msvc.group("rest") or "", "msvc")
    if apple_sdk:
        return _system_row(f"{apple_sdk.group('platform').lower()}-sdk", apple_sdk.group("version"), apple_sdk.group("rest") or "", "apple-sdk")
    rest = unix.group("rest") or ""
    owner, provider = probe.get("dpkg", _dpkg_owner)(value), "dpkg"
    if not owner:
        owner, provider = probe.get("rpm", _rpm_owner)(value), "rpm"
    if not owner and rest:
        owner, provider = probe.get("pkg-config", _pkg_config_owner)(value, rest.split("/", 1)[0]), "pkg-config"
    if not owner:
        return None
    return _system_row(owner[0], owner[1], rest, provider)


def _system_row(package: str, version: str, rest: str, provider: str) -> dict[str, str]:
    version = re.sub(r"[^A-Za-z0-9.+~_-]", "_", version)
    path = f"@system/{dependency_slug(package)}/{version}" + (f"/{rest}" if rest else "")
    return {"path": path, "source": "system", "pin": version, "package": dependency_slug(package), "provider": provider}


def git_blob_sha256(root: Path, commit: str, rels: Iterable[str]) -> dict[str, str]:
    """sha256 of each ``commit:rel`` blob in a checkout; missing blobs are omitted."""
    rels = list(dict.fromkeys(rels))
    if not rels:
        return {}
    try:
        out = subprocess.run(["git", "--no-optional-locks", "-C", str(root), "cat-file", "--batch"],
                             input="".join(f"{commit}:{rel}\n" for rel in rels).encode("utf-8"),
                             capture_output=True, check=True).stdout
    except (subprocess.CalledProcessError, OSError):
        return {}
    digests: dict[str, str] = {}
    offset = 0
    for rel in rels:
        end = out.index(b"\n", offset)
        header = out[offset:end].split()
        offset = end + 1
        if len(header) == 3 and header[1] == b"blob":
            size = int(header[2])
            digests[rel] = hashlib.sha256(out[offset:offset + size]).hexdigest()
            offset += size + 1
    return digests


def tree_sha256(root: Path, exclude: Iterable[str] = ()) -> str:
    """Digest of every file under ``root``: its relative path and its bytes."""
    skip = set(exclude)
    h = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file() and not p.is_symlink()):
        rel = path.relative_to(root).as_posix()
        if rel not in skip:
            h.update(f"{rel}\0{sha256(path)}\n".encode("utf-8"))
    return h.hexdigest()


PATCH_RECIPE_ID = "pulp.vellum.patch-id.v1"
PATCH_RECIPE_FORMULA = ("sha256(utf8('patch-set:' + patch_set.key + '\\nbase-modification:' + base_modification"
                        " + '\\ncopy-tree:' + copy_tree + '\\n'))")


def patch_id(key: str, base_modification: str, copy_tree: str) -> str:
    """The patch identity ``PATCH_RECIPE_ID`` defines over its recorded inputs."""
    return _text_sha256(f"patch-set:{key}\nbase-modification:{base_modification}\ncopy-tree:{copy_tree}\n")


def git_blob_id(data: bytes) -> str:
    """The git blob id of ``data``, as ``git hash-object`` computes it."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def patch_set_source(source: Path, key: str) -> dict | None:
    """The repository patch-set file whose sha256 is the copy's patch-set key."""
    for candidate in sorted((source / "tools/cmake").glob("PulpPatch*.cmake")):
        data = candidate.read_bytes()
        if hashlib.sha256(data).hexdigest() == key:
            return {"pulp_path": candidate.relative_to(source).as_posix(), "pulp_blob": git_blob_id(data), "key": key}
    return None


def patch_violations(row: dict, source: Path | None) -> list[str]:
    """A patched row must carry, and recompute from, every input of its id.

    With the repository at hand the patch-set file must still hash to the
    recorded key and blob.
    """
    path = row.get("path")
    patch_set = row.get("patch_set")
    recipe = row.get("patch_recipe")
    base = row.get("base_modification")
    copy = row.get("copy_tree")
    if not isinstance(patch_set, dict) or not isinstance(recipe, dict) or recipe.get("id") != PATCH_RECIPE_ID \
            or not SHA256_RE.fullmatch(str(patch_set.get("key", ""))) or not re.fullmatch(r"[0-9a-f]{40}", str(patch_set.get("pulp_blob", ""))) \
            or not isinstance(patch_set.get("pulp_path"), str) or host_path(patch_set["pulp_path"]) \
            or not (base == "" or SHA256_RE.fullmatch(str(base))) or not SHA256_RE.fullmatch(str(copy)):
        return [f"patched external input lacks its patch-set provenance {path}"]
    violations: list[str] = []
    if row.get("patch") != patch_id(patch_set["key"], base, copy):
        violations.append(f"patched external input id does not recompute from its recorded inputs {path}")
    replay = recipe.get("replay")
    if not isinstance(replay, dict) or replay.get("method") != PATCH_REPLAY_METHOD \
            or replay.get("patch_set_blob") != patch_set["pulp_blob"] \
            or not isinstance(replay.get("functions"), list) or not replay["functions"] \
            or not all(isinstance(f, str) and re.fullmatch(r"pulp_patch_[A-Za-z0-9_]+", f) for f in replay["functions"]):
        violations.append(f"patched external input does not record a complete replay {path}")
    if source is not None:
        candidate = source / patch_set["pulp_path"]
        if candidate.is_file():
            data = candidate.read_bytes()
            if hashlib.sha256(data).hexdigest() != patch_set["key"] or git_blob_id(data) != patch_set["pulp_blob"]:
                violations.append(f"patch-set source does not match its recorded key and blob {path}")
            elif isinstance(replay, dict) and replay.get("functions") != patch_functions(candidate):
                violations.append(f"replay functions are not the patch set's functions in order {path}")
        else:
            violations.append(f"patch-set source is missing {patch_set['pulp_path']} {path}")
    return violations


PATCH_REPLAY_METHOD = ("export the pinned commit with `git archive <pin>`; in a CMake -P script, include the "
                       "patch-set file whose git blob is patch_set_blob and call each name in functions, in "
                       "order, with the exported tree's root; copy_tree is the digest of the result")


def patch_functions(patch_file: Path) -> list[str]:
    """The ``pulp_patch_*`` functions a patch-set file defines, in definition order."""
    return re.findall(r"^\s*function\(\s*(pulp_patch_[A-Za-z0-9_]+)",
                      patch_file.read_text(encoding="utf-8", errors="replace"), re.M)


def patch_replay(patch_file: Path, blob: str) -> dict:
    """The replay a patched row records: everything needed to rebuild its bytes."""
    return {"method": PATCH_REPLAY_METHOD, "patch_set_blob": blob, "functions": patch_functions(patch_file)}


def replay_patch_set(base: Path, pin: str, patch_file: Path, cmake: str = "cmake") -> dict | None:
    """The tree a patched copy must hold: ``pin`` from ``base``'s object store
    with the patch set replayed. Returns its digest and per-file digests."""
    if not (base / ".git").exists() or not patch_file.is_file():
        return None
    functions = patch_functions(patch_file)
    import tarfile
    import tempfile
    with tempfile.TemporaryDirectory(prefix="pulp-patch-replay-") as td:
        tree = Path(td) / "tree"
        tree.mkdir()
        try:
            commit = run("git", "--no-optional-locks", "-C", str(base), "rev-parse", f"{pin}^{{commit}}")
            archive = subprocess.run(["git", "--no-optional-locks", "-C", str(base), "archive", "--format=tar", commit],
                                     capture_output=True, check=True).stdout
        except (subprocess.CalledProcessError, OSError):
            return None
        with tarfile.open(fileobj=__import__("io").BytesIO(archive)) as tar:
            if hasattr(tarfile, "data_filter"):
                tar.extractall(tree, filter="data")
            else:
                tar.extractall(tree)
        script = Path(td) / "replay.cmake"
        script.write_text(f'include("{patch_file.as_posix()}")\n' + "".join(
            f'{name}("{tree.as_posix()}")\n' for name in functions), encoding="utf-8")
        try:
            subprocess.run([cmake, "-P", str(script)], capture_output=True, check=True)
        except (subprocess.CalledProcessError, OSError):
            return None
        files = {path.relative_to(tree).as_posix(): sha256(path)
                 for path in tree.rglob("*") if path.is_file() and not path.is_symlink()}
        return {"commit": commit, "tree": tree_sha256(tree), "files": files}


class ExternalResolver:
    """Assign every build-machine input a host-independent identity.

    A FetchContent file becomes ``@external/<dependency>/<commit>/<path>``.
    The checkout must hold exactly the checked-in commit; each file row also
    records the sha256 of the bytes that were compiled. A source whose bytes
    differ from that commit (a Pulp-patched build-local copy, or a modified
    checkout) is recorded as ``patched`` under
    ``@external/<dependency>/<commit>+patch-<id>/``, where the patch identity
    covers the patch set and any local modification. A checkout that cannot
    prove its commit is left ``unresolved`` so the gate rejects it.

    The prebuilt Skia toolchain is keyed by its Skia commit and the release
    asset sealed in the cache generation's receipt; each file must match the
    receipt's digest. Toolchain and operating-system headers get an
    ``@system/`` identity.
    """

    def __init__(self, source: Path, build: Path | None, pins: dict[str, dict] | None = None, system_probe=None):
        self.source = source.resolve()
        self.build = build.resolve() if build else None
        self.pins = fetchcontent_pins(source) if pins is None else pins
        cache = cmake_cache_entries(build) if build else {}
        self.roots: dict[Path, str] = {}
        for key, value in cache.items():
            if key.startswith("FETCHCONTENT_SOURCE_DIR_") and value:
                self.roots[Path(value).resolve()] = key.removeprefix("FETCHCONTENT_SOURCE_DIR_").lower()
        self.system_probe = system_probe
        self.cmake = cache.get("CMAKE_COMMAND") or shutil.which("cmake") or "cmake"
        self._states: dict[Path, dict] = {}
        self._skia: dict[Path, dict | None] = {}

    def _git_state(self, root: Path, pin: str) -> dict | None:
        """Commit and local-modification identity of a git checkout.

        A checkout that converts line endings on checkout (``core.autocrlf
        true``) holds bytes that are not its commit's blobs, so it is never
        identified.
        """
        if not (root / ".git").exists():
            return None
        # Shared caches are read by concurrent builds: never take the index
        # lock, and treat a failed query as unproven rather than clean.
        git = ("git", "--no-optional-locks", "-C", str(root))
        try:
            head = run(*git, "rev-parse", "HEAD")
            target = run(*git, "rev-parse", f"{pin}^{{commit}}")
            diff = run(*git, "diff", "--binary", "HEAD")
            tracked = set(run(*git, "ls-files", "-z").split("\0"))
            autocrlf = _try_run(*git, "config", "--get", "core.autocrlf").lower()
        except (subprocess.CalledProcessError, OSError):
            return None
        if not FULL_SHA_RE.fullmatch(head) or head != target or autocrlf == "true":
            return None
        return {"commit": head, "modification": _text_sha256(diff) if diff else "", "tracked": tracked}

    def _state(self, root: Path, pin: str) -> dict | None:
        """Identity of the bytes under one dependency root, or None."""
        if root in self._states:
            return self._states[root]
        state = None
        if FULL_SHA_RE.fullmatch(pin):
            stamp = root / ".pulp-source-key"
            if stamp.is_file() and not (root / ".git").exists():
                # A build-local copy that Pulp patches: its stamp names the
                # shared checkout it was copied from and the patch-set key.
                lines = stamp.read_text(encoding="utf-8", errors="replace").splitlines()
                key = lines[1].strip().lower() if len(lines) >= 2 else ""
                source_of_patch = patch_set_source(self.source, key) if SHA256_RE.fullmatch(key) else None
                expected = (replay_patch_set(Path(lines[0]).resolve(), pin, self.source / source_of_patch["pulp_path"], self.cmake)
                            if source_of_patch and lines[0] else None)
                if expected:
                    # The copy's identity comes from the pinned commit in the
                    # base's object store with the patch set replayed onto it,
                    # so the base's working tree never affects it. Each row is
                    # then checked against the bytes that were compiled.
                    state = {"commit": expected["commit"], "tracked": None, "expected": expected["files"],
                             "patch": patch_id(key, "", expected["tree"]),
                             "patch_fields": {"patch_set": source_of_patch, "base_modification": "",
                                              "copy_tree": expected["tree"],
                                              "patch_recipe": {"id": PATCH_RECIPE_ID, "formula": PATCH_RECIPE_FORMULA,
                                                               "replay": patch_replay(self.source / source_of_patch["pulp_path"], source_of_patch["pulp_blob"])}}}
            else:
                base = self._git_state(root, pin)
                # A modified checkout has no recorded patch set to rebuild it
                # from, so it cannot be identified.
                if base and not base["modification"]:
                    state = {"commit": base["commit"], "tracked": base["tracked"], "root": root, "patch": ""}
        self._states[root] = state
        return state

    def _locate(self, path: Path) -> tuple[str, Path, str] | None:
        if self.build:
            try:
                rel = path.relative_to(self.build / "_deps")
            except ValueError:
                rel = None
            if rel is not None and rel.parts:
                match = re.fullmatch(r"(?:pulp-mutable-)?(?P<name>.+?)-src", rel.parts[0])
                if match:
                    return match.group("name"), self.build / "_deps" / rel.parts[0], Path(*rel.parts[1:]).as_posix() if len(rel.parts) > 1 else ""
        for root, name in self.roots.items():
            try:
                rel = path.relative_to(root)
            except ValueError:
                continue
            return name, root, "" if rel.as_posix() == "." else rel.as_posix()
        return None

    def _skia_root(self, path: Path) -> tuple[Path, str] | None:
        for parent in (path, *path.parents):
            if SKIA_CACHE_RE.fullmatch(parent.name) and (parent / SKIA_RECEIPT).is_file():
                rel = path.relative_to(parent).as_posix()
                return parent, "" if rel == "." else rel
        return None

    def _skia_receipt(self, root: Path) -> dict | None:
        if root not in self._skia:
            receipt = None
            try:
                data = json.loads((root / SKIA_RECEIPT).read_text(encoding="utf-8"))
                stamp = (root / SKIA_ASSET_STAMP).read_text(encoding="utf-8").strip()
                asset = SKIA_CACHE_RE.fullmatch(root.name).group("asset")
                if data.get("asset_sha256") == asset == stamp:
                    receipt = {"asset": asset, "files": {row["path"]: row["sha256"] for row in data.get("files", []) if isinstance(row, dict)}}
            except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
                receipt = None
            self._skia[root] = receipt
        return self._skia[root]

    def identify(self, raw: str, digest: bool = True) -> dict[str, str]:
        """Return the external-input row for an absolute or ``@build/`` path."""
        value = raw.replace("\\", "/")
        system = system_identity(value, self.system_probe)
        if system:
            system_path = Path(value)
            return system | ({"sha256": sha256(system_path)} if digest and system_path.is_file() else {})
        if value.startswith("@build/") and self.build:
            path = (self.build / value.removeprefix("@build/")).resolve()
        else:
            path = Path(value).resolve() if Path(value).is_absolute() else Path(value)
        unresolved = {"path": value, "source": "build-tree", "pin": "unresolved"}
        file_digest = sha256(path) if digest and path.is_file() else ""
        skia_root = self._skia_root(path)
        metadata = self.pins.get("skia")
        if skia_root and metadata:
            root, rest = skia_root
            receipt = self._skia_receipt(root)
            if not receipt or (file_digest and receipt["files"].get(rest) != file_digest):
                return unresolved
            row = {"path": f"@external/skia/{metadata['pin']}" + (f"/{rest}" if rest else ""),
                   "source": metadata["source"], "pin": metadata["pin"], "asset_sha256": receipt["asset"]}
            return row | ({"sha256": file_digest} if file_digest else {})
        located = self._locate(path)
        key = dependency_key(located[0], self.pins) if located else ""
        if not located or not key:
            return unresolved
        _name, root, rel = located
        metadata = self.pins[key]
        state = self._state(root, str(metadata["pin"]))
        unproven = {"path": value, "source": metadata["source"], "pin": "unresolved"}
        if not state:
            reason = ("the patched copy's pin or patch set cannot be replayed from its base checkout"
                      if (root / ".pulp-source-key").is_file() else
                      "the checkout is not at its pin, is modified, or converts line endings")
            return unproven | {"reason": reason}
        if rel and path.is_file() and state["tracked"] is not None and rel not in state["tracked"]:
            # An untracked file in a checkout is not part of the pinned commit.
            return unproven | {"reason": "the file is not tracked at the pin"}
        if state["patch"] and file_digest and state["expected"].get(rel) != file_digest:
            return unproven | {"reason": "the bytes differ from the pin with its patch set replayed"}
        if not state["patch"] and file_digest and git_blob_sha256(state["root"], state["commit"], [rel]).get(rel) != file_digest:
            # A clean checkout's file must be the pinned blob byte for byte; a
            # difference git status hides (assume-unchanged) is not clean.
            return unproven | {"reason": "the bytes differ from the pinned blob"}
        marker = state["commit"] + (f"+patch-{state['patch'][:16]}" if state["patch"] else "")
        row = {"path": f"@external/{dependency_slug(key)}/{marker}" + (f"/{rel}" if rel else ""),
               "source": metadata["source"], "pin": state["commit"]}
        if state["patch"]:
            row |= {"patched": True, "patch": state["patch"], **state["patch_fields"]}
        return row | ({"sha256": file_digest} if file_digest else {})

    def external_link_input(self, path: Path) -> dict | None:
        """The prebuilt Skia toolchain an include directory reaches, as a link input."""
        located = self._skia_root(path)
        metadata = self.pins.get("skia")
        if not located or not metadata:
            return None
        receipt = self._skia_receipt(located[0])
        if not receipt:
            return None
        return {"dependency": "skia", "source": metadata["source"], "pin": metadata["pin"], "asset_sha256": receipt["asset"]}

    def portable(self, raw: str) -> str:
        """Express an include directory without host-specific prefixes."""
        system = system_identity(raw.replace("\\", "/"), self.system_probe)
        if system:
            return system["path"]
        path = Path(raw)
        if not path.is_absolute():
            path = self.source / path
        path = path.resolve()
        if self.build:
            try:
                return "@build/" + path.relative_to(self.build).as_posix()
            except ValueError:
                pass
        try:
            rel = path.relative_to(self.source).as_posix()
            return "." if rel == "." else rel
        except ValueError:
            return self.identify(str(path), digest=False)["path"]


def _npm_lock_pin(path: str) -> str:
    """The lockfile digest that pins an npm row, or "" when it has none."""
    if "/node_modules/" not in path:
        return ""
    lock = Path(__file__).resolve().parents[2] / path.split("/node_modules/", 1)[0] / "package-lock.json"
    return "sha256:" + sha256(lock) if lock.is_file() else ""


def external_input_violations(data: dict) -> list[str]:
    """Require every external row to carry a verified immutable identity.

    A FetchContent row needs the manifest's repository, the checked-in 40-hex
    commit, and a path marker naming that commit; a patched row also needs its
    patch identity and the digest of its bytes. The Skia row needs a release
    asset the manifest lists. A system row needs a package and version. npm
    rows need the digest of the lockfile they come from.
    """
    source_root = data.get("source_root")
    if source_root in (".", None):
        source_root = str(Path(__file__).resolve().parents[2])
    pins = fetchcontent_pins(Path(source_root))
    if not pins:
        pins = fetchcontent_pins(Path(__file__).resolve().parents[2])
    slugs = {dependency_slug(name): name for name in pins}
    violations: list[str] = []
    clean_rows: list[tuple[str, str, dict]] = []
    for row in data.get("external_inputs", []):
        if not isinstance(row, dict):
            violations.append("external input is not an object")
            continue
        path = str(row.get("path", ""))
        source = row.get("source")
        pin = row.get("pin")
        reason = f" ({row['reason']})" if isinstance(row.get("reason"), str) else ""
        if host_path(path):
            violations.append(f"external input is a host path {path}{reason}")
            continue
        if source == "npm":
            expected_lock = _npm_lock_pin(path)
            if path.startswith("@") or not expected_lock or pin != expected_lock:
                violations.append(f"npm external input does not match its lockfile digest {path}: {pin!r}")
            continue
        if source == "system":
            system = SYSTEM_RE.match(path)
            if not system or not pin or system.group("version") != pin or row.get("package") != system.group("package") \
                    or row.get("provider") not in SYSTEM_PROVIDERS:
                violations.append(f"system input lacks an ownership identity {path}")
            elif not SHA256_RE.fullmatch(str(row.get("sha256", ""))):
                violations.append(f"system input has no content digest {path}")
            continue
        if source == "build-tree" or pin == "unresolved":
            violations.append(f"unresolved build-tree external input {path}{reason}")
            continue
        marker = EXTERNAL_RE.match(path)
        legacy = BUILD_DEPS_RE.match(path)
        if marker:
            name = slugs.get(marker.group("slug"), "")
        elif legacy:
            name = dependency_key(legacy.group("name"), pins)
        else:
            violations.append(f"external input has no dependency identity {path}")
            continue
        expected = pins.get(name)
        if expected is None:
            violations.append(f"unknown FetchContent external input {path}")
            continue
        if source != expected["source"]:
            violations.append(f"external input repository mismatch {path}: {source!r}")
        declared = str(expected["pin"])
        if not isinstance(pin, str) or not FULL_SHA_RE.fullmatch(pin):
            violations.append(f"external input pin is not a full 40-hex SHA {path}: {pin!r}")
        elif not FULL_SHA_RE.fullmatch(declared):
            violations.append(f"dependency {name} has no checked-in 40-hex commit (declared {declared!r}) {path}")
        elif pin != declared:
            violations.append(f"external input pin mismatch {path}: {pin}")
        if (marker or row.get("sha256") is not None) and not SHA256_RE.fullmatch(str(row.get("sha256", ""))):
            violations.append(f"external input has no sha256 content digest {path}")
        if marker:
            patch = row.get("patch")
            expected_marker = f"{pin}+patch-{str(patch)[:16]}" if row.get("patched") else pin
            if marker.group("pin") != expected_marker:
                violations.append(f"external input path pin differs from its pin field {path}")
            if row.get("patched") and (not SHA256_RE.fullmatch(str(patch)) or not SHA256_RE.fullmatch(str(row.get("sha256", "")))):
                violations.append(f"patched external input needs a patch identity and a content digest {path}")
            elif row.get("patched"):
                violations.extend(patch_violations(row, Path(source_root)))
            elif any(key in row for key in ("patch", "patch_set", "base_modification", "copy_tree", "patch_recipe")):
                violations.append(f"unpatched external input carries patch fields {path}")
        if "assets" in expected and row.get("asset_sha256") not in expected["assets"]:
            violations.append(f"external input release asset is not a manifest asset {path}: {row.get('asset_sha256')!r}")
        if marker and not row.get("patched") and "assets" not in expected and FULL_SHA_RE.fullmatch(str(pin)):
            clean_rows.append((name, path[marker.end():], row))
    violations.extend(_clean_content_violations(data, clean_rows))
    return violations


def _clean_content_violations(data: dict, rows: list[tuple[str, str, dict]]) -> list[str]:
    """Check clean rows against their pinned blobs where the checkout exists.

    A row that claims a clean pin must hold that commit's bytes; this catches a
    patched file whose patch marker was stripped. Rows whose checkout is not on
    this host cannot be checked here and are left to the producer.
    """
    build = data.get("build_dir")
    if not rows or not build or not Path(str(build)).is_dir():
        return []
    resolver = ExternalResolver(Path(str(data.get("source_root") or Path(__file__).resolve().parents[2])), Path(str(build)))
    checkouts: dict[str, Path] = {}
    for root, name in resolver.roots.items():
        key = dependency_key(name, resolver.pins)
        if key and (root / ".git").exists():
            checkouts.setdefault(key, root)
    violations: list[str] = []
    by_dependency: dict[str, list[tuple[str, dict]]] = {}
    for name, rel, row in rows:
        if name in checkouts:
            by_dependency.setdefault(name, []).append((rel, row))
    for name, entries in by_dependency.items():
        pin = entries[0][1]["pin"]
        blobs = git_blob_sha256(checkouts[name], pin, [rel for rel, _row in entries])
        for rel, row in entries:
            if blobs.get(rel) is None or row.get("sha256") != blobs[rel]:
                violations.append(f"external input bytes differ from its pinned commit {row.get('path')}")
    return violations


def host_path(value: str) -> bool:
    """True when a string carries a host filesystem location."""
    return bool(HOST_PATH_RE.search(value) or HOST_EMBEDDED_RE.search(value) or PARENT_SEGMENT_RE.search(value))


def receipt_path_violations(document: dict) -> list[str]:
    """Reject host paths anywhere in a receipt or written closure manifest.

    ``source_root`` and ``build_dir`` are the only host fields a working
    manifest may carry; export clears both before it writes a closure.
    """
    violations: list[str] = []

    def walk(value, key: str) -> None:
        if isinstance(value, dict):
            for child_key, child in value.items():
                walk(child, child_key)
        elif isinstance(value, list):
            for child in value:
                walk(child, key)
        elif isinstance(value, str) and host_path(value):
            violations.append(f"host path in {key}: {value}")

    for key, value in document.items():
        if key not in {"source_root", "build_dir"}:
            walk(value, key)
    return violations


BUILD_TIMESTAMP_PLACEHOLDER = "<build-timestamp>"


def generated_digest(rel: str, path: Path, source: Path) -> dict | None:
    """A generated input's digest that does not depend on when or whether it was built.

    ``build_info.hpp`` is hashed with its configure timestamp replaced by
    ``BUILD_TIMESTAMP_PLACEHOLDER``; a binary-data source is hashed by
    running its encoder on the recorded font input, so the digest is the same
    before and after a build. Returns None for any other input.
    """
    if re.fullmatch(r"@build/core/runtime/generated/(?:[^/]+/)?pulp/runtime/build_info\.hpp", rel):
        if not path.is_file():
            return None
        text = path.read_text(encoding="utf-8")
        stamp = re.search(r'kBuildIso8601\s*=\s*"([^"]*)"', text)
        if stamp and stamp.group(1):
            text = text.replace(stamp.group(1), BUILD_TIMESTAMP_PLACEHOLDER)
        return {"sha256": _text_sha256(text), "normalization": {"build_timestamp": BUILD_TIMESTAMP_PLACEHOLDER}}
    data = re.fullmatch(r"@build/core/canvas/(?P<target>pulp-bundled-[A-Za-z0-9-]+?)_data_(?P<var>[A-Za-z0-9_]+)\.cpp", rel)
    if not data:
        return None
    namespaces = canvas_binary_namespaces(source)
    inputs = canvas_binary_data(source).get(data.group("target"), [])
    font = next((f for f in inputs if c_identifier(Path(f).name) == data.group("var")), None)
    encoder = source / "tools/cmake/scripts/encode_binary_data.py"
    if not font or data.group("target") not in namespaces or not encoder.is_file():
        return None
    import tempfile
    with tempfile.TemporaryDirectory(prefix="pulp-binary-data-") as td:
        out = Path(td) / "out.cpp"
        try:
            subprocess.run([sys.executable, str(encoder), "--src", str(source / font), "--out", str(out),
                            "--namespace", namespaces[data.group("target")], "--var", data.group("var")],
                           check=True, capture_output=True)
        except (subprocess.CalledProcessError, OSError):
            return None
        return {"sha256": sha256(out)}


def c_identifier(name: str) -> str:
    """CMake's string(MAKE_C_IDENTIFIER) for a file name."""
    ident = re.sub(r"[^A-Za-z0-9_]", "_", name)
    return f"_{ident}" if ident[:1].isdigit() else ident


def canvas_binary_namespaces(source: Path) -> dict[str, str]:
    """The NAMESPACE each pulp_add_binary_data call in core/canvas declares."""
    try:
        text = (source / "core/canvas/CMakeLists.txt").read_text(encoding="utf-8")
    except OSError:
        return {}
    result: dict[str, str] = {}
    for match in re.finditer(r"pulp_add_binary_data\(\s*(pulp-bundled-[A-Za-z0-9-]+)(.*?)\)", text, re.S):
        namespace = re.search(r"\bNAMESPACE\s+(\S+)", match.group(2))
        if namespace:
            result[match.group(1)] = namespace.group(1)
    return result


def canvas_binary_data(source: Path) -> dict[str, list[str]]:
    """Binary-data targets ``core/canvas/CMakeLists.txt`` declares, with their inputs."""
    try:
        text = (source / "core/canvas/CMakeLists.txt").read_text(encoding="utf-8")
    except OSError:
        return {}
    return {match.group(1): sorted(re.findall(r"\$\{PULP_ROOT_DIR\}/(\S+)", match.group(2)))
            for match in re.finditer(r"pulp_add_binary_data\(\s*(pulp-bundled-[A-Za-z0-9-]+)(.*?)\)", text, re.S)}


def generated_producers(rel: str, source: Path) -> list[str] | None:
    """The repository files that generate one build-tree input.

    Returns None for an input nothing in the repository generates: either a
    dependency's own build output (see ``generated_dependency``) or an input
    with no registered generator, which the gate rejects.
    """
    if rel.startswith("@build/core/runtime/generated/"):
        return ["core/runtime/CMakeLists.txt", "core/runtime/include/pulp/runtime/build_info.hpp.in"]
    if re.fullmatch(r"@build/core/view/web_compat_preludes_gen\.(?:hpp|cpp)", rel):
        return ["core/view/cmake/PulpViewJsPreludes.cmake", "core/view/js/embed_js.py"]
    # pulp_add_binary_data writes one header and one source per embedded file.
    data = re.fullmatch(r"@build/core/canvas/(?P<target>pulp-bundled-[A-Za-z0-9-]+?)_data(?:_[A-Za-z0-9_]+)?\.(?:hpp|cpp)", rel)
    if data and data.group("target") in canvas_binary_data(source):
        return ["core/canvas/CMakeLists.txt", "tools/cmake/PulpEmbedData.cmake", "tools/cmake/scripts/encode_binary_data.py",
                *canvas_binary_data(source)[data.group("target")]]
    return None


def all_registered_producers(source: Path) -> set[str]:
    """Every repository file that is in a closure only as a producer."""
    probes = ("@build/core/runtime/generated/x", "@build/core/view/web_compat_preludes_gen.hpp",
              *(f"@build/core/canvas/{target}_data.hpp" for target in canvas_binary_data(source)))
    return {path for probe in probes for path in (generated_producers(probe, source) or [])}


def generated_dependency(rel: str, resolver: "ExternalResolver") -> dict | None:
    """The dependency whose own build generates ``rel``, such as SDL's revision header."""
    match = re.match(r"^@build/_deps/(?P<name>[^/]+?)-build/", rel)
    if not match:
        return None
    key = dependency_key(match.group("name"), resolver.pins)
    if not key:
        return None
    metadata = resolver.pins[key]
    return {"name": dependency_slug(key), "source": metadata["source"], "pin": metadata["pin"]}


def _cmake_cache_typed(build: Path) -> dict[str, tuple[str, str]]:
    try:
        text = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    return {m.group(1): (m.group(2), m.group(3).strip())
            for m in re.finditer(r"^([^#/\s][^:=]*):([A-Z]+)=(.*)$", text, re.M)}


def _truthy(value) -> bool:
    return str(value).strip().upper() in {"1", "ON", "TRUE", "YES", "Y"}


def configure_options_sha256(options: dict) -> str:
    return hashlib.sha256(json.dumps(options, sort_keys=True).encode("utf-8")).hexdigest()


def configure_record(build: Path) -> dict:
    """The configure a closure came from: its Pulp options and identity.

    Every ``PULP_*`` option (BOOL or STRING) is recorded, with the derived
    ``PULP_HAS_*`` results, the build type and the target system and
    processor, so a closure can be regenerated
    from the same options and checked against them. A value naming a host
    location is recorded as ``@host-path``.
    """
    cache = _cmake_cache_typed(build)
    options: dict[str, str] = {}
    for key, (kind, value) in cache.items():
        if (key.startswith("PULP_") and kind in {"BOOL", "STRING"}) or (key.startswith("PULP_HAS_") and kind == "INTERNAL") \
                or key == "CMAKE_BUILD_TYPE":
            options[key] = "@host-path" if host_path(value) else value
    system = {}
    for candidate in sorted((build / "CMakeFiles").glob("*/CMakeSystem.cmake")):
        text = candidate.read_text(encoding="utf-8", errors="replace")
        system = dict(re.findall(r'set\((CMAKE_SYSTEM_NAME|CMAKE_SYSTEM_PROCESSOR) "([^"]*)"\)', text))
    options.update(system)
    identity = {
        "gpu": _truthy(options.get("PULP_ENABLE_GPU")),
        "skia": _truthy(options.get("PULP_HAS_SKIA")),
        "text_shaping": _truthy(options.get("PULP_TEXT_SHAPING")),
        "build_type": options.get("CMAKE_BUILD_TYPE", ""),
        "system": system.get("CMAKE_SYSTEM_NAME", ""),
        "processor": system.get("CMAKE_SYSTEM_PROCESSOR", ""),
    }
    return {"options": options, "options_sha256": configure_options_sha256(options), "identity": identity}


def configure_violations(data: dict) -> list[str]:
    """A recorded configure must agree with its own options and its targets."""
    configure = data.get("configure")
    if configure is None:
        return []
    if not isinstance(configure, dict) or not isinstance(configure.get("options"), dict) or not isinstance(configure.get("identity"), dict):
        return ["configure record is malformed"]
    options, identity = configure["options"], configure["identity"]
    violations: list[str] = []
    if configure.get("options_sha256") != configure_options_sha256(options):
        violations.append("configure options digest does not match the recorded options")
    for field, option in (("gpu", "PULP_ENABLE_GPU"), ("skia", "PULP_HAS_SKIA"), ("text_shaping", "PULP_TEXT_SHAPING")):
        if bool(identity.get(field)) != _truthy(options.get(option)):
            violations.append(f"configure identity {field} disagrees with {option}")
    for field, option in (("build_type", "CMAKE_BUILD_TYPE"), ("system", "CMAKE_SYSTEM_NAME"), ("processor", "CMAKE_SYSTEM_PROCESSOR")):
        if identity.get(field) != options.get(option):
            violations.append(f"configure identity {field} disagrees with {option}")
    if data.get("configuration") and identity.get("build_type") != data["configuration"]:
        violations.append("configure identity build_type disagrees with the codemodel configuration")
    build = data.get("build_dir")
    if build and Path(str(build)).is_dir() and (Path(str(build)) / "CMakeCache.txt").is_file():
        # The closure's build tree is at hand: regenerate the record from it.
        current = configure_record(Path(str(build)))
        if current["identity"] != identity:
            violations.append("configure identity disagrees with the build tree it names")
        if current["options_sha256"] != configure.get("options_sha256"):
            violations.append("configure options disagree with the build tree it names")
    for target in data.get("targets", []):
        if target.get("name") != "pulp-canvas":
            continue
        defines = set(target.get("compile_definitions", []))
        if ("PULP_HAS_SKIA=1" in defines) != bool(identity.get("skia")):
            violations.append("pulp-canvas compile definitions disagree with the configure's Skia identity")
        if ("PULP_HAS_TEXT_SHAPING=1" in defines) != bool(identity.get("text_shaping")):
            violations.append("pulp-canvas compile definitions disagree with the configure's text-shaping identity")
    return violations


def external_link_violations(data: dict) -> list[str]:
    """A target that compiles against Skia records Skia as a link input, and only then."""
    pins = fetchcontent_pins(Path(__file__).resolve().parents[2])
    skia = pins.get("skia") or {}
    violations: list[str] = []
    for target in data.get("targets", []):
        # An absent key is an empty list: omitting it must not skip the check.
        links = target.get("external_link_inputs", [])
        if not isinstance(links, list):
            violations.append(f"{target.get('name')} external_link_inputs is not a list")
            continue
        uses_skia = "PULP_HAS_SKIA=1" in set(target.get("compile_definitions", []))
        skia_links = [link for link in links if isinstance(link, dict) and link.get("dependency") == "skia"]
        if len(skia_links) != len(links):
            violations.append(f"{target.get('name')} records a link input other than Skia")
        if len(skia_links) > 1:
            violations.append(f"{target.get('name')} records more than one Skia link input")
        if uses_skia and not skia_links:
            violations.append(f"{target.get('name')} compiles against Skia but records no Skia link input")
        if skia_links and not uses_skia:
            violations.append(f"{target.get('name')} records a Skia link input but does not compile against Skia")
        for link in skia_links:
            if link.get("pin") != skia.get("pin") or link.get("source") != skia.get("source") \
                    or link.get("asset_sha256") not in skia.get("assets", []):
                violations.append(f"{target.get('name')} Skia link input does not match the manifest pin and assets")
    return violations


def generated_input_violations(data: dict) -> list[str]:
    """Generated inputs name their real generator, and producers need outputs."""
    source = Path(str(data.get("source_root") or Path(__file__).resolve().parents[2]))
    if str(data.get("source_root")) == ".":
        source = Path(__file__).resolve().parents[2]
    violations: list[str] = []
    justified: set[str] = set()
    for row in data.get("generated_inputs", []):
        if not isinstance(row, dict):
            violations.append("generated input is not an object")
            continue
        rel = str(row.get("path", ""))
        expected = generated_producers(rel, source)
        claimed = sorted(set(row.get("producer_paths", [])))
        if expected is None:
            dependency = row.get("producer_dependency")
            if claimed:
                violations.append(f"generated input attributed to a non-producer {rel}: {claimed}")
            elif not re.match(r"^@build/_deps/[^/]+-build/", rel) or not isinstance(dependency, dict) or not dependency.get("name"):
                violations.append(f"generated input has no registered generator {rel}")
            continue
        if claimed != sorted(set(expected)):
            violations.append(f"generated input attributed to a non-producer {rel}: {claimed}")
        justified.update(expected)
    files = set(data.get("files", []))
    for producer in sorted((all_registered_producers(source) & files) - justified):
        violations.append(f"producer in closure without its generated input {producer}")
    return violations


MIRROR_EDGE_TABLE = Path(__file__).with_name("vellum_mirror_edge_patterns.json")


def mirror_edge_table(path: Path = MIRROR_EDGE_TABLE) -> dict:
    """The forbidden-mirror-edge table Pulp's gate and Vellum's extraction scan share.

    ``content_patterns`` apply to the text of every file whose suffix is in
    ``suffixes`` (``""`` is a file with no suffix), comments included;
    ``binary_suffixes`` name the only suffixes a closure may carry unscanned;
    ``path_patterns`` apply to a file's path relative to the mirror root;
    ``approved_exceptions`` exempt one exact line of one file from one
    content rule, each tied to a recorded view-audio seam debt. The Pulp-kept
    rules and the exceptions are generated by vellum_mirror_edge_table.py.
    """
    table = json.loads(path.read_text(encoding="utf-8"))
    if table.get("schema") != "pulp.vellum.mirror-edge-patterns.v1":
        raise SystemExit(f"invalid mirror edge table schema: {path}")
    if set(table["suffixes"]) & set(table["binary_suffixes"]):
        raise SystemExit(f"mirror edge table lists a suffix as both scanned and binary: {path}")
    return {
        "suffixes": set(table["suffixes"]),
        "binary_suffixes": set(table["binary_suffixes"]),
        "content": {name: re.compile(pattern) for name, pattern in table["content_patterns"].items()},
        "path": {name: re.compile(pattern) for name, pattern in table["path_patterns"].items()},
        "exceptions": {(row["rule"], row["path"], row["line"]) for row in table.get("approved_exceptions", [])},
    }


def closure_mention_violations(data: dict, table: dict | None = None) -> list[str]:
    """Every file a closure carries is free of forbidden mirror edges.

    A file is checked by path (a Pulp-kept file may not be mirrored at all)
    and by content, comments included. A file whose suffix is neither
    scanned nor declared binary is refused, so a new kind of file cannot
    ride along unchecked. The patterns are the ones Vellum's extraction scan
    applies to the mirror.
    """
    source = data.get("source_root")
    if not source:
        return []
    table = table or mirror_edge_table()
    root = Path(__file__).resolve().parents[2] if source == "." else Path(source)
    violations: list[str] = []
    for rel in data.get("files", []):
        for name, pattern in table["path"].items():
            if pattern.search(rel):
                violations.append(f"{name} {rel}")
        path = root / rel
        suffix = path.suffix.lower()
        if suffix in table["binary_suffixes"]:
            continue
        if suffix not in table["suffixes"]:
            violations.append(f"mirror-unclassified-suffix {rel}: {suffix} is neither scanned nor declared binary")
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            violations.append(f"mirror-unreadable-file {rel}: {exc}")
            continue
        lines = text.split("\n")
        for name, pattern in table["content"].items():
            for match in pattern.finditer(text):
                number = text.count("\n", 0, match.start()) + 1
                if (name, rel, lines[number - 1]) in table.get("exceptions", set()):
                    continue
                violations.append(f"{name} mention {rel}:{number}: {match.group(0)[:80]}")
    return violations


def cmake_reference_violations(data: dict, allowlist: list[dict] | None, used: set[str]) -> list[str]:
    """No CMake file in a closure may reference a forbidden Pulp target."""
    source = data.get("source_root")
    if not source:
        return []
    root = Path(__file__).resolve().parents[2] if source == "." else Path(source)
    violations: list[str] = []
    for path in data.get("files", []):
        if not (path.endswith(".cmake") or Path(path).name == "CMakeLists.txt"):
            continue
        full = root / path
        if not full.is_file():
            continue
        for line_no, line in enumerate(full.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            code = line.split("#", 1)[0]
            for match in FORBIDDEN_CMAKE_TARGET.finditer(code):
                key = f"{path}:{match.group(0)}"
                if allowlist and allowlist_match(allowlist, "include", key, used):
                    continue
                violations.append(f"forbidden target reference {path}:{line_no}: {match.group(0)}")
    return violations


def target_manifest(source: Path, build: Path, names: list[str]) -> dict:
    config, targets = codemodel(build)
    # Generated sources a target compiles are build-step outputs; anything
    # else generated is written by the configure itself.
    built_outputs: set[str] = set()
    resolver = ExternalResolver(source, build)
    by_name = {data.get("name"): data for data in targets.values()}
    optional = {n.removeprefix(OPTIONAL_PREFIX) for n in names if n.startswith(OPTIONAL_PREFIX)}
    names = [n.removeprefix(OPTIONAL_PREFIX) for n in names if not n.startswith(OPTIONAL_PREFIX) or n.removeprefix(OPTIONAL_PREFIX) in by_name]
    selected: list[dict] = []
    missing: list[str] = []
    all_files: set[Path] = set()
    all_include_dirs: set[Path] = set()
    all_defines: set[str] = set()
    links: set[str] = set()
    generated: set[str] = set()
    resources: set[str] = set()
    external_paths: set[str] = set()
    for name in names:
        data = by_name.get(name)
        if not data:
            if name not in optional:
                missing.append(name)
            continue
        target_files: set[Path] = set()
        source_entries = data.get("sources", [])
        for group in data.get("sourceGroups", []):
            for index in group.get("sourceIndexes", []):
                item = source_entries[index] if index < len(source_entries) else {}
                raw = item.get("path")
                # A custom command's ``.rule`` pseudo-source and a custom
                # target's CMakeFiles stamp are CMake's own bookkeeping, not
                # inputs.
                if raw and not raw.endswith(".rule") and "/CMakeFiles/" not in raw.replace("\\", "/"):
                    p = Path(raw)
                    if not p.is_absolute():
                        p = source / p
                    p = p.resolve()
                    target_files.add(p)
                    all_files.add(p)
                    rel = norm_path(raw, source, build)
                    if p.suffix.lower() in RESOURCE_EXTS:
                        resources.add(rel)
                    if rel.startswith("@build/"):
                        generated.add(rel)
                        built_outputs.add(rel)
        includes: list[str] = []
        defines: list[str] = []
        # CMake's file API omits imported targets, so a prebuilt library a
        # target compiles against is recorded from the include directories
        # that reach it.
        external_links: list[dict] = []
        for group in data.get("compileGroups", []):
            for item in group.get("includes", []):
                raw = item.get("path")
                if raw:
                    p = Path(raw)
                    if not p.is_absolute():
                        p = source / p
                    p = p.resolve()
                    all_include_dirs.add(p)
                    includes.append(resolver.portable(raw))
                    link = resolver.external_link_input(p)
                    if link and link not in external_links:
                        external_links.append(link)
            for item in group.get("defines", []):
                value = item.get("define")
                if value:
                    all_defines.add(value)
                    defines.append(value)
        deps: list[str] = []
        for dep in data.get("dependencies", []):
            dep_data = targets.get(dep.get("id"))
            dep_name = dep_data.get("name") if dep_data else dep.get("id")
            if dep_name:
                links.add(dep_name)
                deps.append(dep_name)
        selected.append({"name": name, "type": data.get("type"), "sources": sorted(rel for rel in (norm_path(str(p), source, build) for p in target_files) if not rel.startswith(("@build/", "/"))), "include_dirs": sorted(set(includes)), "compile_definitions": sorted(set(defines)), "link_targets": sorted(set(deps)), "external_link_inputs": external_links})
    public, declared = unit_public_headers(targets, names, source, unit_header_manifests(build))
    for entry in selected:
        if entry["name"] in declared:
            entry["public_headers"] = declared[entry["name"]]
    all_files = collect_headers(all_files | public, sorted(all_include_dirs), source)
    normalized = [norm_path(str(p), source, build) for p in all_files]
    # File-api does not list configure-time generated headers when they are
    # consumed only through an include directory. Add the known generated
    # products for the selected unit so the receipt remains reproducible.
    selected_names = set(names)
    if selected_names & {"pulp-runtime", "pulp-platform", "pulp-simd", "pulp-state"}:
        runtime_root = build / "core/runtime/generated"
        if runtime_root.is_dir():
            for candidate in runtime_root.rglob("*"):
                if candidate.is_file() and candidate.suffix.lower() in TEXT_EXTS:
                    normalized.append(norm_path(str(candidate), source, build))
                    all_files.add(candidate.resolve())
    if "pulp-canvas" in selected_names:
        # The declaration headers pulp_add_binary_data writes at configure
        # time, one per binary-data target this configure declares.
        for target_name in canvas_binary_data(source):
            candidate = build / "core/canvas" / f"{target_name}_data.hpp"
            if target_name in by_name and candidate.is_file():
                normalized.append(norm_path(str(candidate), source, build))
                all_files.add(candidate.resolve())
    if "pulp-view-script" in selected_names:
        for suffix in ("web_compat_preludes_gen.hpp", "web_compat_preludes_gen.cpp"):
            candidate = build / "core/view" / suffix
            if candidate.is_file():
                normalized.append(norm_path(str(candidate), source, build))
                all_files.add(candidate.resolve())
    # FetchContent source trees are reproducible external inputs.  Build-tree
    # products (including dependency build products such as SDL's generated
    # include revision) are generated inputs and must never become mirror files.
    for rel in normalized:
        if not rel.startswith("@build/"):
            continue
        if rel.startswith("@build/_deps/"):
            dep_path = rel.removeprefix("@build/_deps/")
            if re.search(r"(?:^|/)[^/]+-src/", dep_path):
                external_paths.add(rel)
                continue
        generated.add(rel)
    external_paths.update(p for p in normalized if p.startswith("/") and not p.startswith(str(source.resolve())))
    external = [resolver.identify(p) for p in sorted(external_paths)]
    # Keep the exact CMake/script producers of the generated inputs this
    # closure actually holds, so the receipt can prove which repository rules
    # produced each build file. A producer joins the closure only with its
    # output.
    producer_map = {rel: generated_producers(rel, source) for rel in sorted(generated - external_paths)}
    producer_paths = {path for paths_for_input in producer_map.values() if paths_for_input for path in paths_for_input}
    for producer in producer_paths:
        candidate = source / producer
        if candidate.is_file():
            all_files.add(candidate.resolve())
            normalized.append(producer)
    files = sorted({p for p in normalized if p not in external_paths and not p.startswith("/") and not p.startswith("@build/")})
    generated_rows = []
    for rel in sorted(generated - external_paths):
        candidate = build / rel.removeprefix("@build/")
        row = {"path": rel, "producer_paths": sorted(set(producer_map.get(rel) or []))}
        recipe = generated_digest(rel, candidate, source)
        if recipe:
            row |= recipe
        elif rel in built_outputs:
            # A build-step output with no recipe is listed by its declaration
            # alone; its bytes depend on whether the build has run.
            row["sha256"] = None
        elif candidate.is_file():
            row["sha256"] = sha256(candidate)
        else:
            continue
        dependency = generated_dependency(rel, resolver)
        if dependency:
            row["producer_dependency"] = dependency
        generated_rows.append(row)
    for rel in files:
        if Path(rel).suffix.lower() in RESOURCE_EXTS:
            resources.add(rel)
    return {"schema": "pulp.vellum.closure.v1", "source_root": str(source), "build_dir": str(build), "configuration": config.get("name", ""), "targets": selected, "missing_targets": missing, "files": files, "external_inputs": external, "public_include_dirs": sorted({resolver.portable(str(p)) for p in all_include_dirs}), "compile_definitions": sorted(all_defines), "transitive_link_targets": sorted(links), "resources": sorted(resources), "generated_inputs": generated_rows, "configure": configure_record(build)}



def js_runtime_manifest(source: Path, unit: str) -> dict:
    """Build a deterministic closure for the tracked JSX import runtime.

    JavaScript dependencies are installed inputs rather than mirror files.  The
    committed lockfile is part of the closure and its digest pins the complete
    node_modules graph that a Vellum consumer must provision.
    """
    if not source.is_dir():
        raise SystemExit(f"JSX runtime source root does not exist: {source}")
    try:
        repo_root = Path(run("git", "-C", str(source), "rev-parse", "--show-toplevel"))
        rel_root = source.resolve().relative_to(repo_root.resolve()).as_posix()
        tracked = run("git", "-C", str(repo_root), "ls-files", "--", rel_root).splitlines()
    except (subprocess.CalledProcessError, ValueError) as exc:
        raise SystemExit(f"cannot enumerate tracked JSX runtime files: {exc}") from exc
    files = []
    for rel in tracked:
        if not rel or rel == rel_root or rel.startswith(rel_root + "/node_modules/"):
            continue
        path = repo_root / rel
        if path.is_file():
            files.append(rel)
    lock_rel = f"{rel_root}/package-lock.json"
    lock_path = repo_root / lock_rel
    if not lock_path.is_file() or lock_rel not in files:
        raise SystemExit(f"tracked JSX runtime lockfile is required: {lock_rel}")
    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid JSX runtime package-lock.json: {exc}") from exc
    packages = lock.get("packages")
    if not isinstance(packages, dict):
        raise SystemExit("package-lock.json has no packages object")
    lock_pin = "sha256:" + sha256(lock_path)
    external = []
    for package_path, metadata in sorted(packages.items()):
        if not package_path.startswith("node_modules/"):
            continue
        row = {"path": f"{rel_root}/{package_path}", "source": "npm", "pin": lock_pin}
        external.append(row)
    if not external:
        raise SystemExit("package-lock.json contains no node_modules packages")
    return {
        "schema": "pulp.vellum.closure.v1",
        "unit": unit,
        "source_root": str(source),
        "build_dir": None,
        "configuration": "",
        "targets": [],
        "missing_targets": [],
        "files": sorted(set(files)),
        "external_inputs": external,
        "public_include_dirs": [],
        "compile_definitions": [],
        "transitive_link_targets": [],
        "resources": [],
        "generated_inputs": [],
    }


def cmd_js_manifest(args: argparse.Namespace) -> int:
    result = js_runtime_manifest(Path(args.source_root).resolve(), args.unit)
    Path(args.output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"closure_manifest={args.output} files={len(result['files'])} external_inputs={len(result['external_inputs'])}")
    return 0


OPTIONAL_PREFIX = "?"


def unit_manifest_layout(path: Path) -> list[dict]:
    """The D3 units in layer order: name, targets, and optional targets.

    A unit's ``owned_files`` names repository files it carries although they
    live under another unit's directory; each such file has one owner.
    A unit's ``optional_targets`` are targets only some configures create.
    Each is a name, or ``{"name": ..., "platforms": [CMake system names]}``
    when it belongs to the unit only on those platforms. The file order is the
    layering: a unit may link only its own targets and earlier units'.  An
    alias unit has no target of its own and reuses the target closure of the
    named earlier unit.  A carried unit has no target of its own and records
    the unit whose target closure carries it; this is used for INTERFACE
    targets omitted by a configure's CMake file-api reply.  These forms keep
    source slices distinct without assigning one CMake target to two owners.
    """
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid Vellum unit manifest {path}: {exc}") from exc
    if payload.get("schema") != "pulp.vellum.d3-units.v1":
        raise SystemExit(f"invalid Vellum unit manifest schema: {path}")
    units = payload.get("units")
    if not isinstance(units, list) or not units:
        raise SystemExit("Vellum unit manifest must contain units")
    layout: list[dict] = []
    names: set[str] = set()
    target_owners: dict[str, str] = {}
    for entry in units:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise SystemExit("Vellum unit manifest units require a name")
        name = entry["name"]
        if name in names:
            raise SystemExit(f"duplicate Vellum unit name {name}")
        names.add(name)
        unit_targets = entry.get("targets")
        alias_of = entry.get("alias_of")
        carried_by = entry.get("carried_by")
        if alias_of is not None and carried_by is not None:
            raise SystemExit(f"Vellum unit {name} cannot declare both alias_of and carried_by")
        if alias_of is not None and (not isinstance(alias_of, str) or not alias_of or alias_of == name or alias_of not in names):
            raise SystemExit(f"Vellum unit {name} alias_of must name an earlier unit")
        if carried_by is not None and (not isinstance(carried_by, str) or not carried_by or carried_by == name):
            raise SystemExit(f"Vellum unit {name} carried_by must name another unit")
        if alias_of is None and carried_by is None:
            if not isinstance(unit_targets, list) or not unit_targets or not all(isinstance(t, str) and t for t in unit_targets):
                raise SystemExit(f"Vellum unit {name} requires non-empty targets")
        elif unit_targets not in (None, []):
            kind = "alias" if alias_of is not None else "carried"
            raise SystemExit(f"Vellum {kind} unit {name} must not declare targets")
        if alias_of is not None or carried_by is not None:
            unit_targets = []
        optional: dict[str, list[str] | None] = {}
        for item in entry.get("optional_targets", []):
            if isinstance(item, str) and item:
                optional[item] = None
            elif isinstance(item, dict) and isinstance(item.get("name"), str) and item["name"] \
                    and isinstance(item.get("platforms"), list) and item["platforms"] \
                    and all(isinstance(v, str) and v for v in item["platforms"]):
                optional[item["name"]] = list(item["platforms"])
            else:
                raise SystemExit(f"Vellum unit {name} optional_targets must be names or {{name, platforms}}")
        if (alias_of is not None or carried_by is not None) and optional:
            kind = "alias" if alias_of is not None else "carried"
            raise SystemExit(f"Vellum {kind} unit {name} must not declare optional_targets")
        owned = entry.get("owned_files", [])
        if not isinstance(owned, list) or not all(isinstance(v, str) and v and not host_path(v) for v in owned):
            raise SystemExit(f"Vellum unit {name} owned_files must be repository paths")
        for target in [*unit_targets, *optional]:
            previous = target_owners.get(target)
            if previous is not None:
                raise SystemExit(f"D3 target {target} is owned by both {previous} and {name}; use alias_of for a source slice alias")
            target_owners[target] = name
        row = {"name": name, "targets": list(unit_targets), "optional": optional, "owned_files": list(owned)}
        if alias_of is not None:
            row["alias_of"] = alias_of
        if carried_by is not None:
            row["carried_by"] = carried_by
        layout.append(row)
    for row in layout:
        carrier = row.get("carried_by")
        if carrier is not None and carrier not in names:
            raise SystemExit(f"Vellum unit {row['name']} carried_by must name an existing unit")
    owners: dict[str, str] = {}
    for entry in layout:
        for path in entry["owned_files"]:
            if path in owners:
                raise SystemExit(f"{path} is owned by more than one D3 unit: {owners[path]}, {entry['name']}")
            owners[path] = entry["name"]
    return layout


def unit_manifest_targets(path: Path, unit: str | None = None) -> list[str]:
    """Return unique CMake targets named by the checked-in D3 unit manifest.

    With ``unit``, return only that unit's targets. Optional targets are
    returned with a leading ``?``; ``target_manifest`` takes them when the
    configure has them and does not report them missing.
    """
    layout = unit_manifest_layout(path)
    by_name = {entry["name"]: entry for entry in layout}

    def resolved(entry: dict, stack: tuple[str, ...] = ()) -> list[str]:
        if entry.get("alias_of") is None and entry.get("carried_by") is None:
            return [*entry["targets"], *(f"{OPTIONAL_PREFIX}{t}" for t in entry["optional"])]
        name = entry["name"]
        if name in stack:
            raise SystemExit("cyclic D3 unit alias/carried_by chain: " + " -> ".join((*stack, name)))
        return resolved(by_name[entry.get("alias_of") or entry["carried_by"]], (*stack, name))

    targets: list[str] = []
    found = False
    for entry in layout:
        if unit is None or entry["name"] == unit:
            found = True
            targets.extend(resolved(entry))
    if unit is not None and not found:
        raise SystemExit(f"Vellum unit manifest has no unit named {unit}")
    return list(dict.fromkeys(targets))


def unit_layer_violations(data: dict, layout: list[dict]) -> tuple[set[str], list[str]]:
    """Mirrored targets for this configure, and layering/platform violations.

    Every link between D3 targets must point to the same or an earlier unit,
    and the links among D3 targets must not form a cycle. A platform-scoped
    optional target is mirrored only on its platforms.
    """
    system = ((data.get("configure") or {}).get("identity") or {}).get("system")
    layer: dict[str, int] = {}
    restricted: dict[str, list[str]] = {}
    for index, entry in enumerate(layout):
        for name in entry["targets"]:
            layer.setdefault(name, index)
        for name, platforms in entry["optional"].items():
            layer.setdefault(name, index)
            if platforms:
                restricted[name] = platforms
    mirrored = {name for name in layer if name not in restricted or system in restricted[name]}
    violations: list[str] = []
    graph: dict[str, set[str]] = {}
    for target in data.get("targets", []):
        source = canonical_target(target.get("name", ""))
        if source in restricted and system not in restricted[source]:
            violations.append(f"{source} belongs to its D3 unit only on {', '.join(restricted[source])}")
        links = {canonical_target(link) for link in target.get("link_targets", [])}
        graph[source] = {link for link in links if link in layer}
        if source not in layer:
            continue
        for link in sorted(graph[source]):
            if layer[link] > layer[source]:
                violations.append(f"upward D3 link {source} ({layout[layer[source]]['name']}) -> "
                                  f"{link} ({layout[layer[link]]['name']})")
    # Skia enters the graph at the unit that owns its support library; no
    # target in an earlier unit may consume Skia.
    floor = layer.get(SKIA_SUPPORT_TARGET)
    if floor is not None:
        for target in data.get("targets", []):
            source = canonical_target(target.get("name", ""))
            defines = set(target.get("compile_definitions", []))
            links = {canonical_target(link) for link in target.get("link_targets", [])}
            if source in layer and layer[source] < floor and (
                    defines & {"PULP_HAS_SKIA=1", "SK_DAWN"} or SKIA_SUPPORT_TARGET in links):
                violations.append(f"Skia consumer {source} ({layout[layer[source]]['name']}) sits below "
                                  f"{layout[floor]['name']}, where Skia enters the D3 graph")
    state: dict[str, int] = {}

    def visit(node: str, stack: list[str]) -> None:
        state[node] = 1
        for nxt in sorted(graph.get(node, ())):
            if nxt == node:
                continue
            if state.get(nxt) == 1:
                cycle = stack[stack.index(nxt):] + [nxt] if nxt in stack else [node, nxt]
                violations.append("D3 link cycle " + " -> ".join(cycle))
            elif not state.get(nxt):
                visit(nxt, stack + [nxt])
        state[node] = 2

    for node in sorted(graph):
        if not state.get(node):
            visit(node, [node])
    return mirrored, violations


def ensure_codemodel_reply(source_root: str, build_dir: str) -> None:
    """Re-run the configure once when the file-api reply is missing.

    The codemodel query is written during configure, so the first configure of
    a fresh build directory finishes before CMake sees it and writes no reply.
    """
    if not list((Path(build_dir) / ".cmake/api/v1/reply").glob("index-*.json")):
        subprocess.run(["cmake", "-S", source_root, "-B", build_dir], check=True, stdout=subprocess.DEVNULL)


def cmd_manifest(args: argparse.Namespace) -> int:
    if args.unit and not args.unit_manifest:
        raise SystemExit("--unit requires --unit-manifest")
    targets = unit_manifest_targets(Path(args.unit_manifest), args.unit) if args.unit_manifest else args.target
    ensure_codemodel_reply(args.source_root, args.build_dir)
    result = target_manifest(Path(args.source_root).resolve(), Path(args.build_dir).resolve(), targets)
    Path(args.output).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"closure_manifest={args.output} files={len(result['files'])} missing={len(result['missing_targets'])}")
    return 0


def policy_target_forbidden(name: str) -> bool:
    return bool(FORBIDDEN_TARGET.search(name.removeprefix("pulp::")))


def policy_include_forbidden(name: str) -> bool:
    return bool(FORBIDDEN_INCLUDE.match(name))


def symbol_closure(undefined: Iterable[str], allowed: Iterable[str] = ()) -> list[str]:
    allowed_set = set(allowed)
    return sorted(symbol for symbol in undefined if symbol not in allowed_set and re.search(r"(?:audio|signal|host|midi|music|format|content|ContentRegistry)", symbol, re.I))


def load_allowlist(path: Path) -> list[dict]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != "pulp.vellum.closure.allowlist.v1":
        raise SystemExit(f"invalid closure allowlist schema: {path}")
    entries = payload.get("entries")
    if not isinstance(entries, list) or not entries:
        raise SystemExit("closure allowlist must contain entries")
    seen: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict) or not {"id", "owner", "debt_id", "kind", "match"} <= set(entry):
            raise SystemExit("closure allowlist entries require id, owner, debt_id, kind, and match")
        if entry["id"] in seen:
            raise SystemExit(f"duplicate closure allowlist id: {entry['id']}")
        seen.add(entry["id"])
        if entry["kind"] not in {"edge", "target", "include", "root"}:
            raise SystemExit(f"invalid closure allowlist kind: {entry['kind']}")
        if "approver" in entry and (not isinstance(entry["approver"], str) or not entry["approver"].strip()):
            raise SystemExit(f"allowlist approver must name who approved the exception: {entry['id']}")
        platforms = entry.get("platforms")
        if platforms is not None and (not isinstance(platforms, list) or not platforms or not all(isinstance(v, str) and v for v in platforms)):
            raise SystemExit(f"allowlist platforms must be a non-empty list of CMake system names: {entry['id']}")
        if entry["kind"] == "edge" and (
            not isinstance(entry.get("exit_criterion"), str) or not entry["exit_criterion"].strip()
        ):
            raise SystemExit(f"edge allowlist entry requires exit_criterion: {entry['id']}")
    return entries


def allowlist_match(entries: list[dict], kind: str, value: str, used: set[str]) -> bool:
    for entry in entries:
        if entry["kind"] != kind:
            continue
        match_type = entry.get("match_type", "exact")
        matched = value.startswith(entry["match"]) if match_type == "prefix" else value == entry["match"]
        if matched:
            used.add(entry["id"])
            return True
    return False


def canonical_target(name: str) -> str:
    """Spell a ``pulp::x`` CMake alias as the ``pulp-x`` target it names."""
    return "pulp-" + name.removeprefix("pulp::") if name.startswith("pulp::") else name


def edge_key(source: str, target: str) -> str:
    """Exact allowlist key for one link edge; distinct targets never merge."""
    return f"{canonical_target(source)}->{canonical_target(target)}"


def is_pulp_target(name: str) -> bool:
    return canonical_target(name).startswith(("pulp-", "pulp_"))


def unmirrored_link_violations(data: dict, mirrored: set[str], allowlist: list[dict] | None, used: set[str]) -> list[str]:
    """Every Pulp target a D3 unit links must itself be a D3 unit target.

    The only exception is an allowlisted edge naming that exact
    ``source -> target`` link; nothing passes through another edge's debt.
    """
    violations: list[str] = []
    for target in data.get("targets", []):
        for link in target.get("link_targets", []):
            name = canonical_target(link)
            if not is_pulp_target(name) or name in mirrored:
                continue
            if allowlist and allowlist_match(allowlist, "edge", edge_key(target["name"], name), used):
                continue
            violations.append(f"unmirrored Pulp target {target['name']} -> {name}")
    return violations


def cmd_gate(args: argparse.Namespace) -> int:
    failed = False
    full_allowlist = load_allowlist(Path(args.allowlist)) if args.allowlist else None
    used_allowlist: set[str] = set()
    gated_systems: set[str] = set()
    generated_targets = unit_manifest_targets(Path(args.unit_manifest)) if args.unit_manifest else args.target
    for raw in args.manifest:
        manifest_path = Path(raw)
        if not manifest_path.is_file() and args.generate:
            if not args.source_root or not args.build_dir or not generated_targets:
                raise SystemExit("--generate requires --source-root, --build-dir, and --target")
            ensure_codemodel_reply(args.source_root, args.build_dir)
            generated = target_manifest(
                Path(args.source_root).resolve(), Path(args.build_dir).resolve(), generated_targets
            )
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            manifest_path.write_text(json.dumps(generated, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        data = json.loads(Path(raw).read_text(encoding="utf-8"))
        name = Path(raw).name
        system = ((data.get("configure") or {}).get("identity") or {}).get("system")
        if system:
            gated_systems.add(system)
        # An entry scoped to platforms matches only a manifest from one of them.
        allowlist = None if full_allowlist is None else [
            entry for entry in full_allowlist if entry.get("platforms") is None or (system and system in entry["platforms"])]
        violations: list[str] = []
        if data.get("schema") == "pulp.vellum.closure.v1" and data.get("targets") and not args.unit_manifest:
            violations.append("a CMake closure is gated against the D3 units: pass --unit-manifest")
        violations.extend(external_input_violations(data))
        violations.extend(receipt_path_violations(data))
        violations.extend(configure_violations(data))
        violations.extend(external_link_violations(data))
        violations.extend(generated_input_violations(data))
        violations.extend(cmake_reference_violations(data, allowlist, used_allowlist))
        if args.unit_manifest:
            if not isinstance(data.get("configure"), dict):
                violations.append("closure does not record its configure")
            mirrored, layer_violations = unit_layer_violations(data, unit_manifest_layout(Path(args.unit_manifest)))
            violations.extend(layer_violations)
            violations.extend(unmirrored_link_violations(data, mirrored, allowlist, used_allowlist))
        if data.get("missing_targets"):
            violations.extend("missing target: " + x for x in data["missing_targets"])
        for target in data.get("targets", []):
            for link in target.get("link_targets", []):
                allowed = link in args.allow
                if allowlist:
                    allowed = allowed or allowlist_match(allowlist, "edge", edge_key(target["name"], link), used_allowlist)
                    allowed = allowed or allowlist_match(allowlist, "target", link, used_allowlist)
                if policy_target_forbidden(link) and not allowed:
                    violations.append(f"forbidden link {target['name']} -> {link}")
        for symbol in data.get("undefined_symbols", []):
            if symbol_closure([symbol]):
                violations.append(f"forbidden undefined symbol {symbol}")
        for path in data.get("files", []):
            if path.startswith("@build/"):
                continue
            if path.startswith(FORBIDDEN_ROOTS):
                allowed_root = any(path.startswith(prefix) for prefix in args.allow_root_prefix)
                if allowlist:
                    allowed_root = allowed_root or allowlist_match(allowlist, "root", path, used_allowlist)
                if not allowed_root:
                    violations.append(f"forbidden closure root {path}")
                continue
            full = Path(data["source_root"]) / path
            if full.suffix.lower() in TEXT_EXTS and full.is_file():
                for line_no, _delimiter, include_name in include_edges(full):
                    if policy_include_forbidden(include_name):
                        # A path exemption is only valid for the explicitly named edge. Never let
                        # one legacy file exemption accept every forbidden include in that file.
                        allowed = any(
                            path == allowed_path and include_name == allowed_include
                            for allowed_path, allowed_include in getattr(args, "allow_include_edge", [])
                        )
                        allowed = allowed or any(path.startswith(prefix) for prefix in args.allow_include_prefix)
                        if allowlist:
                            allowed = allowed or allowlist_match(allowlist, "include", f"{path}:{include_name}", used_allowlist)
                        if not allowed:
                            violations.append(f"forbidden include {path}:{line_no}: {include_name}")
        status = "PASS" if not violations else ("REPORT-ONLY" if args.report_only else "FAIL")
        print(f"{name}: {status}")
        for violation in violations:
            print(f"  {violation}")
        failed |= bool(violations)
    if full_allowlist:
        # An entry scoped to platforms is only expected to be used when a gated
        # manifest comes from one of them.
        applicable = {entry["id"] for entry in full_allowlist
                      if entry.get("platforms") is None or set(entry["platforms"]) & gated_systems}
        unused = sorted(applicable - used_allowlist)
        for entry in unused:
            print(f"allowlist entry unused: {entry}")
        failed |= bool(unused)
    return 1 if failed and not args.report_only else 0


def git_blob_rows(repo: Path, commit: str, paths: Iterable[str]) -> list[dict]:
    rows: list[dict] = []
    for path in paths:
        if path.startswith("@build/"):
            continue
        try:
            row = run("git", "-C", str(repo), "ls-tree", commit, "--", path)
        except subprocess.CalledProcessError:
            continue
        if row:
            mode, kind, blob, name = row.split("\t", 1)[0].split(" ") + [row.split("\t", 1)[1]]
            if kind == "blob":
                rows.append({"path": name, "mode": mode, "blob": blob})
    return rows


def export_owned_file_violations(manifests: list[dict], unit_names: list[str], units: Path | None = None) -> list[str]:
    """Check file ownership across one combined D3 export.

    A ``carried_by`` unit has no target of its own, so its target closure may
    also appear in the carrier's manifest.  The carried unit still owns its
    source files: a carrier may contain one of those files only when the
    carried unit is included in the same export.  A carrier-only export is
    rejected instead of silently attributing the file to the wrong unit.
    """
    layout = unit_manifest_layout(units or Path(__file__).with_name("vellum_d3_units.json"))
    by_name = {entry["name"]: entry for entry in layout}
    owner = {path: entry["name"] for entry in layout for path in entry["owned_files"]}
    selected = set(unit_names)

    def carrier_contains(owner_name: str, carrier_name: str) -> bool:
        """Whether carrier_name is in owner_name's carried_by chain."""
        seen: set[str] = set()
        current = owner_name
        while current in by_name and current not in seen:
            seen.add(current)
            current = by_name[current].get("carried_by")
            if current == carrier_name:
                return True
            if current is None:
                break
        return False

    violations: list[str] = []
    for manifest, unit in zip(manifests, unit_names):
        for path in manifest.get("files", []):
            source_owner = owner.get(path)
            if source_owner is not None and source_owner != unit:
                # A carrier's target closure can repeat the carried unit's
                # files.  Preserve the source owner and require that owner to
                # be present in this combined export before accepting it.
                if source_owner in selected and carrier_contains(source_owner, unit):
                    continue
                violations.append(f"{path} belongs to D3 unit {owner[path]}, not {unit}")
    return violations


def export_unmirrored_links(manifests: list[dict]) -> list[str]:
    """Pulp targets the exported closures link but none of them exports.

    There is no override: the mirror must carry every Pulp target its
    closures link.
    """
    exported = {canonical_target(t.get("name", "")) for m in manifests for t in m.get("targets", [])}
    missing: set[str] = set()
    for manifest in manifests:
        for link in manifest.get("transitive_link_targets", []):
            name = canonical_target(link)
            if is_pulp_target(name) and name not in exported:
                missing.add(name)
    return sorted(missing)


def exportable_closure(manifest: dict, unit_name: str, cpu_only_units: Iterable[str] = ()) -> dict:
    """Return the closure export writes for one manifest, or refuse it.

    Export never writes a closure whose inputs fail provenance, that still
    names a host path, that does not record its configure, whose generated
    inputs name the wrong producer, or whose files mention a Pulp-owned
    subsystem. The canvas is the GPU/Skia-backed graph:
    a canvas closure from any other configure is refused unless the unit is
    explicitly labelled ``cpu-only``, and the label travels with the closure.
    """
    normalized = dict(manifest)
    normalized["source_root"] = None
    normalized["build_dir"] = None
    violations = receipt_path_violations(normalized) + external_input_violations(manifest)
    violations += configure_violations(manifest) + generated_input_violations(manifest) + external_link_violations(manifest)
    for row in manifest.get("generated_inputs", []):
        if isinstance(row, dict) and not SHA256_RE.fullmatch(str(row.get("sha256"))):
            violations.append(f"generated input has no reproducible digest; it needs a recipe {row.get('path')}")
    violations += closure_mention_violations(manifest)
    for row in manifest.get("external_inputs", []):
        if isinstance(row, dict) and row.get("patched") and row.get("base_modification"):
            # The patch set can only be replayed onto an unmodified base.
            violations.append(f"patched input was copied from a modified base checkout {row.get('path')}")
    configure = manifest.get("configure")
    if not isinstance(configure, dict) or not isinstance(configure.get("identity"), dict):
        violations.append("closure does not record its configure")
    elif any(t.get("name") == "pulp-canvas" for t in manifest.get("targets", [])):
        identity = configure["identity"]
        if not (identity.get("gpu") and identity.get("skia")):
            if unit_name in set(cpu_only_units):
                normalized["configure"] = {**configure, "label": "cpu-only"}
            else:
                violations.append("canvas closure comes from a non-GPU or non-Skia configure; label the unit cpu-only to export it")
    if violations:
        raise SystemExit(f"closure {unit_name} fails provenance: " + "; ".join(violations))
    return normalized


def cmd_export(args: argparse.Namespace) -> int:
    if not re.fullmatch(r"[0-9a-f]{40}", args.pulp_sha):
        raise SystemExit("--pulp-sha must be a full 40-hex commit SHA")
    manifests = [json.loads(Path(path).read_text(encoding="utf-8")) for path in args.manifest]
    closures = [
        exportable_closure(manifest, args.unit_name[index] if index < len(args.unit_name) else Path(args.manifest[index]).stem,
                           args.cpu_only_unit)
        for index, manifest in enumerate(manifests)
    ]
    owned_misplaced = export_owned_file_violations(manifests, [
        args.unit_name[index] if index < len(args.unit_name) else Path(args.manifest[index]).stem
        for index in range(len(manifests))])
    if owned_misplaced:
        raise SystemExit("; ".join(owned_misplaced))
    unmirrored = export_unmirrored_links(manifests)
    if unmirrored:
        raise SystemExit("exported closures link Pulp targets no exported closure carries: " + ", ".join(unmirrored))
    repo = Path(args.repo).resolve()
    out = Path(args.output).resolve()
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"output must be empty: {out}")
    out.parent.mkdir(parents=True, exist_ok=True)
    filter_repo = shutil.which("git-filter-repo") or shutil.which("git-filter-repo.py")
    if not filter_repo:
        raise SystemExit("git-filter-repo is required for export; refusing fallback projection")
    paths = sorted({p for manifest in manifests for p in manifest["files"] if not p.startswith("@build/")})
    subprocess.run(["git", "clone", "--no-local", str(repo), str(out)], check=True, stdout=subprocess.DEVNULL)
    mirror_root = args.mirror_root.strip("/")
    if not mirror_root or mirror_root.startswith("@"):
        raise SystemExit("mirror-root must be a non-empty repository-relative directory")
    rename_map = args.rename_map or {}
    cmd = [filter_repo, "--force"]
    for path in paths:
        mirror_path = rename_map.get(path, path)
        if Path(mirror_path).is_absolute() or ".." in Path(mirror_path).parts:
            raise SystemExit(f"rename map escapes repository: {path} -> {mirror_path}")
        cmd.extend(["--path", path, "--path-rename", f"{path}:{mirror_root}/{mirror_path}"])
    run("git", "-C", str(out), "update-ref", "refs/heads/export-source", args.pulp_sha)
    run("git", "-C", str(out), "checkout", "--detach", "export-source")
    cmd.extend(["--refs", "export-source"])
    subprocess.run(cmd, cwd=out, check=True)
    run("git", "-C", str(out), "checkout", "--detach", "export-source")
    mirror_sha = run("git", "-C", str(out), "rev-parse", "refs/heads/export-source")
    mirror_tree = run("git", "-C", str(out), "rev-parse", f"{mirror_sha}:mirror")
    rows = git_blob_rows(repo, args.pulp_sha, paths)
    by_path = {row["path"]: row["blob"] for row in rows}
    export_id = f"{datetime.now(timezone.utc):%Y%m%d}-{args.pulp_sha[:12]}"
    landing = Path(args.landing).resolve() if args.landing else out.parent / f"vellum-export-{export_id}"
    closure_dir = landing / "provenance" / "mirror" / "closures" / export_id
    closure_dir.mkdir(parents=True, exist_ok=True)
    units = []
    for index, manifest in enumerate(manifests):
        unit_name = args.unit_name[index] if index < len(args.unit_name) else Path(args.manifest[index]).stem
        normalized = closures[index]
        closure_path = closure_dir / f"{unit_name}.json"
        closure_path.write_text(json.dumps(normalized, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        configure = normalized.get("configure", {})
        units.append({"name": unit_name, "closure_manifest": f"provenance/mirror/closures/{export_id}/{unit_name}.json", "closure_sha256": sha256(closure_path), "oracle_class": "identity",
                      "configure": {"identity": configure.get("identity"), "options_sha256": configure.get("options_sha256"), "label": configure.get("label", "")}})
    files = sorted(({"mirror_path": f"{mirror_root}/{rename_map.get(path, path)}", "pulp_path": path, "mode": next(row["mode"] for row in rows if row["path"] == path), "blob": by_path[path]} for path in paths if path in by_path), key=lambda row: row["mirror_path"])
    commit_map_src = out / ".git" / "filter-repo" / "commit-map"
    commit_map_path = landing / "provenance" / "mirror" / "commit-maps" / f"{export_id}.txt"
    commit_map_path.parent.mkdir(parents=True, exist_ok=True)
    commit_map_bytes = commit_map_src.read_bytes() if commit_map_src.is_file() else b""
    required_map_row = f"{args.pulp_sha} {mirror_sha}\n".encode()
    if required_map_row not in commit_map_bytes:
        commit_map_bytes += required_map_row
    commit_map_path.write_bytes(commit_map_bytes)
    tool_rows = git_blob_rows(repo, args.pulp_sha, ["tools/scripts/vellum_closure.py"])
    receipt_rel = f"provenance/mirror/receipts/{export_id}.json"
    generated_inputs = []
    seen_generated = set()
    for manifest in manifests:
        for item in manifest.get("generated_inputs", []):
            if not isinstance(item, dict) or not {"path", "sha256", "producer_paths"} <= set(item):
                raise SystemExit("generated_inputs entries require path, sha256, and producer_paths")
            key = item["path"]
            if key in seen_generated and next(row for row in generated_inputs if row["path"] == key) != item:
                raise SystemExit(f"conflicting generated input: {key}")
            if key not in seen_generated:
                generated_row = {"path": key, "sha256": item["sha256"], "producer_paths": sorted(set(item["producer_paths"]))}
                for optional_key in ("normalization", "producer_dependency"):
                    if optional_key in item:
                        generated_row[optional_key] = item[optional_key]
                generated_inputs.append(generated_row)
                seen_generated.add(key)
    generated_inputs.sort(key=lambda row: row["path"])
    file_paths = {row["pulp_path"] for row in files}
    for item in generated_inputs:
        missing = sorted(set(item["producer_paths"]) - file_paths)
        if missing:
            raise SystemExit(f"generated input producers are not exported: {item['path']}: {missing}")
    receipt = {
        "schema": "pulp.vellum.export-receipt.v2",
        "pulp_repository": "Generous-Corp/pulp",
        "export_id": export_id,
        "pulp_sha": args.pulp_sha,
        "mirror_ref": args.mirror_ref,
        "mirror_sha": mirror_sha,
        "mirror_tree": mirror_tree,
        "rename": {"from_prefix": "", "to_prefix": "mirror/"},
        "commit_map": {"path": f"provenance/mirror/commit-maps/{export_id}.txt", "sha256": sha256(commit_map_path)},
        "units": units,
        "files": files,
        "generated_inputs": generated_inputs,
        "tool": {"path": "tools/scripts/vellum_closure.py", "pulp_blob": tool_rows[0]["blob"] if tool_rows else ""},
        "created_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    path_violations = receipt_path_violations(receipt)
    if path_violations:
        raise SystemExit("receipt contains host paths: " + "; ".join(path_violations))
    if args.dry_run:
        receipt["dry_run"] = True
    receipt_path = landing / "provenance" / "mirror" / "receipts" / f"{export_id}.json"
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"export_receipt={receipt_path} mirror_sha={mirror_sha} blobs={len(files)}")
    return 0


def cmd_mentions(args: argparse.Namespace) -> int:
    """Scan the named D3 units' closures for Pulp-owned subsystem mentions."""
    ensure_codemodel_reply(args.source_root, args.build_dir)
    source, build, units = Path(args.source_root).resolve(), Path(args.build_dir).resolve(), Path(args.unit_manifest)
    problems: list[str] = []
    for unit in args.unit:
        manifest = target_manifest(source, build, unit_manifest_targets(units, unit))
        problems += [f"{unit}: {violation}" for violation in closure_mention_violations(manifest)]
    print(f"mentions {' '.join(args.unit)}: {'FAIL' if problems else 'PASS'}")
    for problem in problems:
        print(f"  {problem}")
    return 1 if problems else 0


def cmd_export_check(args: argparse.Namespace) -> int:
    """Run every check export applies, for the named D3 units, without exporting."""
    ensure_codemodel_reply(args.source_root, args.build_dir)
    source, build, units = Path(args.source_root).resolve(), Path(args.build_dir).resolve(), Path(args.unit_manifest)
    manifests = [target_manifest(source, build, unit_manifest_targets(units, unit)) for unit in args.unit]
    problems: list[str] = []
    for unit, manifest in zip(args.unit, manifests):
        try:
            exportable_closure(manifest, unit)
        except SystemExit as exc:
            problems.append(str(exc))
    problems += export_owned_file_violations(manifests, args.unit, units)
    unmirrored = export_unmirrored_links(manifests)
    if unmirrored:
        problems.append("exported closures link Pulp targets no exported closure carries: " + ", ".join(unmirrored))
    print(f"export-check {' '.join(args.unit)}: {'FAIL' if problems else 'PASS'}")
    for problem in problems:
        print(f"  {problem}")
    return 1 if problems else 0


def cmd_verify(args: argparse.Namespace) -> int:
    receipt = json.loads(Path(args.receipt).read_text(encoding="utf-8"))
    repo = Path(args.mirror).resolve()
    head = run("git", "-C", str(repo), "rev-parse", "HEAD")
    recorded_head = receipt.get("mirror_sha", receipt.get("mirror_commit"))
    if head != recorded_head:
        print(f"identity=FAIL\n  mirror HEAD {head} != receipt mirror commit {recorded_head}")
        return 1
    expected = {row["mirror_path"]: row["blob"] for row in receipt["files"]}
    actual = {row["path"]: row["blob"] for row in git_blob_rows(repo, head, expected)}
    bad = [path for path, blob in expected.items() if actual.get(path) != blob]
    if bad:
        print("identity=FAIL")
        for path in bad[:20]: print(f"  mismatch: {path}")
        return 1
    print(f"identity=PASS blobs={len(expected)}")
    return 0


def _include_edge(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected PATH=INCLUDE")
    path, include = value.split("=", 1)
    return path, include


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("manifest"); p.add_argument("--source-root", required=True); p.add_argument("--build-dir", required=True); p.add_argument("--target", action="append"); p.add_argument("--unit-manifest"); p.add_argument("--unit"); p.add_argument("--output", required=True); p.set_defaults(func=cmd_manifest)
    p = sub.add_parser("js-manifest"); p.add_argument("--source-root", required=True); p.add_argument("--unit", default="design-import"); p.add_argument("--output", required=True); p.set_defaults(func=cmd_js_manifest)
    p = sub.add_parser("gate"); p.add_argument("--manifest", action="append", required=True); p.add_argument("--allow", action="append", default=[]); p.add_argument("--allow-include-path", action="append", default=[]); p.add_argument("--allow-include-edge", action="append", default=[], type=_include_edge); p.add_argument("--allow-include-prefix", action="append", default=[]); p.add_argument("--allow-root-prefix", action="append", default=[]); p.add_argument("--allowlist"); p.add_argument("--generate", action="store_true"); p.add_argument("--source-root"); p.add_argument("--build-dir"); p.add_argument("--target", action="append"); p.add_argument("--unit-manifest"); p.set_defaults(func=cmd_gate, report_only=False)
    p = sub.add_parser("report"); p.add_argument("--manifest", action="append", required=True); p.add_argument("--allow", action="append", default=[]); p.add_argument("--allow-include-path", action="append", default=[]); p.add_argument("--allow-include-edge", action="append", default=[], type=_include_edge); p.add_argument("--allow-include-prefix", action="append", default=[]); p.add_argument("--allow-root-prefix", action="append", default=[]); p.add_argument("--allowlist"); p.add_argument("--generate", action="store_true"); p.add_argument("--source-root"); p.add_argument("--build-dir"); p.add_argument("--target", action="append"); p.add_argument("--unit-manifest"); p.set_defaults(func=cmd_gate, report_only=True)
    p = sub.add_parser("export"); p.add_argument("--repo", required=True); p.add_argument("--manifest", action="append", required=True); p.add_argument("--unit-name", action="append", default=[]); p.add_argument("--pulp-sha", required=True); p.add_argument("--output", required=True); p.add_argument("--landing"); p.add_argument("--mirror-root", default="mirror"); p.add_argument("--mirror-ref", default="refs/heads/mirror/pulp-framework"); p.add_argument("--dry-run", action="store_true"); p.add_argument("--rename-map", type=json.loads, default=None); p.add_argument("--cpu-only-unit", action="append", default=[]); p.set_defaults(func=cmd_export)
    p = sub.add_parser("mentions"); p.add_argument("--source-root", required=True); p.add_argument("--build-dir", required=True); p.add_argument("--unit-manifest", required=True); p.add_argument("--unit", action="append", required=True); p.set_defaults(func=cmd_mentions)
    p = sub.add_parser("export-check"); p.add_argument("--source-root", required=True); p.add_argument("--build-dir", required=True); p.add_argument("--unit-manifest", required=True); p.add_argument("--unit", action="append", required=True); p.set_defaults(func=cmd_export_check)
    p = sub.add_parser("verify"); p.add_argument("--receipt", required=True); p.add_argument("--mirror", required=True); p.set_defaults(func=cmd_verify)
    args = parser.parse_args()
    return args.func(args)

if __name__ == "__main__":
    raise SystemExit(main())
