#!/usr/bin/env python3
"""Dependency audit and upstream drift checker for Pulp.

This audit guards the four canonical attribution surfaces:

- ``DEPENDENCIES.md``
- ``NOTICE.md``
- ``docs/reference/licensing.md``
- ``tools/deps/manifest.json`` (machine-readable inventory)

It verifies two classes of invariant:

1. **Consistency** — every manifest entry that claims coverage is actually
   listed in the three Markdown files. This was the original check shipped
   in #565.

2. **Completeness** — every dependency declared in a real manifest source
   (``requirements-docs.txt``, ``mkdocs.yml``, ``external/``, a recognized
   redistributed bootstrap binary, or a fetch anywhere in the repo's CMake
   files and setup scripts — ``FetchContent_Declare``, ``ExternalProject_Add``,
   ``file(DOWNLOAD)``, and curl/wget/git-clone/setup-helper downloads) is
   represented by a manifest entry, matched by name or by upstream URL. This
   class of check was missing, which is how #582 shipped MkDocs Material
   without touching the four attribution files, and how the wgpu-native
   runtime archive setup.sh downloads went unlisted while only the root
   CMakeLists.txt was read.

3. **Truthfulness** (``--verify-licenses``) — the attribution surfaces must
   agree with the license text actually on disk. The checks above only ask
   whether a dependency is *named* in each file, so both of the following
   passed a green ``--strict`` audit: NOTICE.md reproduced a truncated MIT
   license for 25 of its 26 MIT entries, and the VST3 SDK was labeled MIT
   across all four surfaces while the pinned tree was "Steinberg VST3
   License OR GPLv3". A name being present says nothing about the text being
   right.

4. **Renderer agreement** (``--strict``) — gpu-runtime's renderer lock
   (``gpu-runtime/renderer-lock.json`` in the Vellum repository), read at the
   exact commit Pulp pins gpu-runtime to, must name Pulp's macOS universal
   Skia/Dawn archive digest and Skia release. The lock is the one place the
   renderer pin is written; Pulp's manifest must agree with it.
"""

from __future__ import annotations

import argparse
import datetime
import functools
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

import cmake_commands
from pulp_lock import load as load_pulp_lock, validate as validate_pulp_lock


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tools" / "deps" / "manifest.json"
DEPENDENCIES_MD = ROOT / "DEPENDENCIES.md"
NOTICE_MD = ROOT / "NOTICE.md"
LICENSING_MD = ROOT / "docs" / "reference" / "licensing.md"

REQUIREMENTS_DOCS = ROOT / "requirements-docs.txt"
MKDOCS_YML = ROOT / "mkdocs.yml"
ROOT_CMAKELISTS = ROOT / "CMakeLists.txt"
EXTRA_CMAKELISTS = [
    ROOT / "bindings" / "python" / "CMakeLists.txt",
]
EXTERNAL_DIR = ROOT / "external"
GRADLE_WRAPPER_JAR = ROOT / "android" / "gradle" / "wrapper" / "gradle-wrapper.jar"


# ── License verification ────────────────────────────────────────────────────
#
# Where a dependency's real license text lives locally. setup.sh clones into a
# shared FetchContent cache and links external/<name> at it, so both roots are
# checked. Trees that are not present locally are reported as unverified rather
# than assumed good — an absent tree is not evidence of a correct license.
FETCHCONTENT_CACHE = Path.home() / "Library" / "Caches" / "Pulp" / "fetchcontent-src"

LICENSE_FILE_NAMES = ("LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING", "license.txt")

# A permission notice has three parts, and reproducing only the first is the
# truncation this check exists to catch: these licenses condition redistribution
# on including the notice, and the warranty disclaimer is part of it.
#
# Families are identified by a phrase unique to each, not by the shared opener
# "Permission is hereby granted" — the Boost license opens with that line too,
# and matching on it alone misreports BSL entries (Catch2) as truncated MIT.
# Each family's condition is worded differently ("shall" vs "must"), so the
# structure is checked per family rather than with one set of markers.
LICENSE_DISCLAIMER = "THE SOFTWARE IS PROVIDED"
LICENSE_FAMILIES = (
    # (family, phrase identifying it, phrase for its inclusion condition,
    #  phrase for its warranty disclaimer). Checked in order and the first
    # match wins, so a license that shares MIT's grant wording must come
    # before MIT. Unicode-3.0 (ICU's license) grants "to deal in the Data
    # Files or Software" and conditions on the notice appearing "with all
    # copies"; judged as MIT it read as truncated.
    (
        "Unicode-3.0",
        "to deal in the Data Files or Software without restriction",
        "this copyright and permission notice appear with all copies",
        'DATA FILES AND SOFTWARE ARE PROVIDED "AS IS"',
    ),
    (
        "MIT",
        "to deal in the Software without restriction",
        "shall be included in all",
        LICENSE_DISCLAIMER,
    ),
    ("BSL-1.0", "Boost Software License", "must be included in all copies", LICENSE_DISCLAIMER),
)

# Match the spelled-out phrase, never a bare "GPL": v3.7.12's LICENSE.txt words
# it "General Public License (GPL) Version 3" and never "GNU General Public
# License", while "GPL" alone false-positives on identifiers (gPluginFactory).
COPYLEFT_MARKERS = (
    "General Public License",
    "Server Side Public License",
    "Mozilla Public License",
)

# Licenses whose presence in a redistributed tree is a policy violation per
# CLAUDE.md. MPL-2.0 is review-required rather than forbidden, so it is
# reported as a warning by name rather than a hard failure.
REVIEW_ONLY_MARKERS = ("Mozilla Public License",)


def load_manifest() -> list[dict]:
    return json.loads(MANIFEST.read_text())["dependencies"]


def flatten(text: str) -> str:
    """Collapse all whitespace so phrase matching survives line wrapping.

    License prose is hard-wrapped at differing widths, so a phrase spans a
    newline as often as not ("to deal\\nin the Software"). Matching raw text
    reported entries as missing clauses they plainly contained.
    """
    return re.sub(r"\s+", " ", text)


def parse_notice_entries() -> dict[str, str]:
    """Return {entry name: body text} for every ``## `` block in NOTICE.md."""
    text = NOTICE_MD.read_text()
    entries: dict[str, str] = {}
    for block in re.split(r"^## ", text, flags=re.M)[1:]:
        name, _, body = block.partition("\n")
        entries[name.strip()] = body
    return entries


def find_notice_truncations() -> list[tuple[str, list[str]]]:
    """Find NOTICE.md entries reproducing an incomplete permission notice.

    Only entries whose license family is recognized are checked; BSD, Apache,
    zlib and public-domain entries carry no matching phrase and are left alone.
    """
    problems: list[tuple[str, list[str]]] = []
    for name, raw_body in parse_notice_entries().items():
        body = flatten(raw_body)
        for _family, identifier, condition, disclaimer in LICENSE_FAMILIES:
            if identifier not in body:
                continue
            missing = []
            if condition not in body:
                missing.append("inclusion condition")
            if disclaimer not in body:
                missing.append("warranty disclaimer")
            if missing:
                problems.append((name, missing))
            break
    return problems


def local_source_tree(dep: dict) -> Path | None:
    """Locate a dependency's checked-out tree, or None if not available.

    ``external/`` is searched before the shared FetchContent cache, and never
    merged with it: external/<dep> is what the build actually compiles (setup.sh
    links it at the pinned ref), while the cache accumulates every ref ever
    fetched. Searching them together let a stale cache dir shadow the real tree
    and the audit then verified a version the repo does not use.

    A cache hit must also match the manifest's pinned version, since the cache
    holds several refs of the same dependency side by side.
    """
    aliases = manifest_alias_set(dep) | {dep["name"]}
    wanted = {_normalise(a) for a in aliases}

    if EXTERNAL_DIR.is_dir():
        for path in sorted(p for p in EXTERNAL_DIR.iterdir() if p.is_dir()):
            # _normalise strips punctuation, which makes the directory
            # "cpp-httplib" match the manifest's "cpphttplib" alias.
            if _normalise(path.name) in wanted:
                return path

    version = dep.get("version", "")
    if FETCHCONTENT_CACHE.is_dir() and version:
        for path in sorted(p for p in FETCHCONTENT_CACHE.iterdir() if p.is_dir()):
            # Cache dirs are "<name>-<ref>". Strip the ref using the pinned
            # version rather than splitting on "-", which mangles multi-hyphen
            # refs ("sdl3-release-3.2.12" -> "sdl3-release-3.2").
            if not path.name.endswith(version):
                continue
            if _normalise(path.name[: -len(version)].rstrip("-")) in wanted:
                return path
    return None


def find_license_files(tree: Path) -> list[Path]:
    """Top-level and one-level-deep license files in a dependency tree.

    Scoped deliberately: a dependency's own license sits at the root or in the
    module dirs beside it, while a deep walk pulls in vendored third-party and
    sample dependencies whose licenses are not the dependency's own claim.

    Deduplicated by resolved path — macOS is case-insensitive, so LICENSE.txt
    and license.txt name one file there and would otherwise report twice.
    """
    wanted = {n.lower() for n in LICENSE_FILE_NAMES}
    found: dict[Path, Path] = {}

    def collect(directory: Path) -> None:
        try:
            entries = sorted(directory.iterdir())
        except OSError:
            return
        for entry in entries:
            if entry.name.lower() in wanted and entry.is_file():
                found.setdefault(entry.resolve(), entry)

    collect(tree)
    for child in sorted(p for p in tree.iterdir() if p.is_dir()):
        if child.name in {"doc", "docs", "test", "tests", "samples", "thirdparty"}:
            continue
        collect(child)
    return list(found.values())


def verify_dep_license(dep: dict) -> tuple[str, list[str]]:
    """Check a dependency's on-disk license against its manifest claim.

    Returns (status, problems) where status is one of verified / unverified.
    """
    declared = dep.get("license", "")
    tree = local_source_tree(dep)
    if tree is None:
        return "unverified", []

    license_files = find_license_files(tree)
    if not license_files:
        return "unverified", []

    problems: list[str] = []
    for path in license_files:
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue
        rel = path.relative_to(tree)
        flat = flatten(text)
        for marker in COPYLEFT_MARKERS:
            if marker not in flat:
                continue
            severity = "review-required" if marker in REVIEW_ONLY_MARKERS else "FORBIDDEN"
            problems.append(
                f"{rel} offers {marker} ({severity}) but the manifest declares "
                f"{declared!r}"
            )
    return "verified", problems


RAW_DOWNLOAD = re.compile(r"\bfile\s*\(\s*DOWNLOAD\b", re.IGNORECASE)
FETCHCONTENT_DECLARE = re.compile(r"\bFetchContent_Declare\s*\(", re.IGNORECASE)


def _cmake_files(tree: Path) -> list[Path]:
    return sorted(p for p in tree.rglob("*")
                  if p.is_file() and (p.suffix == ".cmake" or p.name == "CMakeLists.txt"))


# A configured build directory to read `<name>_SOURCE_DIR` from (--build-dir):
# the exact tree that build configured with, wherever FetchContent put it.
BUILD_DIR: Path | None = None


def _build_source_dir(name: str) -> Path | None:
    cache = BUILD_DIR / "CMakeCache.txt" if BUILD_DIR else None
    if cache is None or not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(f"{name}_SOURCE_DIR:") and "=" in line:
            path = Path(line.split("=", 1)[1])
            return path if path.is_dir() else None
    return None


def offline_fetch_tree(dep: dict, contract: dict) -> Path | None:
    """The pinned tree to scan. A configured build directory names the exact
    source it used. Otherwise FetchContent names its shared-cache entry after
    the CMake dependency name and the full ref (`webgpu-<40-hex sha>`), which
    neither the manifest name nor its short pinned ref spell, so the contract
    names the cache entry and the pin is matched as a ref prefix."""
    name, version = contract.get("cache_name", ""), dep.get("version", "")
    built = _build_source_dir(name) if name else None
    if built is not None:
        return built
    tree = local_source_tree(dep)
    if tree is not None:
        return tree
    if not (name and version and FETCHCONTENT_CACHE.is_dir()):
        return None
    matches = sorted(p for p in FETCHCONTENT_CACHE.iterdir()
                     if p.is_dir() and p.name.startswith(f"{name}-{version}"))
    return matches[0] if len(matches) == 1 else None


def verify_offline_fetch(dep: dict) -> tuple[str, list[str]]:
    """A dependency marked `offline_fetch` must fetch only through FetchContent.

    A changed-surface bounded run derives its base inventory with a
    FETCHCONTENT_FULLY_DISCONNECTED configure
    (tools/scripts/run_changed_surface_tests.py base_projection), so a miss
    in the shared source cache fails instead of downloading. A raw
    file(DOWNLOAD) anywhere in the pinned tree escapes that and could reach
    the network mid-plan. Returns (status, problems); a tree that is not
    checked out is "unverified", never a pass.
    """
    contract = dep.get("offline_fetch")
    if not contract:
        return "not-required", []
    tree = offline_fetch_tree(dep, contract)
    if tree is None:
        return "unverified", []
    files = _cmake_files(tree)
    problems = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), start=1):
            if RAW_DOWNLOAD.search(line.split("#", 1)[0]):
                problems.append(
                    f"{path.relative_to(tree)}:{number} uses a raw file(DOWNLOAD), which a "
                    "FETCHCONTENT_FULLY_DISCONNECTED configure does not govern; the bounded "
                    "run's base inventory (run_changed_surface_tests.base_projection) needs "
                    "every download to go through FetchContent, or to be refused there"
                )
    if not any(FETCHCONTENT_DECLARE.search(p.read_text(encoding="utf-8", errors="replace"))
               for p in files):
        problems.append("no FetchContent_Declare found in its CMake files: the offline-fetch "
                        "check read nothing it could vouch for")
    return "verified", problems


def parse_dependencies_md() -> set[str]:
    text = DEPENDENCIES_MD.read_text()
    names: set[str] = set()
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if not cells:
            continue
        first = cells[0]
        if first in {"Name", "SDK", "------", "-----"}:
            continue
        if set(first) == {"-"}:
            continue
        names.add(first)
    return names


def parse_notice_md() -> set[str]:
    text = NOTICE_MD.read_text()
    names: set[str] = set()
    for line in text.splitlines():
        if line.startswith("## "):
            names.add(line[3:].strip())
    return names


def parse_licensing_md() -> set[str]:
    """Extract dependency names from docs/reference/licensing.md tables.

    Table rows that attribute a dependency look like:
        | **Highway** | Apache-2.0 | ... | [link] |
    We pull the first-column bolded name so the check mirrors DEPENDENCIES.md.
    """
    text = LICENSING_MD.read_text()
    names: set[str] = set()
    bold_re = re.compile(r"\*\*([^*]+)\*\*")
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if not cells:
            continue
        first = cells[0]
        match = bold_re.search(first)
        if match:
            names.add(match.group(1).strip())
    return names


# ---------------------------------------------------------------------------
# Manifest-source scanners
# ---------------------------------------------------------------------------
#
# Each scanner returns a list of ``DeclaredDep`` records. A ``DeclaredDep``
# names a dependency as it is written in the upstream manifest file (pip
# package name, CMake FetchContent target, etc.) alongside the source file
# + optional path context so the audit can render a useful diagnostic.
#
# The audit normalizes both the declared name and each manifest entry
# (canonical name + ``external_names`` aliases) before comparing, so that
# casing / punctuation differences like ``webgpu`` vs ``WebGPU-distribution``
# or ``mbedtls`` vs ``Mbed TLS`` match.


@dataclass(frozen=True)
class DeclaredDep:
    name: str
    source: str  # human-readable source label, e.g. "requirements-docs.txt"
    location: str = ""  # optional extra path / line hint
    url: str = ""  # upstream URL when the declaration names one


def _normalise(name: str) -> str:
    """Canonicalise a dependency name for comparison.

    Strips all non-alphanumeric characters and lowercases the result, so
    ``Mbed TLS``, ``mbedtls``, and ``mbed-tls`` all compare equal.
    """
    return re.sub(r"[^a-z0-9]", "", name.lower())


# Some manifest entries have well-known upstream aliases that aren't
# captured explicitly in manifest.json. Populate a small default map so
# the audit works even without entries adding ``external_names`` lists.
# This is a presentation helper only — manifest entries can still add
# ``external_names`` for their authoritative alias list.
DEFAULT_ALIASES: dict[str, tuple[str, ...]] = {
    "CHOC": ("choc",),
    "WebGPU-distribution": ("webgpu",),
    "Mbed TLS": ("mbedtls",),
    "three.js": ("threejs",),
    "Catch2": ("catch2",),
    "LV2": ("lv2",),
    "CLAP": ("clap",),
    "Yoga": ("yoga",),
    "Highway": ("highway", "hwy"),
    "DRACO": ("draco",),
    "SDL3": ("sdl3",),
    "pybind11": ("pybind11",),
    "node-addon-api": ("nodeaddonapi",),
    "pugixml": ("pugixml",),
    "miniz": ("miniz",),
    "cpp-httplib": ("cpphttplib", "httplib"),
    "dr_libs": ("drlibs", "dr_flac", "dr_mp3", "dr_wav"),
    "nanosvg": ("nanosvg",),
    "AudioUnitSDK": ("audiounitsdk", "AudioUnitSDK"),
    "VST3 SDK": ("vst3sdk", "vst3"),
    "Inter": ("inter", "fonts"),
    "JetBrains Mono": ("jetbrainsmono", "fonts"),
    "Skia": ("skia", "skia-build"),
    "Dawn": ("dawn",),
    "msdfgen": ("msdfgen",),
    "Oboe": ("oboe",),
    "MTS-ESP": ("mtsesp", "mts_esp"),
    "mkdocs-material": ("material", "mkdocsmaterial"),
    "mkdocs": ("mkdocs",),
    "mkdocs-awesome-pages-plugin": ("awesomepages", "awesomepagesplugin"),
    "mkdocs-git-revision-date-localized-plugin": (
        "gitrevisiondatelocalized",
        "gitrevisiondatelocalizedplugin",
        "git-revision-date-localized",
    ),
    "pymdown-extensions": ("pymdownextensions", "pymdownx"),
    "Pygments": ("pygments",),
    "Material Design Icons": ("materialdesignicons", "mdi"),
}


def manifest_alias_set(dep: dict) -> set[str]:
    """Return the set of normalized aliases that match this manifest entry."""
    keys = {_normalise(dep["name"])}
    for alias in dep.get("external_names", ()):  # explicit entries first
        keys.add(_normalise(alias))
    for alias in DEFAULT_ALIASES.get(dep["name"], ()):
        keys.add(_normalise(alias))
    return keys


def parse_requirements_docs() -> list[DeclaredDep]:
    if not REQUIREMENTS_DOCS.exists():
        return []
    out: list[DeclaredDep] = []
    for raw in REQUIREMENTS_DOCS.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        # strip env markers + version specifiers
        line = line.split(";", 1)[0].strip()
        match = re.match(r"[A-Za-z0-9_.\-]+", line)
        if not match:
            continue
        out.append(DeclaredDep(name=match.group(0), source="requirements-docs.txt"))
    return out


_MKDOCS_THEME_RE = re.compile(r"^theme:\s*$|^\s+name:\s*([A-Za-z0-9_.\-]+)\s*$")


def parse_mkdocs_yml() -> list[DeclaredDep]:
    """Return the declared theme and plugins from mkdocs.yml.

    This parser does not depend on PyYAML to keep the audit runnable in
    minimal environments. It walks the file line-by-line extracting:

    * ``theme.name`` — surfaces ``material`` when mkdocs-material is used
    * ``plugins:`` list entries — e.g. ``awesome-pages``,
      ``git-revision-date-localized``
    * ``markdown_extensions:`` entries beginning with ``pymdownx.`` —
      captured as ``pymdown-extensions`` (only once)

    Name normalization later maps ``material`` to ``mkdocs-material``,
    ``awesome-pages`` to ``mkdocs-awesome-pages-plugin``, etc., via the
    ``DEFAULT_ALIASES`` table + optional per-entry ``external_names``.
    """
    if not MKDOCS_YML.exists():
        return []
    out: list[DeclaredDep] = []
    in_plugins = False
    in_theme = False
    in_markdown_exts = False
    saw_pymdownx = False
    text = MKDOCS_YML.read_text()
    for raw in text.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        stripped = raw.rstrip()
        indent = len(stripped) - len(stripped.lstrip(" "))

        if re.match(r"^theme:\s*$", stripped):
            in_theme, in_plugins, in_markdown_exts = True, False, False
            continue
        if re.match(r"^plugins:\s*$", stripped):
            in_plugins, in_theme, in_markdown_exts = True, False, False
            continue
        if re.match(r"^markdown_extensions:\s*$", stripped):
            in_markdown_exts, in_theme, in_plugins = True, False, False
            continue
        if stripped and indent == 0:
            # left whatever section we were in
            in_plugins = in_theme = in_markdown_exts = False

        if in_theme:
            m = re.match(r"^\s+name:\s*([A-Za-z0-9_.\-]+)\s*$", stripped)
            if m:
                out.append(DeclaredDep(
                    name=m.group(1),
                    source="mkdocs.yml",
                    location="theme.name",
                ))

        if in_plugins:
            # matches both `  - search` and `  - git-revision-date-localized:`
            m = re.match(r"^\s+-\s+([A-Za-z0-9_.\-]+)\s*:?\s*$", stripped)
            if m:
                out.append(DeclaredDep(
                    name=m.group(1),
                    source="mkdocs.yml",
                    location="plugins",
                ))

        if in_markdown_exts and not saw_pymdownx:
            # Surface pymdown-extensions once if any pymdownx.* line appears.
            m = re.match(r"^\s+-\s+pymdownx\.[A-Za-z0-9_.\-]+\s*:?\s*$", stripped)
            if m:
                out.append(DeclaredDep(
                    name="pymdown-extensions",
                    source="mkdocs.yml",
                    location="markdown_extensions",
                ))
                saw_pymdownx = True
    return out


_FETCHCONTENT_RE = re.compile(r"FetchContent_Declare\s*\(\s*([A-Za-z0-9_.\-]+)")


def parse_fetchcontent(cmake_file: Path) -> list[DeclaredDep]:
    if not cmake_file.exists():
        return []
    out: list[DeclaredDep] = []
    text = cmake_file.read_text()
    for match in _FETCHCONTENT_RE.finditer(text):
        out.append(DeclaredDep(
            name=match.group(1),
            source=str(cmake_file.relative_to(ROOT)),
            location="FetchContent_Declare",
        ))
    return out


# ── Repo-wide fetch scan ─────────────────────────────────────────────────────
#
# Every CMake file and setup script that can fetch something is read, not just
# the root CMakeLists.txt: a FetchContent_Declare in tools/cmake/ or a curl in
# setup.sh is as much a dependency as one in the root file.

# Path prefixes that are never scanned, with the reason. Anything else that
# fetches must have a manifest row.
SCAN_EXCLUDED_PREFIXES: dict[str, str] = {
    "external/": "vendored upstream trees; their own build logic is upstream's",
    "planning/": "private planning submodule, not part of the build",
    "tools/packages/test-stubs/": "freshness-check consumers of `pulp add` registry "
                                  "packages (tools/packages/registry.json); built only "
                                  "by freshness-check.yml, never part of Pulp",
}
# Path fragments (anywhere in the path) that mark test fixtures.
SCAN_EXCLUDED_FRAGMENTS: tuple[str, ...] = ("/tests/fixtures/", "/test/fixtures/")

_SETUP_SCRIPT_RE = re.compile(r"(^|/)(setup|bootstrap)[^/]*\.(sh|ps1)$")
# Any other shell script under tools/ or hooks/ is read for curl/wget
# downloads only.
_TOOLS_SHELL_RE = re.compile(r"^(tools|hooks)/.*\.sh$")
_CMAKE_FETCH_CALL_RE = re.compile(
    r"\b(FetchContent_Declare|ExternalProject_Add|execute_process|file)\s*\(", re.IGNORECASE)
_CMAKE_SET_RE = re.compile(
    r"\bset\s*\(\s*([A-Za-z0-9_]+)\s+(\"[^\"]*\"|[^\s\)\"]+)", re.IGNORECASE)
_SHELL_ASSIGN_RE = re.compile(
    r"^\s*(?:local\s+|export\s+|readonly\s+)?\$?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r"(\"[^\"]*\"|'[^']*'|[^\s;#]+)", re.MULTILINE)
_URL_RE = re.compile(r"(?:https?|git)://[^\s\"'()<>;]+|git@[A-Za-z0-9_.\-]+:[^\s\"'()<>;]+")
_SHELL_FETCH_RE = re.compile(
    r"(^|[\s\"'(|;&])(curl|wget|git\s+clone|Invoke-WebRequest|iwr|Start-BitsTransfer"
    r"|ensure_shared_[a-z_]*source[a-z_]*)\b")
_SHELL_HTTP_FETCH_RE = re.compile(r"(^|[\s\"'(|;&])(curl|wget)\b")

# Pulp's own repository and site: a script that downloads Pulp (an installer,
# a release helper) is not fetching a third-party dependency.
SELF_URL_IDENTITIES: tuple[str, ...] = (
    "github.com/generous-corp/pulp",
    "github.com/danielraffel/pulp",
    "www.generouscorp.com/pulp",
)

# Fetches the scan reports that have no manifest row yet, keyed by the script
# and the exact URL the scan resolves (so another asset from the same upstream
# in the same script is still reported), each with the reason it is tolerated
# and what would resolve it. They are printed on every run and
# do not fail --strict; an entry whose fetch no longer exists is an error, so
# the list cannot quietly outlive what it excuses.
KNOWN_UNLISTED_FETCHES: dict[tuple[str, str], str] = {
    ("tools/ci/tart-provision.sh",
     "https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh"):
        "CI-host provisioning: installs Homebrew into a CI VM image; nothing it "
        "fetches is redistributed. Follow-up: decide whether CI-host tooling "
        "belongs in the manifest.",
    ("tools/ci/tart-provision.sh",
     "https://github.com/actions/runner/releases/download/v2.334.0/"
     "actions-runner-osx-arm64-2.334.0.tar.gz"):
        "CI-host provisioning: GitHub Actions runner for a CI VM image, not "
        "redistributed; fetched by an unpinned version variable without a "
        "digest. Follow-up: pin by digest or record a manifest row.",
    ("tools/ci/tart-provision.sh", "https://sh.rustup.rs"):
        "CI-host provisioning: rustup installer piped to sh in a CI VM image, "
        "not redistributed. Follow-up: same decision as the other CI-host tools.",
}


def split_known_unlisted(
    uncovered: list[DeclaredDep],
    known: dict[tuple[str, str], str] | None = None,
) -> tuple[list[DeclaredDep], list[DeclaredDep], list[tuple[str, str]]]:
    """Split uncovered fetches into (unknown, known, stale known entries)."""
    known = KNOWN_UNLISTED_FETCHES if known is None else known
    unknown: list[DeclaredDep] = []
    tolerated: list[DeclaredDep] = []
    used: set[tuple[str, str]] = set()
    for dep in uncovered:
        key = (dep.source, dep.url)
        if key not in known:
            key = None
        if key is None:
            unknown.append(dep)
        else:
            tolerated.append(dep)
            used.add(key)
    stale = sorted(set(known) - used)
    return unknown, tolerated, stale


def _scan_excluded(rel: str) -> bool:
    if any(rel.startswith(prefix) for prefix in SCAN_EXCLUDED_PREFIXES):
        return True
    return any(fragment in "/" + rel for fragment in SCAN_EXCLUDED_FRAGMENTS)


def _is_scanned_file(rel: str) -> bool:
    name = rel.rsplit("/", 1)[-1]
    return (name == "CMakeLists.txt" or name.endswith(".cmake")
            or bool(_SETUP_SCRIPT_RE.search(rel)) or bool(_TOOLS_SHELL_RE.search(rel)))


def fetch_scan_files(root: Path) -> list[Path]:
    """CMake files and setup scripts under ``root`` that the scan reads.

    In a git checkout this is the tracked plus untracked-but-not-ignored files,
    so build directories (ignored) and nested worktrees (separate repos) never
    count. Outside git (test fixtures) the tree is walked, skipping build dirs.
    """
    rels: list[str] = []
    listed = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
         "--exclude-standard"],
        capture_output=True, check=False,
    )
    toplevel = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, encoding="utf-8", check=False,
    )
    in_repo = (listed.returncode == 0 and toplevel.returncode == 0
               and Path(toplevel.stdout.strip()).resolve() == root.resolve())
    if in_repo:
        rels = [p for p in listed.stdout.decode("utf-8", "replace").split("\0") if p]
    else:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if d != ".git" and d != "_deps" and not d.startswith("build")]
            for filename in filenames:
                rels.append((Path(dirpath) / filename).relative_to(root).as_posix())
    out = []
    for rel in sorted(set(rels)):
        if _scan_excluded(rel) or not _is_scanned_file(rel):
            continue
        path = root / rel
        if path.is_file():
            out.append(path)
    return out


def _strip_cmake_comments(text: str) -> str:
    lines = []
    for line in text.splitlines():
        in_quote = False
        cut = len(line)
        for index, char in enumerate(line):
            if char == '"' and (index == 0 or line[index - 1] != "\\"):
                in_quote = not in_quote
            elif char == "#" and not in_quote:
                cut = index
                break
        lines.append(line[:cut])
    return "\n".join(lines)


def _call_body(text: str, open_paren: int) -> str:
    """The argument text of the call whose ``(`` is at ``open_paren``."""
    depth = 0
    for index in range(open_paren, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:index]
    return text[open_paren + 1:]


def _resolve_vars(value: str, variables: dict[str, str]) -> str:
    for _ in range(5):
        # ${VAR:-default}: the assigned value when known, else the default.
        updated = re.sub(r"\$\{([A-Za-z0-9_]+):-([^}]*)\}",
                         lambda m: variables.get(m.group(1), m.group(2)), value)
        updated = re.sub(r"\$\{([A-Za-z0-9_]+)\}",
                         lambda m: variables.get(m.group(1), m.group(0)), updated)
        updated = re.sub(r"\$([A-Za-z_][A-Za-z0-9_]*)",
                         lambda m: variables.get(m.group(1), m.group(0)), updated)
        if updated == value:
            break
        value = updated
    return value


def _url_key(url: str) -> tuple[str, ...]:
    """Comparable identity for an upstream URL, as path components:
    ``("github.com", owner, repo)`` for GitHub (any of its hosts), otherwise the
    host followed by the path segments. Matching compares whole components,
    so ``github.com/acme/tool`` never matches ``github.com/acme/toolkit``."""
    url = url.strip().strip("\"'").rstrip("/")
    url = re.sub(r"^git@([^:]+):", r"https://\1/", url)
    url = re.sub(r"^[a-z+]+://", "", url, flags=re.IGNORECASE)
    url = url.split("?", 1)[0].split("#", 1)[0]
    host, _, path = url.partition("/")
    host = host.lower().split("@")[-1]
    parts = [p.lower() for p in path.split("/") if p]
    if parts:
        parts[-1] = parts[-1].removesuffix(".git")
    if host in {"github.com", "www.github.com", "codeload.github.com",
                "raw.githubusercontent.com", "objects.githubusercontent.com"}:
        if len(parts) >= 2:
            return ("github.com", parts[0], parts[1].removesuffix(".git"))
    if host == "api.github.com" and len(parts) >= 3 and parts[0] == "repos":
        return ("github.com", parts[1], parts[2].removesuffix(".git"))
    return (host, *parts)


def _key_from_identity(identity: str) -> tuple[str, ...]:
    """A ``host/owner/repo`` constant written as text, as a component key."""
    return tuple(identity.lower().split("/"))


def _key_within(key: tuple[str, ...], prefix: tuple[str, ...]) -> bool:
    return key[:len(prefix)] == prefix


def _url_is_static(url: str) -> bool:
    return not any("$" in part for part in _url_key(url))


def _name_from_url(url: str) -> str:
    key = _url_key(url)
    if key[0] == "github.com" and len(key) == 3:
        return key[2]
    return key[-1]


def _first_url(text: str) -> str:
    match = _URL_RE.search(text)
    return match.group(0) if match else ""


def parse_cmake_fetches(path: Path, root: Path) -> list[DeclaredDep]:
    """FetchContent_Declare / ExternalProject_Add / file(DOWNLOAD) in a CMake file."""
    text = _strip_cmake_comments(path.read_text(encoding="utf-8", errors="replace"))
    variables = {}
    for match in _CMAKE_SET_RE.finditer(text):
        variables.setdefault(match.group(1), match.group(2).strip('"'))
    rel = path.relative_to(root).as_posix()
    out: list[DeclaredDep] = []
    for match in _CMAKE_FETCH_CALL_RE.finditer(text):
        command = match.group(1).lower()
        body = _call_body(text, match.end() - 1)
        tokens = body.split()
        if not tokens:
            continue
        line = text.count("\n", 0, match.start()) + 1
        if command == "execute_process":
            # A download run as a process at configure time.
            words = [w.strip('"') for w in tokens]
            if not any(os.path.basename(w) in {"curl", "wget"} for w in words):
                continue
            for url in _URL_RE.findall(_resolve_vars(body, variables)):
                if not _url_is_static(url):
                    continue
                out.append(DeclaredDep(name=_name_from_url(url), source=rel,
                                       location=f"execute_process download line {line}",
                                       url=url))
            continue
        if command == "file":
            if tokens[0].upper() != "DOWNLOAD" or len(tokens) < 2:
                continue
            url = _resolve_vars(tokens[1].strip('"'), variables)
            if not _URL_RE.match(url) or not _url_is_static(url):
                continue  # URL not statically known; nothing to attribute
            out.append(DeclaredDep(name=_name_from_url(url), source=rel,
                                   location=f"file(DOWNLOAD) line {line}", url=url))
            continue
        name = tokens[0].strip('"')
        url = _resolve_vars(_first_url(body), variables) if _first_url(body) else ""
        if url and not _url_is_static(url):
            url = ""
        label = "FetchContent_Declare" if command == "fetchcontent_declare" else "ExternalProject_Add"
        if "SOURCE_DIR" in tokens and not url:
            continue  # a local source override, not a fetch
        out.append(DeclaredDep(name=name, source=rel, location=f"{label} line {line}", url=url))
    return out


def parse_setup_script_fetches(path: Path, root: Path,
                               fetch_re: re.Pattern = _SHELL_FETCH_RE) -> list[DeclaredDep]:
    """Downloads in a shell script with a statically known URL. Setup and
    bootstrap scripts count every fetch verb; other tools/ scripts pass
    ``_SHELL_HTTP_FETCH_RE`` and count curl/wget only."""
    raw = path.read_text(encoding="utf-8", errors="replace")
    joined = re.sub(r"\\\r?\n", " ", raw)  # backslash continuations
    variables: dict[str, str] = {}
    for match in _SHELL_ASSIGN_RE.finditer(joined):
        value = match.group(2).strip("\"'")
        if "$(" not in value and "`" not in value:  # command output is not static
            variables.setdefault(match.group(1), value)
    rel = path.relative_to(root).as_posix()
    out: list[DeclaredDep] = []
    for number, line in enumerate(joined.splitlines(), start=1):
        if line.lstrip().startswith("#") or not fetch_re.search(line):
            continue
        for url in _URL_RE.findall(_resolve_vars(line, variables)):
            if not _url_is_static(url) or "example.invalid" in url:
                continue
            out.append(DeclaredDep(name=_name_from_url(url), source=rel,
                                   location=f"download line {number}", url=url))
    return out


def scan_fetch_declarations(root: Path | None = None) -> list[DeclaredDep]:
    """Every fetch the repo's CMake files and setup scripts declare."""
    root = root or ROOT
    out: list[DeclaredDep] = []
    for path in fetch_scan_files(root):
        rel = path.relative_to(root).as_posix()
        if _SETUP_SCRIPT_RE.search(rel):
            out.extend(parse_setup_script_fetches(path, root))
        elif _TOOLS_SHELL_RE.search(rel):
            out.extend(parse_setup_script_fetches(path, root, _SHELL_HTTP_FETCH_RE))
        else:
            out.extend(parse_cmake_fetches(path, root))
    return out


# external/ subdirectories that are documentation-only (no redistributed
# source) and therefore need no manifest entry of their own.
EXTERNAL_IGNORE = {"fonts"}  # covered by Inter / JetBrains Mono entries
EXTERNAL_ALIASES = {"fonts": ("Inter", "JetBrains Mono")}


def parse_external_dirs() -> list[DeclaredDep]:
    if not EXTERNAL_DIR.is_dir():
        return []
    out: list[DeclaredDep] = []
    for child in sorted(EXTERNAL_DIR.iterdir()):
        if not child.is_dir() or child.name.startswith("."):
            continue
        if child.name in EXTERNAL_IGNORE:
            continue
        out.append(DeclaredDep(
            name=child.name,
            source="external/",
            location=child.name + "/",
        ))
    return out


def parse_redistributed_tooling() -> list[DeclaredDep]:
    """Surface committed binary bootstrap tools that need attribution."""
    if not GRADLE_WRAPPER_JAR.is_file():
        return []
    return [DeclaredDep(
        name="Gradle Wrapper",
        source="android/gradle/wrapper/gradle-wrapper.jar",
        location="redistributed binary",
    )]


def collect_declared(
    extra_requirements: Path | None = None,
    extra_mkdocs: Path | None = None,
    extra_cmake: list[Path] | None = None,
    scan_root: Path | None = None,
) -> list[DeclaredDep]:
    """Aggregate declared deps across all manifest sources.

    ``extra_*`` are injection hooks for tests — they replace the default
    paths when provided. Tests pass synthetic fixtures to verify the
    completeness gate catches missing deps without touching real repo
    files. ``extra_cmake`` replaces the repo-wide fetch scan with exactly
    those files; ``scan_root`` points the repo-wide scan at another tree.
    """
    global REQUIREMENTS_DOCS, MKDOCS_YML, EXTRA_CMAKELISTS, ROOT_CMAKELISTS
    saved = (REQUIREMENTS_DOCS, MKDOCS_YML, list(EXTRA_CMAKELISTS), ROOT_CMAKELISTS)
    try:
        if extra_requirements is not None:
            REQUIREMENTS_DOCS = extra_requirements
        if extra_mkdocs is not None:
            MKDOCS_YML = extra_mkdocs
        if extra_cmake is not None:
            ROOT_CMAKELISTS = extra_cmake[0] if extra_cmake else ROOT_CMAKELISTS
            EXTRA_CMAKELISTS = list(extra_cmake[1:]) if len(extra_cmake) > 1 else []

        declared: list[DeclaredDep] = []
        declared.extend(parse_requirements_docs())
        declared.extend(parse_mkdocs_yml())
        if extra_cmake is not None:
            declared.extend(parse_fetchcontent(ROOT_CMAKELISTS))
            for cm in EXTRA_CMAKELISTS:
                declared.extend(parse_fetchcontent(cm))
        else:
            declared.extend(scan_fetch_declarations(scan_root or ROOT))
        declared.extend(parse_external_dirs())
        declared.extend(parse_redistributed_tooling())
        return declared
    finally:
        REQUIREMENTS_DOCS, MKDOCS_YML, EXTRA_CMAKELISTS, ROOT_CMAKELISTS = saved


def find_uncovered_declarations(
    manifest: list[dict],
    declared: list[DeclaredDep],
) -> list[DeclaredDep]:
    """Return declared deps that aren't backed by a manifest entry."""
    covered: set[str] = set()
    covered_urls: set[tuple[str, ...]] = set()
    for dep in manifest:
        covered |= manifest_alias_set(dep)
        for url in [dep.get("repository", "")] + [
            art.get("url", "") for art in dep.get("artifacts", [])
        ] + list(dep.get("upstream_urls", ())):
            if url and "://" in url:
                covered_urls.add(_url_key(url))
    covered_urls.update(_key_from_identity(identity) for identity in SELF_URL_IDENTITIES)
    # Multi-alias external dirs (e.g. external/fonts → Inter + JetBrains Mono)
    # collapse into the aliases of their represented entries.
    for dir_name, aliases in EXTERNAL_ALIASES.items():
        for alias in aliases:
            covered.add(_normalise(alias))

    # A small set of declared names we ignore entirely — these are
    # MkDocs built-ins (``search``), fonts referenced via Google Fonts
    # CDN (``Inter``, ``JetBrains Mono`` are already vendored manifest
    # entries so the normaliser catches them).
    ignored = {_normalise(n) for n in {"search"}}

    uncovered: list[DeclaredDep] = []
    for dep in declared:
        key = _normalise(dep.name)
        if key in ignored:
            continue
        if dep.url:
            # A declaration that names its upstream is matched by that
            # upstream: a FetchContent name is a local label (choc_wclap,
            # moonbase_cpp), and a different repo under a familiar name is a
            # different dependency.
            key = _url_key(dep.url)
            if any(_key_within(key, known) for known in covered_urls):
                continue
            uncovered.append(dep)
            continue
        if key in covered:
            continue
        uncovered.append(dep)
    return uncovered


# ── Shared renderer lock agreement ─────────────────────────────────────────
#
# gpu-runtime's renderer-lock.json is the one place the Skia/Dawn milestone and
# the skia-builder digests are written; Pulp's manifest repeats them for its own
# fetch, so --strict reads the lock at the exact commit Pulp pins gpu-runtime to
# and requires it to name Pulp's archive digest and Skia release. The comparison
# is fail-closed: a lock that cannot be read is a failure, never a skipped check.
# A spec whose path is a CMake module (the pre-lock layout) is still read with
# the regexes below.
VELLUM_DRIFT_FIELD = "vellum_renderer_drift_check"
GPU_RUNTIME_ENTRY = "vellum-gpu-runtime"
VELLUM_CHECKOUT_ENV = "PULP_VELLUM_CHECKOUT"
PIN_TRANSITION_FILE = "tools/deps/pin_transition.json"
VELLUM_ARCHIVE_SHA_RE = re.compile(
    r'set\(\s*_vellum_expected_archive_sha256\s+"([0-9A-Fa-f]{64})"\s*\)')
VELLUM_TUPLE_RE = re.compile(r"(chrome/m\d+)\s+macOS universal tuple")
FULL_SHA_RE = re.compile(r"[0-9a-f]{40}")


def validate_pin_transition(transition: object, artifact_id: str, old_digest: str,
                            new_digest: str, *, now: datetime.datetime) -> list[str]:
    """Validate a time-bounded old/new digest transition."""
    if not isinstance(transition, dict) or transition.get("schema") != "pulp.pin-transition.v1":
        return ["pin transition has an invalid schema"]
    errors: list[str] = []
    for field in ("owner", "reason"):
        if not isinstance(transition.get(field), str) or not transition[field].strip():
            errors.append(f"pin transition has no {field}")

    dates = {}
    for field in ("issued_at", "expires_at"):
        value = transition.get(field)
        try:
            date = datetime.datetime.fromisoformat(value.replace("Z", "+00:00")) if isinstance(value, str) else None
        except ValueError:
            date = None
        if date is None or date.tzinfo is None:
            errors.append(f"pin transition has an invalid {field}")
        else:
            dates[field] = date
    if len(dates) == 2:
        issued, deadline = dates["issued_at"], dates["expires_at"]
        if issued > now:
            errors.append("pin transition issued_at is in the future")
        if deadline <= now:
            errors.append("pin transition is expired")
        if not datetime.timedelta(0) < deadline - issued <= datetime.timedelta(days=14):
            errors.append("pin transition lifetime must be positive and at most 14 days")

    entries = transition.get("entries")
    if not isinstance(entries, list):
        return errors + ["pin transition entries must be a list"]
    matches = [entry for entry in entries
               if isinstance(entry, dict) and entry.get("artifact_id") == artifact_id]
    if len(entries) != 1 or len(matches) != 1:
        return errors + [f"pin transition must have exactly one entry for {artifact_id}"]
    entry = matches[0]
    old = entry.get("old_sha256")
    new = entry.get("new_sha256")
    if not all(isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{64}", value)
               for value in (old, new)):
        errors.append(f"pin transition {artifact_id} has malformed digests")
    elif old.lower() == new.lower() or old.lower() != old_digest.lower() or (
            new.lower() != new_digest.lower()):
        errors.append(f"pin transition {artifact_id} does not name the observed old/new digests")
    if not isinstance(entry.get("reason"), str) or not entry["reason"].strip():
        errors.append(f"pin transition {artifact_id} has no reason")
    if not isinstance(entry.get("owner"), str) or not entry["owner"].strip():
        errors.append(f"pin transition {artifact_id} has no owner")
    return errors


def renderer_transition(artifact_id: str, old_digest: str, new_digest: str, *,
                        now: datetime.datetime) -> list[str]:
    """Read and validate the checked-in transition record, failing closed."""
    path = ROOT / PIN_TRANSITION_FILE
    try:
        transition = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return [f"{PIN_TRANSITION_FILE} cannot be read: {error}"]
    return validate_pin_transition(transition, artifact_id, old_digest, new_digest, now=now)


class VellumPinUnavailable(RuntimeError):
    """Vellum's pinned renderer module could not be read at the pinned commit."""


def read_vellum_pin_bytes(repository: str, commit: str, path: str) -> bytes:
    """The exact bytes of Vellum's `path` at `commit`, from a local checkout or GitHub.

    A checkout named by PULP_VELLUM_CHECKOUT is read with `git show`, so the
    override still compares the pinned commit rather than whatever that checkout
    has on disk. Bytes, not text, so a digest over them matches the file.
    """
    checkout = os.environ.get(VELLUM_CHECKOUT_ENV)
    if checkout:
        try:
            result = subprocess.run(
                ["git", "-C", checkout, "show", f"{commit}:{path}"],
                capture_output=True, timeout=30, check=False,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise VellumPinUnavailable(f"git show in {checkout} failed: {error}") from error
        if result.returncode != 0:
            stderr = result.stderr.decode("utf-8", errors="replace").strip()
            raise VellumPinUnavailable(
                f"{VELLUM_CHECKOUT_ENV}={checkout} cannot show {commit}:{path}: "
                f"{stderr or 'git exited ' + str(result.returncode)}")
        return result.stdout
    match = GITHUB_REPO_RE.search(repository)
    if not match:
        raise VellumPinUnavailable(f"repository is not a GitHub URL: {repository!r}")
    owner, name = match.groups()
    url = f"https://raw.githubusercontent.com/{owner}/{name}/{commit}/{path}"
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            return response.read()
    except (urllib.error.URLError, OSError) as error:
        raise VellumPinUnavailable(f"cannot read {url}: {error}") from error


def read_vellum_pin_source(repository: str, commit: str, path: str) -> str:
    """Vellum's renderer module or lock at `commit`, as text (see read_vellum_pin_bytes)."""
    data = read_vellum_pin_bytes(repository, commit, path)
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise VellumPinUnavailable(f"{path} at {commit} is not UTF-8: {error}") from error


def vellum_renderer_drift(manifest: list[dict], read_source=None, *,
                          now: datetime.datetime | None = None) -> tuple[str, list[str]]:
    """Compare gpu-runtime's renderer lock (or Vellum's older pin) with Pulp's Skia entry.

    Returns (status, problems): status is "not-configured" when the Skia entry
    declares no comparison, "match" when the pins agree or a trusted base
    transition covers the drift, and "mismatch" or "unavailable" on failure.
    """
    skia = next((dep for dep in manifest if dep.get("name") == "Skia"), None)
    if skia is None or VELLUM_DRIFT_FIELD not in skia:
        return "not-configured", []
    spec = skia[VELLUM_DRIFT_FIELD]
    if not isinstance(spec, dict):
        return "unavailable", [f"Skia {VELLUM_DRIFT_FIELD} is not an object"]
    repository = spec.get("repository")
    commit = spec.get("commit")
    path = spec.get("path")
    asset_key = spec.get("pulp_release_asset")
    problems = []
    if not isinstance(repository, str) or not repository:
        problems.append("repository is missing")
    if not isinstance(commit, str) or not FULL_SHA_RE.fullmatch(commit):
        problems.append(f"commit must be a full 40-hex SHA, not {commit!r}")
    if (not isinstance(path, str) or not path or path.startswith("/")
            or ".." in Path(path).parts):
        problems.append(f"path must be repository-relative, not {path!r}")
    assets = (skia.get("determinism") or {}).get("release_assets") or {}
    asset = assets.get(asset_key) if isinstance(asset_key, str) else None
    pulp_sha = asset.get("sha256") if isinstance(asset, dict) else None
    if not isinstance(pulp_sha, str):
        problems.append(f"pulp_release_asset {asset_key!r} names no Skia release asset digest")
    pulp_tag = skia.get("version")
    if problems:
        return "unavailable", [f"Skia {VELLUM_DRIFT_FIELD}: {p}" for p in problems]

    try:
        source = (read_source or read_vellum_pin_source)(repository, commit, path)
    except VellumPinUnavailable as error:
        return "unavailable", [f"Vellum renderer pin unreadable at {commit}: {error}"]
    where = f"{repository} @ {commit} {path}"
    if path.endswith(".json"):
        try:
            lock = json.loads(source)
            platform = lock["platforms"][spec.get("lock_platform", "macos-arm64")]
            vellum_sha, vellum_tag = str(platform["sha256"]).lower(), str(lock["milestone"])
        except (ValueError, KeyError, TypeError) as error:
            return "unavailable", [f"{where} is not a renderer lock with that platform: {error!r}"]
    else:
        digests = set(VELLUM_ARCHIVE_SHA_RE.findall(source))
        tags = set(VELLUM_TUPLE_RE.findall(source))
        if len(digests) != 1 or len(tags) != 1:
            return "unavailable", [
                f"{where} does not declare exactly one _vellum_expected_archive_sha256 "
                f"and one 'chrome/mNNN macOS universal tuple' (found digests={sorted(digests)}, "
                f"tags={sorted(tags)})"]
        vellum_sha, vellum_tag = digests.pop().lower(), tags.pop()
    runtime = next((dep for dep in manifest if dep.get("name") == GPU_RUNTIME_ENTRY), None)
    if runtime is not None:
        pinned = ((runtime.get("upstream") or {}).get("commit") or "").lower()
        if pinned != commit.lower():
            problems.append(
                f"the renderer lock is read at {commit}, but Pulp pins {GPU_RUNTIME_ENTRY} "
                f"at {pinned or 'nothing'}; they must be the same commit")
    transition_accepted = False
    if vellum_sha != pulp_sha.lower():
        # Direction matters: the record moves from Vellum's old digest to Pulp's new digest,
        # never the reverse.
        transition_errors = renderer_transition(
            f"renderer/{asset_key}", vellum_sha, pulp_sha.lower(),
            now=now if now is not None else datetime.datetime.now(datetime.timezone.utc))
        transition_accepted = not transition_errors
        if transition_errors:
            problems.append(
                f"archive digest differs: Vellum {vellum_sha} ({where}) vs Pulp {pulp_sha} "
                f"(manifest Skia release_assets.{asset_key}); " + "; ".join(transition_errors))
    # Every macOS build forwards its device layer to gpu-runtime, whose lock
    # pins one macOS asset, so each mac-* selector must resolve that asset: an
    # x86_64-only build that fetched a thin archive fails the lock's Dawn check.
    expected_mac_sha = pulp_sha.lower() if transition_accepted else vellum_sha
    for key in sorted(assets):
        if not key.startswith("mac-") or key == asset_key:
            continue
        other = assets[key].get("sha256") if isinstance(assets[key], dict) else None
        if not isinstance(other, str) or other.lower() != expected_mac_sha:
            if transition_accepted:
                problems.append(
                    f"manifest Skia release_assets.{key} is {other}, expected the Pulp "
                    f"transition digest {expected_mac_sha}")
                continue
            problems.append(
                f"manifest Skia release_assets.{key} is {other}, but gpu-runtime's lock "
                f"pins one macOS asset, {vellum_sha} ({where}); a macOS build that "
                f"resolves {key} would fail its Dawn digest check")
    if vellum_tag != pulp_tag:
        problems.append(
            f"Skia release differs: Vellum {vellum_tag} ({where}) vs Pulp {pulp_tag} "
            "(manifest Skia version)")
    return ("mismatch" if problems else "match"), problems


# The CMake file configure reads the gpu-runtime pin from, and the reader of
# pulp.lock it includes.
GPU_RUNTIME_PIN_FILE = "tools/cmake/PulpGpuRuntime.cmake"
GPU_RUNTIME_LOCK_READER = "tools/cmake/PulpVellumLock.cmake"
GPU_RUNTIME_RECORD_MODULE = "tools/cmake/PulpGpuRuntimeRecord.cmake"
# Evaluates the pin file with real CMake and records what it does (see the file).
GPU_RUNTIME_DRIVER = ROOT / "tools/deps/gpu_runtime_driver.cmake"
GPU_RUNTIME_FETCHED_DIR = "/pulp-driver/vellum_gpu_runtime/gpu-runtime"
CMAKE_ENV = "PULP_CMAKE"
# The one source the declaration may name, and the only options it may carry.
# Anything else (URL, URL_HASH, SOURCE_DIR, DOWNLOAD_COMMAND, SVN_*, HG_*,
# CVS_*, PATCH_COMMAND, ...) could take or alter the source outside the pin.
GPU_RUNTIME_REPOSITORY = "https://github.com/Generous-Corp/vellum.git"
GPU_RUNTIME_DECLARE_OPTIONS = {"GIT_REPOSITORY", "GIT_TAG", "SOURCE_SUBDIR", "GIT_SHALLOW",
                               "GIT_PROGRESS"}
GPU_RUNTIME_DECLARE_FLAGS = {"EXCLUDE_FROM_ALL", "SYSTEM"}
# What the pin file may include: the lock reader and the prefix helpers, which
# the driver really evaluates, and FetchContent itself. Any other include is
# code the driver cannot see, so it fails.
GPU_RUNTIME_ALLOWED_INCLUDES = ("FetchContent",)
# A second lock the audit evaluates the pin file against. The pin must follow
# it, so a pin copied into CMake as a literal cannot pass.
GPU_RUNTIME_PROBE_LOCK = {
    "schema": "pulp.lock.v1",
    "vellum": {"tag": "v9.8.7", "sha": "0123456789abcdef0123456789abcdef01234567",
               "sdk_digest": "1" * 64, "gpu_runtime_digest": "2" * 64},
    "render_toolchain": {"lock_digest": "3" * 64},
}
# Set anywhere in the tree, these make FetchContent use a source other than the
# declared one. A developer's own -D or PULP_FETCHCONTENT_SOURCE_DIR_<NAME>
# override is outside the tree and not audited here.
GPU_RUNTIME_SOURCE_OVERRIDE_VARS = ("FETCHCONTENT_SOURCE_DIR_VELLUM_GPU_RUNTIME",
                                    "FETCHCONTENT_FULLY_DISCONNECTED")
# Set before the pin file runs, this makes the lock reader read another lock.
GPU_RUNTIME_LOCK_PATH_VAR = "PULP_VELLUM_LOCK_FILE"
GPU_RUNTIME_SOURCE_HELPERS = ("pulp_register_fetchcontent_source",
                              "pulp_materialize_mutable_fetchcontent_source")
# Commands that read a variable name without writing it.
CMAKE_READ_ONLY_COMMANDS = {"if", "elseif", "while", "message"}
# Commands the driver records; a function() or macro() of the same name
# elsewhere in the tree would replace the real one in a configure.
GPU_RUNTIME_RELIED_COMMANDS = {
    "include", "set", "string", "file", "foreach", "message", "return", "find_package",
    "get_directory_property", "fetchcontent_declare", "fetchcontent_makeavailable",
}


class GpuRuntimeRun:
    """One evaluation of the pin file: its recorded calls and how it ended."""

    def __init__(self, calls: list[tuple[str, list[str]]], output: str,
                 trace: list[tuple[int, str]] | None = None) -> None:
        self.calls = calls
        self.output = output
        # Every command CMake executed in the pin file, as (line, command),
        # from CMake's own --trace, up to the message() that stopped it.
        self.trace = trace or []

    def named(self, name: str) -> list[tuple[int, list[str]]]:
        return [(i, args) for i, (call, args) in enumerate(self.calls) if call == name]

    @property
    def stopped(self) -> bool:
        """It ended at a message(FATAL_ERROR) or ran to the end of the file."""
        return bool(self.calls) and self.calls[-1][0] in ("message", "end")


def find_cmake() -> str | None:
    return os.environ.get(CMAKE_ENV) or shutil.which("cmake")


def run_gpu_runtime_driver(root: Path, source: str, fetched_version: str, *,
                           prefix: bool = False, lock_file: Path | None = None,
                           cmake: str | None = None) -> GpuRuntimeRun:
    """Evaluate `source` under `root` with gpu_runtime_driver.cmake."""
    with tempfile.TemporaryDirectory() as work:
        record = Path(work) / "record.txt"
        trace = Path(work) / "trace.json"
        command = [cmake or find_cmake() or "cmake", "--trace-format=json-v1",
                   f"--trace-redirect={trace}", f"--trace-source={(root / source).resolve()}",
                   f"-DPULP_DRIVER_ROOT={root.resolve()}",
                   f"-DPULP_DRIVER_MODULE={(root / source).resolve()}",
                   f"-DPULP_DRIVER_RECORD={record}",
                   f"-DPULP_DRIVER_FETCHED_VERSION={fetched_version}",
                   f"-DPULP_DRIVER_PREFIX_MODE={'ON' if prefix else 'OFF'}"]
        if lock_file is not None:
            command.append(f"-D{GPU_RUNTIME_LOCK_PATH_VAR}={lock_file}")
        command += ["-P", str(GPU_RUNTIME_DRIVER)]
        try:
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", timeout=60, check=False)
            output = result.stdout + result.stderr
        except (OSError, subprocess.SubprocessError) as error:
            return GpuRuntimeRun([], f"cannot run {command[0]}: {error}")
        calls = []
        try:
            lines = record.read_text(encoding="utf-8").splitlines()
        except OSError:
            lines = []
        for line in lines:
            name, *fields = line.split("\t")
            calls.append((name, [bytes.fromhex(field).decode("utf-8", "replace")
                                 for field in fields]))
        executed = []
        try:
            entries = trace.read_text(encoding="utf-8").splitlines()
        except OSError:
            entries = []
        for entry in entries:
            try:
                event = json.loads(entry)
            except ValueError:
                continue
            if "cmd" not in event:
                continue
            executed.append((int(event.get("line", 0)), str(event["cmd"]).lower()))
            # CMake keeps tracing block ends while it unwinds from the stop.
            if (executed[-1][1] == "message"
                    and (event.get("args") or [""])[0] in ("FATAL_ERROR", "SEND_ERROR")):
                break
        return GpuRuntimeRun(calls, output, executed)


def _declaration_problems(source: str, args: list[str], sha: str) -> list[str]:
    problems = []
    options: dict[str, list[str]] = {}
    unexpected, index = [], 1
    while index < len(args):
        option = args[index]
        if option in GPU_RUNTIME_DECLARE_FLAGS:
            index += 1
        elif option in GPU_RUNTIME_DECLARE_OPTIONS and index + 1 < len(args):
            options.setdefault(option, []).append(args[index + 1])
            index += 2
        else:
            unexpected.append(option)
            index += 1
    if options.get("GIT_REPOSITORY") != [GPU_RUNTIME_REPOSITORY]:
        problems.append(f"{source}: the vellum_gpu_runtime FetchContent GIT_REPOSITORY must be "
                        f"exactly {GPU_RUNTIME_REPOSITORY} (found {options.get('GIT_REPOSITORY')})")
    if options.get("GIT_TAG") != [sha]:
        problems.append(f"{source}: the vellum_gpu_runtime FetchContent GIT_TAG must be the lock's "
                        f"vellum.sha {sha} (found {options.get('GIT_TAG')})")
    if unexpected:
        problems.append(f"{source}: the vellum_gpu_runtime FetchContent declaration carries "
                        f"{', '.join(unexpected)}, which could take its source from somewhere "
                        "other than the pinned commit; only "
                        f"{', '.join(sorted(GPU_RUNTIME_DECLARE_OPTIONS | GPU_RUNTIME_DECLARE_FLAGS))} "
                        "are allowed")
    return problems


def _source_mode_problems(root: Path, source: str, run: GpuRuntimeRun, sha: str,
                          lock_path: Path, label: str) -> list[str]:
    """One declaration of the pinned commit, made available with no source override."""
    problems = []
    declares = [(i, args) for i, args in run.named("FetchContent_Declare")
                if args and args[0].lower() == "vellum_gpu_runtime"]
    if len(declares) != 1:
        return [f"{source} ({label}) must declare FetchContent vellum_gpu_runtime exactly once "
                f"(found {len(declares)})"]
    at, args = declares[0]
    problems += [f"{problem} ({label})" for problem in _declaration_problems(source, args, sha)]
    made = [i for i, args in run.named("FetchContent_MakeAvailable")
            if i > at and "vellum_gpu_runtime" in args]
    if not made:
        problems.append(f"{source} ({label}) never makes vellum_gpu_runtime available after "
                        "declaring it")
    for _, args in run.named("FetchContentOverrides"):
        if args[0].endswith("_VELLUM_GPU_RUNTIME") and any(args[1:]):
            problems.append(f"{source} ({label}): FetchContent takes vellum_gpu_runtime from "
                            f"{args[1] or 'the local tree (FETCHCONTENT_FULLY_DISCONNECTED)'}, "
                            "not the pinned commit")
    for _, args in run.named("lock"):
        if not args or Path(args[0]).resolve() != lock_path.resolve():
            problems.append(f"{source} ({label}) read the pin from {args[:1]}, not {lock_path}")
    if not run.named("lock"):
        problems.append(f"{source} ({label}) never read a lock through PulpVellumLock.cmake")
    return problems


def gpu_runtime_cmake_problems(root: Path, source: str) -> list[str]:
    """Assert, by running it with real CMake, that `source` builds gpu-runtime from pulp.lock.

    tools/deps/gpu_runtime_driver.cmake evaluates `source` with `cmake -P`,
    recording the FetchContent, find_package, version-read, include and
    FATAL_ERROR calls it makes. The audit asserts on those calls, so
    comments, quoting, escapes, dead branches and return() behave as CMake
    defines them. Evaluated against the repository's pulp.lock and against a
    second probe lock, it requires:

    - exactly one vellum_gpu_runtime declaration, with GIT_REPOSITORY the
      canonical Vellum repository, GIT_TAG the lock's vellum.sha (under both
      locks), and no option that takes the source from elsewhere;
    - the lock read through PulpVellumLock.cmake from the repository's
      pulp.lock, and no include the driver cannot evaluate;
    - configure stopping with FATAL_ERROR when, and only when, the fetched
      project's version differs from the lock's tag;
    - the prefix package found by the lock's version (EXACT), after the
      record and archive checks ran with the lock's sha and archive digest.

    A file that replaces a recorded command (function(), macro() or
    cmake_language(EVAL)) leaves the recorder unrun, so the record it must
    produce is missing and the audit fails. gpu_runtime_tree_problems covers
    what the rest of the tree could do before this file runs.
    """
    problems: list[str] = []
    cmake = find_cmake()
    if cmake is None:
        return [f"cmake was not found (set {CMAKE_ENV}); {source} is checked by evaluating it "
                "with cmake -P, so the gpu-runtime pin cannot be verified"]
    try:
        lock = load_pulp_lock(root / "pulp.lock")
        sha = lock["vellum"]["sha"]
        version = str(lock["vellum"]["tag"]).removeprefix("v")
        digest = lock["vellum"]["gpu_runtime_digest"]
    except (ValueError, KeyError, TypeError) as error:
        return [f"cannot read pulp.lock to check {source}: {error}"]
    if not (root / source).is_file():
        return [f"{source} is missing"]

    with tempfile.TemporaryDirectory() as work:
        probe_lock = Path(work) / "probe.lock"
        probe_lock.write_text(json.dumps(GPU_RUNTIME_PROBE_LOCK), encoding="utf-8")
        probe_sha = GPU_RUNTIME_PROBE_LOCK["vellum"]["sha"]
        probe_version = GPU_RUNTIME_PROBE_LOCK["vellum"]["tag"].removeprefix("v")
        probe_digest = GPU_RUNTIME_PROBE_LOCK["vellum"]["gpu_runtime_digest"]
        runs = {
            "source": run_gpu_runtime_driver(root, source, version, cmake=cmake),
            "fetched version above": run_gpu_runtime_driver(root, source, version + ".99",
                                                             cmake=cmake),
            "fetched version below": run_gpu_runtime_driver(root, source, "0.0.0", cmake=cmake),
            "probe lock": run_gpu_runtime_driver(root, source, probe_version, cmake=cmake,
                                                 lock_file=probe_lock),
            "prefix": run_gpu_runtime_driver(root, source, version, prefix=True, cmake=cmake),
            "prefix, probe lock": run_gpu_runtime_driver(root, source, probe_version, prefix=True,
                                                         cmake=cmake, lock_file=probe_lock),
        }
        for label, run in runs.items():
            if not run.stopped:
                tail = "\n".join(run.output.strip().splitlines()[-6:])
                problems.append(f"evaluating {source} with cmake -P ({label}) failed before a "
                                f"FATAL_ERROR or the end of the file:\n{tail}")
            for _, args in run.named("include"):
                target = args[0] if len(args) == 1 else None
                passed = target is not None and Path(target).is_absolute() and Path(target).resolve() in (
                    (root / GPU_RUNTIME_LOCK_READER).resolve(), (root / GPU_RUNTIME_RECORD_MODULE).resolve())
                if not passed and args not in ([name] for name in GPU_RUNTIME_ALLOWED_INCLUDES):
                    problems.append(f"{source} ({label}) includes {args}, which the audit cannot "
                                    "evaluate; it may include only PulpVellumLock.cmake, "
                                    "PulpGpuRuntimeRecord.cmake and FetchContent")
        if problems:
            return problems

        problems += _source_mode_problems(root, source, runs["source"], sha, root / "pulp.lock",
                                          "source")
        problems += _source_mode_problems(root, source, runs["probe lock"], probe_sha, probe_lock,
                                          "probe lock")

        # The version check. These runs differ from the source run only in
        # the fetched project's version. Each must stop with FATAL_ERROR
        # straight after reading that version, and the source run must have
        # executed exactly the same pin-file commands up to that message() and
        # then done something else: the stop depends on the version and
        # nothing else.
        same = runs["source"]
        for label in ("fetched version above", "fetched version below"):
            wrong = runs[label]
            reads = [i for i, args in wrong.named("get_directory_property")
                     if args[1:] == ["DIRECTORY", GPU_RUNTIME_FETCHED_DIR, "DEFINITION",
                                     "PROJECT_VERSION"]]
            checked = False
            if reads and wrong.trace:
                at, calls, steps = reads[-1], wrong.calls, wrong.trace
                checked = (len(calls) == at + 2 and calls[at + 1][0] == "message"
                           and same.calls[:at + 1] == calls[:at + 1]
                           and steps[-1][1] == "message"
                           and same.trace[:len(steps) - 1] == steps[:-1]
                           and len(same.trace) >= len(steps)
                           and same.trace[len(steps) - 1] != steps[-1])
            if not checked:
                problems.append(f"{source} does not stop configure with FATAL_ERROR when, and "
                                "only when, the fetched gpu-runtime's PROJECT_VERSION differs "
                                f"from the lock's tag ({label})")

        for label, run, want_sha, want_version, want_digest in (
                ("prefix", runs["prefix"], sha, version, digest),
                ("prefix, probe lock", runs["prefix, probe lock"], probe_sha, probe_version,
                 probe_digest)):
            finds = run.named("find_package")
            if run.named("FetchContent_Declare"):
                problems.append(f"{source} ({label}) declares a FetchContent source although "
                                "PULP_GPU_RUNTIME_PREFIX is set")
            if (len(finds) != 1 or finds[0][1][:2] != ["VellumGpuRuntime", want_version]
                    or "EXACT" not in finds[0][1]):
                problems.append(f"{source} ({label}) must find the prefix package as "
                                f"find_package(VellumGpuRuntime {want_version} EXACT ...) "
                                f"(found {[args for _, args in finds]})")
                continue
            before = finds[0][0]
            records = [args for i, args in run.named("pulp_check_gpu_runtime_record") if i < before]
            archives = [args for i, args in run.named("pulp_check_gpu_runtime_archive") if i < before]
            if [args[1:2] for args in records] != [[want_sha]]:
                problems.append(f"{source} ({label}) must check the prefix record against the "
                                f"lock's sha {want_sha} before finding it (found {records})")
            if [args[2:3] for args in archives] != [[want_digest]]:
                problems.append(f"{source} ({label}) must check the prefix archive against the "
                                f"lock's gpu_runtime_digest before finding it (found {archives})")
    return problems


def _named_writes(command: cmake_commands.Command, names: tuple[str, ...]) -> list[str]:
    """The `names` that `command` writes: arguments naming them, in any quoting.

    set("NAME" v) and set([[NAME]] v) write NAME just as set(NAME v) does.
    """
    if command.name in CMAKE_READ_ONLY_COMMANDS:
        return []
    upper = {name.upper(): name for name in names}
    return [upper[argument.value.upper()] for argument in command.arguments
            if argument.value.upper() in upper]


CMAKE_EXPANSION = re.compile(r"\$\{[^}]*\}|\$ENV\{[^}]*\}|@[A-Za-z_][A-Za-z0-9_]*@|\$<[^>]*>")
CMAKE_VARIABLE_REFERENCE = re.compile(r"\$\{([A-Za-z0-9_]+)\}")
# Commands whose first argument is the variable they write.
CMAKE_DESTINATION_COMMANDS = {"set", "unset", "option"}
CMAKE_SET_KEYWORDS = {"CACHE", "PARENT_SCOPE", "FORCE", "INTERNAL", "BOOL", "FILEPATH", "PATH",
                      "STRING"}


def _expand_known(value: str, known: dict[str, set[str]]) -> set[str]:
    """Every text `value` can have, substituting the literal variables seen so far."""
    results = {""}
    position = 0
    for match in CMAKE_VARIABLE_REFERENCE.finditer(value):
        options = known.get(match.group(1).upper()) or {match.group(0)}
        results = {prefix + value[position:match.start()] + option
                   for prefix in results for option in sorted(options)[:8]}
        position = match.end()
        if len(results) > 64:
            return {value}
    return {prefix + value[position:] for prefix in results}


def _record_literal(command: cmake_commands.Command, known: dict[str, set[str]]) -> None:
    """Remember the literal text set(NAME text) and string(CONCAT NAME ...) give a variable."""
    values = command.values()
    if command.name == "set" and len(values) >= 2:
        parts = [value for value in values[1:] if value not in CMAKE_SET_KEYWORDS]
        if len(parts) == 1 and "PARENT_SCOPE" not in values and "CACHE" not in values:
            known.setdefault(values[0].upper(), set()).update(_expand_known(parts[0], known))
    elif command.name == "string" and len(values) >= 3 and values[0].upper() == "CONCAT":
        text = {""}
        for part in values[2:]:
            text = {prefix + option for prefix in text for option in _expand_known(part, known)}
        known.setdefault(values[1].upper(), set()).update(sorted(text)[:8])


def _expanded_destination_writes(command: cmake_commands.Command, names: tuple[str, ...],
                                 known: dict[str, set[str]]) -> list[str]:
    """The `names` an expanded destination could be: set(${n} v), set(PULP_${n} v).

    A destination is resolved through the literal set()/string(CONCAT) values seen
    earlier in the same file; what stays unresolved is treated as every name its
    literal text allows, so set(PULP_VELLUM_${s} v) fails closed. A destination
    that is only an unresolved expansion is the out-parameter idiom
    (set(${out} v)) the tree uses everywhere, and is not flagged: the name it
    receives must come from a literal argument or a resolved expansion elsewhere,
    which is flagged. A write with PARENT_SCOPE targets the caller's variable
    and is skipped for the same reason.
    """
    if (command.name not in CMAKE_DESTINATION_COMMANDS or not command.arguments
            or "PARENT_SCOPE" in command.values()):
        return []
    if not CMAKE_EXPANSION.search(command.arguments[0].value):
        return []
    found: list[str] = []
    for destination in _expand_known(command.arguments[0].value, known):
        if not CMAKE_EXPANSION.search(destination):
            found += [name for name in names if name.upper() == destination.upper()]
            continue
        parts = CMAKE_EXPANSION.split(destination)
        if not any(parts):
            continue
        pattern = ".*".join(re.escape(part) for part in parts)
        found += [name for name in names if re.fullmatch(pattern, name, re.IGNORECASE)]
    return sorted(set(found))


GPU_RUNTIME_SOURCE_HELPER_FILE = "tools/cmake/PulpFetchContent.cmake"
CMAKE_EVAL_DEPTH = 4


def _unescape_cmake(text: str) -> str:
    """The text a quoted CMake argument holds: \\n, \\t and \\r become characters."""
    escapes = {"n": "\n", "t": "\t", "r": "\r"}
    return re.sub(r"\\(.)", lambda m: escapes.get(m.group(1), m.group(1)), text, flags=re.DOTALL)


def _scan_tree_commands(rel: str, commands: list[cmake_commands.Command],
                        protected: tuple[str, ...], known: dict[str, set[str]],
                        problems: list[str], depth: int) -> None:
    """Flag what `commands` could do to the gpu-runtime fetch.

    cmake_language(EVAL CODE ...) is evaluated: its code text is tokenized and
    scanned as commands too, so the text is not an opaque string. A CALL is
    scanned as the command it calls. Code that cannot be read fails closed.
    """
    for command in commands:
        if rel != GPU_RUNTIME_SOURCE_HELPER_FILE:
            # The helper file builds FETCHCONTENT_SOURCE_DIR_<name> for any dependency;
            # a call registering vellum_gpu_runtime through it is flagged below.
            expanded_names = _expanded_destination_writes(command, protected, known)
        else:
            expanded_names = [name for name in _expanded_destination_writes(command, protected, known)
                              if name not in GPU_RUNTIME_SOURCE_OVERRIDE_VARS]
        for name in expanded_names:
            problems.append(f"{rel}:{command.line} sets a variable named by an expansion that "
                            f"can be {name}, with {command.name}(), which can make the "
                            "gpu-runtime fetch use another source or lock")
        _record_literal(command, known)
        for name in _named_writes(command, protected):
            problems.append(f"{rel}:{command.line} sets {name} with {command.name}(), which "
                            "can make the gpu-runtime fetch use another source or lock")
        if command.name == "cmake_language" and command.arguments:
            mode = command.arguments[0].value.upper()
            values = command.values()
            if mode == "EVAL":
                if len(values) < 3 or values[1].upper() != "CODE" or depth >= CMAKE_EVAL_DEPTH:
                    problems.append(f"{rel}:{command.line} cmake_language(EVAL) is not in the "
                                    "form EVAL CODE <text> the audit can read")
                else:
                    try:
                        embedded = cmake_commands.tokenize(_unescape_cmake("".join(values[2:])))
                    except cmake_commands.CMakeSyntaxError as error:
                        problems.append(f"{rel}:{command.line} cmake_language(EVAL) runs code the "
                                        f"audit cannot parse as CMake: {error}")
                    else:
                        _scan_tree_commands(f"{rel}:{command.line} EVAL", embedded, protected,
                                            known, problems, depth + 1)
            elif mode == "CALL":
                if len(values) < 2:
                    problems.append(f"{rel}:{command.line} cmake_language(CALL) names no command")
                else:
                    called = cmake_commands.Command(values[1].lower(), command.arguments[2:],
                                                    command.line)
                    if not CMAKE_EXPANSION.search(values[1]):
                        _scan_tree_commands(f"{rel} CALL", [called], protected, known,
                                            problems, depth + 1)
                    else:
                        problems.append(f"{rel}:{command.line} cmake_language(CALL) names its "
                                        f"command by an expansion, {values[1]}, so the audit "
                                        "cannot tell what it runs")
            elif "SET_DEPENDENCY_PROVIDER" in values:
                problems.append(f"{rel}:{command.line} sets a dependency provider, which can "
                                "replace the pinned gpu-runtime fetch")
        if (command.name in GPU_RUNTIME_SOURCE_HELPERS and command.arguments
                and command.arguments[0].value.lower() == "vellum_gpu_runtime"):
            problems.append(f"{rel}:{command.line} registers a FetchContent source for "
                            f"vellum_gpu_runtime with {command.name}(), so it may not build "
                            "from the pinned commit")
        if command.name in ("function", "macro") and command.arguments:
            defined = command.arguments[0].value
            if defined.lower() in GPU_RUNTIME_RELIED_COMMANDS or (
                    CMAKE_EXPANSION.search(defined) and any(CMAKE_EXPANSION.split(defined))
                    and any(re.fullmatch(".*".join(re.escape(part) for part in
                                                   CMAKE_EXPANSION.split(defined)), name, re.IGNORECASE)
                            for name in GPU_RUNTIME_RELIED_COMMANDS)):
                problems.append(f"{rel}:{command.line} redefines {defined}() "
                                "with " + command.name + "(), which the gpu-runtime pin relies on")


def gpu_runtime_tree_problems(root: Path) -> list[str]:
    """What the rest of the tree could do to the gpu-runtime fetch before the pin file runs.

    Scans every command (tools/deps/cmake_commands.py; comments and the text
    of quoted or bracket arguments are not commands) in tools/cmake/*.cmake
    and the root CMakeLists.txt for: a FetchContent source override for
    vellum_gpu_runtime, a write of PULP_VELLUM_LOCK_FILE outside the lock
    reader, a source registered for vellum_gpu_runtime through Pulp's
    FetchContent helpers, a dependency provider, and a function() or macro()
    replacing a command the pin evaluation relies on.
    """
    problems: list[str] = []
    candidates = sorted((root / "tools/cmake").glob("*.cmake"))
    if (root / "CMakeLists.txt").is_file():
        candidates.append(root / "CMakeLists.txt")
    for path in candidates:
        rel = path.relative_to(root).as_posix()
        try:
            commands = cmake_commands.tokenize(path.read_text(encoding="utf-8"))
        except OSError as error:
            problems.append(f"cannot read {rel}: {error}")
            continue
        except cmake_commands.CMakeSyntaxError as error:
            problems.append(f"cannot parse {rel} as CMake, so its effect on the gpu-runtime fetch "
                            f"cannot be checked: {error}")
            continue
        protected = GPU_RUNTIME_SOURCE_OVERRIDE_VARS
        if rel != GPU_RUNTIME_LOCK_READER:
            protected = (*protected, GPU_RUNTIME_LOCK_PATH_VAR)
        _scan_tree_commands(rel, commands, protected, {}, problems, depth=0)
    return problems


def gpu_runtime_pin_agreement(manifest: list[dict], root: Path = ROOT) -> list[str]:
    """The build must fetch and version-check gpu-runtime from pulp.lock.

    tools/cmake/PulpGpuRuntime.cmake is what configure reads and the manifest
    is what the audit and DEPENDENCIES.md read; pulp_lock_agreement ties the
    manifest to pulp.lock. This asserts the CMake side of that chain by
    evaluating the CMake, so an edit that fetches an unpinned commit or skips
    the lock-derived version check fails here.
    """
    entry = next((dep for dep in manifest if dep.get("name") == GPU_RUNTIME_ENTRY), None)
    if entry is None:
        # Nothing to agree with is only fine while nothing is fetched.
        try:
            commands = cmake_commands.tokenize(
                (root / GPU_RUNTIME_PIN_FILE).read_text(encoding="utf-8"))
        except OSError:
            return []
        except cmake_commands.CMakeSyntaxError as error:
            return [f"cannot parse {GPU_RUNTIME_PIN_FILE} as CMake: {error}"]
        if any(command.name == "fetchcontent_declare" and command.arguments
               and command.arguments[0].value.lower() == "vellum_gpu_runtime"
               for command in commands):
            return [f"{GPU_RUNTIME_PIN_FILE} fetches gpu-runtime, but the manifest has no "
                    f"{GPU_RUNTIME_ENTRY} entry"]
        return []
    sources = entry.get("source_files") or []
    if len(sources) != 1:
        return [f"{GPU_RUNTIME_ENTRY} must name exactly one source file, the CMake pin"]
    if sources[0] != GPU_RUNTIME_PIN_FILE:
        return [f"{GPU_RUNTIME_ENTRY} source_files must be exactly {GPU_RUNTIME_PIN_FILE}, "
                f"not {sources[0]!r}: the audit evaluates only that file"]
    return [*gpu_runtime_cmake_problems(root, sources[0]), *gpu_runtime_tree_problems(root)]



def renderer_lock_digest_problems(lock: dict, manifest: list[dict], read_bytes=None) -> list[str]:
    """pulp.lock's render_toolchain.lock_digest is sha256 of gpu-runtime's renderer lock.

    The digest is taken over the exact bytes of the file the Skia entry's
    vellum_renderer_drift_check names (gpu-runtime/renderer-lock.json), read at
    pulp.lock's vellum.sha from raw.githubusercontent.com or, when
    PULP_VELLUM_CHECKOUT is set, with `git show` in that checkout. An
    unreadable lock fails closed, as the drift check does.
    """
    digest = (lock.get("render_toolchain") or {}).get("lock_digest")
    sha = (lock.get("vellum") or {}).get("sha")
    if not isinstance(digest, str) or not isinstance(sha, str) or not FULL_SHA_RE.fullmatch(sha):
        return []  # pulp_lock.validate already reports the malformed field
    skia = next((dep for dep in manifest if dep.get("name") == "Skia"), None)
    spec = (skia or {}).get(VELLUM_DRIFT_FIELD)
    if not isinstance(spec, dict):
        return [f"render_toolchain.lock_digest cannot be checked: the Skia entry has no "
                f"{VELLUM_DRIFT_FIELD} naming gpu-runtime's renderer lock"]
    repository, path, commit = spec.get("repository"), spec.get("path"), spec.get("commit")
    if (not isinstance(repository, str) or not isinstance(path, str)
            or not path.endswith(".json") or path.startswith("/") or ".." in Path(path).parts):
        return [f"render_toolchain.lock_digest cannot be checked: Skia {VELLUM_DRIFT_FIELD} "
                f"does not name a repository-relative renderer lock (path {path!r})"]
    problems = []
    if not isinstance(commit, str) or commit.lower() != sha:
        problems.append(f"Skia {VELLUM_DRIFT_FIELD} reads the renderer lock at {commit}, but "
                        f"pulp.lock pins Vellum {sha}; they must be the same commit")
    try:
        data = (read_bytes or read_vellum_pin_bytes)(repository, sha, path)
    except VellumPinUnavailable as error:
        return [*problems, f"render_toolchain.lock_digest cannot be checked: {error}"]
    actual = hashlib.sha256(data).hexdigest()
    if actual != digest:
        problems.append(f"render_toolchain.lock_digest is {digest}, but {path} at Vellum {sha} "
                        f"hashes to {actual}")
    return problems


def pulp_lock_agreement(manifest: list[dict], root: Path = ROOT, read_bytes=None) -> list[str]:
    """Validate pulp.lock, bind its renderer digest, and derive the legacy gpu-runtime pin from it."""
    try:
        lock = load_pulp_lock(root / "pulp.lock")
    except ValueError as exc:
        return [str(exc)]
    problems = validate_pulp_lock(lock)
    vellum = lock.get("vellum", {})
    entry = next((dep for dep in manifest if dep.get("name") == GPU_RUNTIME_ENTRY), None)
    if entry is None:
        problems.append(f"manifest has no {GPU_RUNTIME_ENTRY} entry required by pulp.lock")
        return problems
    pinned = ((entry.get("upstream") or {}).get("commit") or "").lower()
    if pinned != vellum.get("sha"):
        problems.append(f"manifest {GPU_RUNTIME_ENTRY} pins {pinned or 'nothing'}, but pulp.lock pins {vellum.get('sha')}")
    version = str(entry.get("version", "")).split(" ", 1)[0]
    if version != str(vellum.get("tag", "")).removeprefix("v"):
        problems.append(f"manifest {GPU_RUNTIME_ENTRY} version is {version or 'missing'}, but pulp.lock pins {vellum.get('tag')}")
    problems.extend(renderer_lock_digest_problems(lock, manifest, read_bytes))
    return problems


def run_git_ls_remote(repo: str, *refs: str, options: tuple[str, ...] = ()) -> str:
    # Options must precede the repository; git treats anything AFTER it as a ref
    # pattern. `git ls-remote <repo> --tags --refs` therefore matches no refs and
    # exits 0 with empty output — which this function cannot distinguish from
    # "the repo has no such ref", so the latest-version lookup silently returned
    # None for every dependency and the audit reported a bare "present" forever.
    cmd = ["git", "ls-remote", *options, repo, *refs]
    try:
        result = subprocess.run(
            cmd,
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        )
        return result.stdout.strip()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return ""


# Two-component releases are real: pugixml ships `v1.14`, and requiring a patch
# component dropped it from the candidate set entirely — so the audit reported
# the older three-component `v1.12.1` as "latest", a downgrade presented as
# drift. Patch defaults to 0 when absent.
SEMVER_RE = re.compile(r"(\d+)\.(\d+)(?:\.(\d+))?")

# A pre-release is not a newer release. These suffixes parse to the same semver
# key as the final version, so `v1.2.0rc2` tied with and then beat `v1.2.0`, and
# a Gradle milestone outranked a stable line. Anything carrying one of these is
# excluded rather than ranked.
PRERELEASE_RE = re.compile(r"(?i)(rc|alpha|beta|dev|pre|snapshot|-m\d+\b)")


def semver_key(value: str):
    match = SEMVER_RE.search(value)
    if not match:
        return None
    # Patch is optional; absent means 0 so `v1.14` ranks as (1, 14, 0) and
    # compares correctly against `v1.12.1`.
    return tuple(int(part) if part is not None else 0 for part in match.groups())


class ReleaseLookupUnavailable(RuntimeError):
    """The releases API could not be reached, as distinct from having no releases.

    Kept separate so a rate limit is never silently rendered as "no newer
    release". An unauthenticated run hits the limit within a handful of
    dependencies, and a report that quietly degraded to tag ordering at that
    point would be the same silent all-clear this module already produced once.
    """


GITHUB_REPO_RE = re.compile(r"github\.com[:/]+([^/]+)/([^/]+?)(?:\.git)?/?$")


@functools.lru_cache(maxsize=1)
def github_token() -> str | None:
    """A token for api.github.com, or None if the host offers none.

    Unauthenticated GitHub allows 60 requests/hour per IP, which this audit
    exhausts before it reaches its second dependency, so a token is effectively
    required rather than an optimization. `ghapp` is preferred over `gh`: it
    authenticates as the Shipyard App with its own quota instead of spending the
    human's shared personal-token budget.
    """
    for variable in ("GH_TOKEN", "GITHUB_TOKEN"):
        value = os.environ.get(variable)
        if value:
            return value
    for command in (["ghapp", "auth", "token"], ["gh", "auth", "token"]):
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=30, check=False)
        except (OSError, subprocess.SubprocessError):
            continue
        token = result.stdout.strip()
        if result.returncode == 0 and token:
            return token
    return None


def github_latest_release(repo: str) -> str | None:
    """The tag GitHub itself calls latest, or None when that cannot be answered.

    Tag ordering is a heuristic and it is wrong in ways that are hard to
    enumerate: Mbed TLS carries `mbedos-16.03-release` tags that outrank every
    real `mbedtls-4.x`, Yoga carries date-shaped tags, and repositories mix
    prefixes across release lines. Where a project publishes releases, that is
    the authoritative answer and no amount of tag parsing beats asking.

    Returns None on any failure — unauthenticated rate limits, a repository with
    no releases, a non-GitHub host. The caller falls back to tags, and a None
    here must never be reported as "no newer version": the tag path says that,
    and it says it from evidence.
    """
    match = GITHUB_REPO_RE.search(repo)
    if not match:
        return None
    url = f"https://api.github.com/repos/{match.group(1)}/{match.group(2)}/releases/latest"
    request = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "pulp-deps-audit",
    })
    token = github_token()
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as error:
        # 404 is a real answer: this project publishes tags but not releases, so
        # the tag heuristic is the right source and needs no caveat. Anything
        # else — rate limiting above all — means we did not get to ask, and the
        # caller must not present the fallback as though we had.
        if error.code == 404:
            return None
        raise ReleaseLookupUnavailable(str(error.code)) from None
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as error:
        raise ReleaseLookupUnavailable(type(error).__name__) from None
    tag = payload.get("tag_name")
    return tag if isinstance(tag, str) and tag else None


def latest_semver_tag_from_refs(refs: Iterable[str], tag_pattern: str | None = None) -> str | None:
    """Pick the highest real release from `refs/tags/...` names.

    Split out from the network call so the selection rules — which are where
    every past misreport came from — can be tested against a literal ref list.

    `tag_pattern` (the upstream's own release-tag regex) restricts the scan to
    tags that fully match it. A repository that also tags sub-crates (jdx/mise
    tags `mise-sigstore-v0.1.0` beside `v2026.10.12`) otherwise wins with the
    sub-crate tag. A project whose own pattern is date-shaped declares that
    shape, so the year-major filter below does not apply to it.
    """
    pattern = re.compile(tag_pattern) if tag_pattern else None
    candidates: list[tuple[tuple[int, int, int], str]] = []
    for ref in refs:
        if not ref:
            continue
        tag = ref.removeprefix("refs/tags/")
        if pattern is not None and not pattern.fullmatch(tag):
            continue
        key = semver_key(tag)
        if key is None:
            continue
        # Date-shaped tags parse as perfectly good semver and then dominate the
        # ordering forever: Yoga's `v2017.02.07.00` reads as (2017, 2, 7), which
        # outranks its real latest release `v3.2.1`. Reporting that as "latest"
        # is worse than reporting nothing, because it is a drift signal someone
        # might act on. No project ships a major version in the thousands, so a
        # year-shaped major is the honest discriminator.
        if pattern is None and key[0] >= 1000:
            continue
        if PRERELEASE_RE.search(tag):
            continue
        candidates.append((key, tag))
    if not candidates:
        return None
    candidates.sort()
    return candidates[-1][1]


def latest_semver_tag(repo: str, tag_pattern: str | None = None) -> str | None:
    output = run_git_ls_remote(repo, options=("--tags", "--refs"))
    return latest_semver_tag_from_refs(
        (line.split("\t", 1)[1] for line in output.splitlines() if "\t" in line),
        tag_pattern,
    )


def upstream_status(dep: dict) -> str:
    kind = dep["upstream"]["kind"]
    repo = dep["repository"]
    if kind == "none":
        return "manual"
    if kind == "git-head":
        output = run_git_ls_remote(repo, "HEAD")
        return output.split()[0][:12] if output else "missing"
    if kind == "git-branch":
        ref = dep["upstream"]["ref"]
        output = run_git_ls_remote(repo, f"refs/heads/{ref}")
        sha = output.split()[0][:12] if output else "missing"
        return f"{ref} @ {sha}"
    if kind == "git-tag":
        ref = dep["upstream"]["ref"]
        output = run_git_ls_remote(repo, f"refs/tags/{ref}")
        exact = "present" if output else "missing"
        tag_pattern = dep["upstream"].get("tag_pattern")
        # A malformed pattern is this row's defect, not a reason to abort the
        # audit of every other dependency.
        if tag_pattern:
            try:
                re.compile(tag_pattern)
            except re.error as invalid:
                return f"{exact}; invalid tag_pattern: {invalid}"
        # Authoritative first, heuristic second — and say which one answered.
        # A release outside the project's own tag pattern (a sub-crate's) is
        # not this dependency's latest, so the filtered tags answer instead.
        try:
            released = github_latest_release(repo)
            caveat = ""
            if released and tag_pattern and not re.fullmatch(tag_pattern, released):
                caveat = f" (tag-order guess; releases API answer {released} is outside tag_pattern)"
                released = None
            latest = released or latest_semver_tag(repo, tag_pattern)
        except ReleaseLookupUnavailable as unavailable:
            latest = latest_semver_tag(repo, tag_pattern)
            caveat = f" (tag-order guess; releases API unavailable: {unavailable})"
        if latest and latest != ref:
            return f"{exact}; latest={latest}{caveat}"
        return exact
    if kind == "git-commit":
        ref = dep["upstream"]["ref"].lower()
        output = run_git_ls_remote(repo)
        for line in output.splitlines():
            sha = line.split(maxsplit=1)[0].lower()
            if sha.startswith(ref) or ref.startswith(sha):
                return "present"
        return "missing"
    return "unknown"


def render_markdown(
    rows: list[dict],
    missing_deps: list[str],
    missing_notice: list[str],
    missing_licensing: list[str],
    uncovered: list[DeclaredDep],
) -> str:
    lines = [
        "# Dependency Audit",
        "",
        "| Name | Version | License | Source | Upstream | DEPENDENCIES.md | NOTICE.md | licensing.md |",
        "|------|---------|---------|--------|----------|------------------|-----------|--------------|",
    ]
    for row in rows:
        lines.append(
            f"| {row['name']} | {row['version']} | {row['license']} | {row['source_kind']} | "
            f"{row['upstream']} | {row['dependencies_md']} | {row['notice_md']} | {row['licensing_md']} |"
        )
    if missing_deps:
        lines.extend(["", "## Missing from DEPENDENCIES.md", ""])
        lines.extend(f"- {name}" for name in missing_deps)
    if missing_notice:
        lines.extend(["", "## Missing from NOTICE.md", ""])
        lines.extend(f"- {name}" for name in missing_notice)
    if missing_licensing:
        lines.extend(["", "## Missing from docs/reference/licensing.md", ""])
        lines.extend(f"- {name}" for name in missing_licensing)
    if uncovered:
        lines.extend(["", "## Declared but missing from manifest.json", ""])
        for dep in uncovered:
            where = f" ({dep.location})" if dep.location else ""
            lines.append(f"- `{dep.name}` from {dep.source}{where}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit dependency inventory and drift")
    parser.add_argument("--check-upstream", action="store_true", help="Query upstream repos")
    parser.add_argument("--format", choices=["text", "markdown"], default="text")
    parser.add_argument("--strict", action="store_true", help="Fail if docs/notices are incomplete")
    parser.add_argument(
        "--verify-licenses",
        action="store_true",
        help="Check attribution text against the licenses actually on disk",
    )
    parser.add_argument(
        "--require-offline-trees",
        action="store_true",
        help="Fail when an offline_fetch dependency's tree is not available to check "
             "(for hosts whose configure must have populated it: the gate and the m3 lane)",
    )
    parser.add_argument(
        "--offline-fetch-only",
        action="store_true",
        help="Run only the offline-fetch contract (implies reading checked-out trees)",
    )
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="Configured build directory whose <name>_SOURCE_DIR names the tree")
    args = parser.parse_args()
    global BUILD_DIR
    BUILD_DIR = args.build_dir
    if args.offline_fetch_only:
        return offline_fetch_main(load_manifest(), args.require_offline_trees)

    manifest = load_manifest()
    deps_md_names = parse_dependencies_md()
    notice_names = parse_notice_md()
    licensing_names = parse_licensing_md()

    rows = []
    missing_deps: list[str] = []
    missing_notice: list[str] = []
    missing_licensing: list[str] = []

    for dep in manifest:
        in_deps = dep["name"] in deps_md_names
        in_notice = dep["name"] in notice_names
        # licensing.md uses the presentation name inside bold markers.
        # Accept either the manifest name directly or a loose match without
        # trailing " SDK" (e.g. "VST3 SDK" is listed as "VST3 SDK" already).
        in_licensing = dep["name"] in licensing_names or (
            dep["name"].replace("-", " ") in licensing_names
        )
        if dep["documented_in_dependencies_md"] and not in_deps:
            missing_deps.append(dep["name"])
        if dep["documented_in_notice_md"] and not in_notice:
            missing_notice.append(dep["name"])
        # AAX/ASIO and other developer-supplied SDKs are exempt from the
        # public licensing.md table (they live in the "Optional Vendor SDK"
        # section instead), so gate on documented_in_notice_md which already
        # marks them false.
        if dep["documented_in_notice_md"] and not in_licensing:
            missing_licensing.append(dep["name"])
        rows.append({
            "name": dep["name"],
            "version": dep["version"],
            "license": dep["license"],
            "source_kind": dep["source_kind"],
            "upstream": upstream_status(dep) if args.check_upstream else "skipped",
            "dependencies_md": "yes" if in_deps else "no",
            "notice_md": "yes" if in_notice else "no",
            "licensing_md": "yes" if in_licensing else "no",
        })

    # Completeness check — any dep declared in a real manifest source
    # (pip requirements, mkdocs.yml, FetchContent_Declare, external/, or a
    # recognized redistributed bootstrap binary) must be in manifest.json.
    declared = collect_declared()
    uncovered, known_unlisted, stale_known = split_known_unlisted(
        find_uncovered_declarations(manifest, declared))

    truncated_notices: list[tuple[str, list[str]]] = []
    license_problems: list[tuple[str, list[str]]] = []
    unverified: list[str] = []
    offline_problems: list[tuple[str, list[str]]] = []
    offline_unverified: list[str] = []
    offline_verified: list[str] = []
    if args.verify_licenses:
        truncated_notices = find_notice_truncations()
        for dep in manifest:
            status, problems = verify_dep_license(dep)
            if status == "unverified":
                unverified.append(dep["name"])
            if problems:
                license_problems.append((dep["name"], problems))
            # Same mode: both read the checked-out trees and nothing else.
            status, problems = verify_offline_fetch(dep)
            if status == "unverified":
                offline_unverified.append(dep["name"])
            elif status == "verified":
                offline_verified.append(dep["name"])
            if problems:
                offline_problems.append((dep["name"], problems))

    if args.format == "markdown":
        output = render_markdown(
            rows, missing_deps, missing_notice, missing_licensing, uncovered,
        )
        sys.stdout.write(output)
    else:
        for row in rows:
            print(
                f"{row['name']}: version={row['version']} license={row['license']} "
                f"source={row['source_kind']} upstream={row['upstream']} "
                f"DEPENDENCIES.md={row['dependencies_md']} NOTICE.md={row['notice_md']} "
                f"licensing.md={row['licensing_md']}"
            )
        if missing_deps:
            print("\nMissing from DEPENDENCIES.md:")
            for name in missing_deps:
                print(f"  - {name}")
        if missing_notice:
            print("\nMissing from NOTICE.md:")
            for name in missing_notice:
                print(f"  - {name}")
        if missing_licensing:
            print("\nMissing from docs/reference/licensing.md:")
            for name in missing_licensing:
                print(f"  - {name}")
        if uncovered:
            print("\nDeclared but missing from manifest.json:")
            for dep in uncovered:
                where = f" ({dep.location})" if dep.location else ""
                url = f" <{dep.url}>" if dep.url else ""
                print(f"  - {dep.name} from {dep.source}{where}{url}")
        if known_unlisted:
            print("\nKnown unlisted fetches (follow-ups; see KNOWN_UNLISTED_FETCHES):")
            for dep in known_unlisted:
                print(f"  - {dep.source} ({dep.location}) <{dep.url}>")
        if stale_known:
            print("\nKNOWN_UNLISTED_FETCHES entries whose fetch no longer exists:")
            for source, identity in stale_known:
                print(f"  - {source}: {identity}")
        if args.verify_licenses:
            if license_problems:
                print("\nLicense text contradicts the manifest:")
                for name, problems in license_problems:
                    for problem in problems:
                        print(f"  - {name}: {problem}")
            if truncated_notices:
                print("\nNOTICE.md reproduces an incomplete MIT permission notice:")
                for name, missing in truncated_notices:
                    print(f"  - {name}: missing {', '.join(missing)}")
            if unverified:
                print(
                    f"\nLicense not verified against a local tree ({len(unverified)}) — "
                    "not checked out, so not proof of anything:"
                )
                for name in unverified:
                    print(f"  - {name}")
            if not (license_problems or truncated_notices):
                print("\nLicense verification: no problems found")
            if offline_problems:
                print("\nOffline-fetch contract broken:")
                for name, problems in offline_problems:
                    for problem in problems:
                        print(f"  - {name}: {problem}")
            if offline_unverified:
                print("\nOffline fetch not verified (tree not checked out): "
                      + ", ".join(offline_unverified))
            if offline_verified and not offline_problems:
                print("Offline-fetch contract: FetchContent only in "
                      + ", ".join(offline_verified))

    vellum_status, vellum_problems = ("skipped", [])
    if args.strict:
        vellum_status, vellum_problems = vellum_renderer_drift(manifest)
        if vellum_problems:
            print(f"\nVellum renderer pin drift ({vellum_status}):")
            for problem in vellum_problems:
                print(f"  - {problem}")
        elif vellum_status == "match":
            print("\nVellum renderer pin: matches the manifest or is covered by the base pin transition")
        pin_problems = gpu_runtime_pin_agreement(manifest)
        if pin_problems:
            print("\ngpu-runtime pin drift:")
            for problem in pin_problems:
                print(f"  - {problem}")
            vellum_problems = [*vellum_problems, *pin_problems]
        lock_problems = pulp_lock_agreement(manifest)
        if lock_problems:
            print("\npulp.lock drift:")
            for problem in lock_problems:
                print(f"  - {problem}")
            vellum_problems = [*vellum_problems, *lock_problems]

    if args.require_offline_trees and offline_unverified:
        print("\nOffline-fetch trees required but absent: " + ", ".join(offline_unverified))
        return 1
    if args.strict and (
        missing_deps or missing_notice or missing_licensing or uncovered or stale_known
        or license_problems or truncated_notices or offline_problems or vellum_problems
    ):
        return 1
    return 0


def offline_fetch_main(manifest: list[dict], require_trees: bool) -> int:
    """Only the offline-fetch contract: what a configured build checks."""
    failed = False
    checked = 0
    for dep in manifest:
        status, problems = verify_offline_fetch(dep)
        if status == "not-required":
            continue
        checked += 1
        if status == "unverified":
            print(f"{dep['name']}: offline fetch NOT VERIFIED: its pinned tree is not available")
            failed = failed or require_trees
        for problem in problems:
            print(f"{dep['name']}: {problem}")
            failed = True
        if status == "verified" and not problems:
            print(f"{dep['name']}: offline fetch verified (FetchContent only)")
    if checked == 0:
        print("no dependency is marked offline_fetch: nothing was checked")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
