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
   ``FetchContent_Declare`` blocks, or ``external/``) is represented by a
   manifest entry. This class of check was missing, which is how #582
   shipped MkDocs Material without touching the four attribution files.
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


def load_manifest() -> list[dict]:
    return json.loads(MANIFEST.read_text())["dependencies"]


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
# The audit normalises both the declared name and each manifest entry
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
    """Return the set of normalised aliases that match this manifest entry."""
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

    Name normalisation later maps ``material`` to ``mkdocs-material``,
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
    # (pip requirements, mkdocs.yml, FetchContent_Declare, external/)
    # must be represented in manifest.json.
    declared = collect_declared()
    uncovered = find_uncovered_declarations(manifest, declared)

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

    if args.strict and (
        missing_deps or missing_notice or missing_licensing or uncovered
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
