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
   (``requirements-docs.txt``, ``mkdocs.yml``, ``CMakeLists.txt``'s
   ``FetchContent_Declare`` blocks, ``external/``, or a recognized
   redistributed bootstrap binary) is represented by a manifest entry. This
   class of check was missing, which is how #582 shipped MkDocs Material
   without touching the four attribution files.

3. **Truthfulness** (``--verify-licenses``) — the attribution surfaces must
   agree with the license text actually on disk. The checks above only ask
   whether a dependency is *named* in each file, so both of the following
   passed a green ``--strict`` audit: NOTICE.md reproduced a truncated MIT
   license for 25 of its 26 MIT entries, and the VST3 SDK was labeled MIT
   across all four surfaces while the pinned tree was "Steinberg VST3
   License OR GPLv3". A name being present says nothing about the text being
   right.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


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
    # (family, phrase identifying it, phrase for its inclusion condition)
    ("MIT", "to deal in the Software without restriction", "shall be included in all"),
    ("BSL-1.0", "Boost Software License", "must be included in all copies"),
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
        for _family, identifier, condition in LICENSE_FAMILIES:
            if identifier not in body:
                continue
            missing = []
            if condition not in body:
                missing.append("inclusion condition")
            if LICENSE_DISCLAIMER not in body:
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
    "WebGPU-distribution": ("webgpu", "wgpu-native"),
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
) -> list[DeclaredDep]:
    """Aggregate declared deps across all manifest sources.

    ``extra_*`` are injection hooks for tests — they replace the default
    paths when provided. Tests pass synthetic fixtures to verify the
    completeness gate catches missing deps without touching real repo
    files.
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
        declared.extend(parse_fetchcontent(ROOT_CMAKELISTS))
        for cm in EXTRA_CMAKELISTS:
            declared.extend(parse_fetchcontent(cm))
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
    for dep in manifest:
        covered |= manifest_alias_set(dep)
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
        if key in ignored or key in covered:
            continue
        uncovered.append(dep)
    return uncovered


def run_git_ls_remote(repo: str, *refs: str) -> str:
    cmd = ["git", "ls-remote", repo, *refs]
    try:
        result = subprocess.run(
            cmd,
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
            timeout=5,
        )
        return result.stdout.strip()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return ""


SEMVER_RE = re.compile(r"(\d+)\.(\d+)\.(\d+)")


def semver_key(value: str):
    match = SEMVER_RE.search(value)
    if not match:
        return None
    return tuple(int(part) for part in match.groups())


def latest_semver_tag(repo: str) -> str | None:
    output = run_git_ls_remote(repo, "--tags", "--refs")
    candidates: list[tuple[tuple[int, int, int], str]] = []
    for line in output.splitlines():
        if not line:
            continue
        ref = line.split("\t", 1)[1]
        tag = ref.removeprefix("refs/tags/")
        key = semver_key(tag)
        if key is not None:
            candidates.append((key, tag))
    if not candidates:
        return None
    candidates.sort()
    return candidates[-1][1]


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
        latest = latest_semver_tag(repo)
        if latest and latest != ref:
            return f"{exact}; latest={latest}"
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
    args = parser.parse_args()

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
    uncovered = find_uncovered_declarations(manifest, declared)

    truncated_notices: list[tuple[str, list[str]]] = []
    license_problems: list[tuple[str, list[str]]] = []
    unverified: list[str] = []
    if args.verify_licenses:
        truncated_notices = find_notice_truncations()
        for dep in manifest:
            status, problems = verify_dep_license(dep)
            if status == "unverified":
                unverified.append(dep["name"])
            if problems:
                license_problems.append((dep["name"], problems))

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
                print(f"  - {dep.name} from {dep.source}{where}")
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

    if args.strict and (
        missing_deps or missing_notice or missing_licensing or uncovered
        or license_problems or truncated_notices
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
