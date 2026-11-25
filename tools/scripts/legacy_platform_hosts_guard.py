#!/usr/bin/env python3
"""Keep new code from depending on the quarantined legacy platform hosts.

The Windows, Linux, iOS, Android and web UI hosts move into
``integrations/legacy-platform-hosts/`` as porting reference. Each platform's
subtree is removed once its Vellum host lands, so nothing outside the
quarantine may start to depend on it. Until the move, the same files live at
their original paths; both locations count as the quarantine.

The check is lexical, so it needs no configured build. Outside the quarantine
it reports:

* include -- a C/C++/ObjC include of a quarantined header, by its public
  ``pulp/...`` spelling or by a path that resolves into the quarantine;
* path -- a CMake argument naming a quarantined file or directory, resolved
  against the CMake file's own directory and the common root variables;
* link -- a CMake reference to a target that a quarantine CMake file defines.

Today's edges are allowlisted per (file, platform, kind) with a reason and an
exact count. More matches than allowed fail (a new reference hid behind an old
one); fewer fail too, so a removed edge leaves no stale exemption.

Exit codes: 0 clean, 1 violation, 2 scan error, including a scan that found no
CMake files, no sources, or no quarantine member for some platform (the
instrument looked in the wrong place).
"""
from __future__ import annotations

import argparse
import collections
import fnmatch
import os
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import core_vellum_direction as cmake_lex  # noqa: E402
import include_boundary  # noqa: E402

QUARANTINE_ROOT = "integrations/legacy-platform-hosts/"
_Q = QUARANTINE_ROOT

# platform -> quarantine members. A path ending in "/" is a directory. Each
# platform lists its original location and its quarantine location, so the
# guard holds before, during and after the move.
MEMBERS: dict[str, tuple[str, ...]] = {
    # Anything else under the quarantine root (its CMakeLists.txt, README.md).
    "root": (),
    "win": (
        "core/view/platform/win/",
        "core/view/include/pulp/view/platform/uia_mapping.hpp",
        "core/view/include/pulp/view/platform/win_plugin_drop_target.hpp",
        "core/view/include/pulp/view/platform/win_plugin_input_router.hpp",
        "core/view/include/pulp/view/platform/win_pointer_input.hpp",
        "core/view/include/pulp/view/platform/win_surface_lifecycle.hpp",
        _Q + "win/",
    ),
    "linux": (
        "core/view/platform/linux/",
        "core/view/include/pulp/view/platform/atspi_geometry.hpp",
        "core/view/include/pulp/view/platform/atspi_mapping.hpp",
        _Q + "linux/",
    ),
    "ios": (
        "core/view/platform/ios/",
        "core/format/src/au_view_controller_ios.mm",
        _Q + "ios/",
    ),
    "android": (
        "core/view/platform/android/",
        "core/render/platform/android/",
        _Q + "android/",
    ),
    "web": (
        "core/view/platform/web/",
        "core/view/include/pulp/view/web/web_event_translate.hpp",
        _Q + "web/",
    ),
    "tests": (
        "test/test_uia_mapping.cpp",
        "test/test_atspi_mapping.cpp",
        "test/test_win_plugin_input_router.cpp",
        "test/test_win_pointer_input.cpp",
        "test/test_x11_resize_echo.cpp",
        "test/test_linux_pure_helpers.cpp",
        "test/test_motion_android_bridge.cpp",
        "test/test_window_host_web.cpp",
        "test/test_text_accessibility_linux.cpp",
        _Q + "tests/",
    ),
}

# Public include spellings of the quarantined headers. Matched exactly: the
# macOS headers beside them (ns_role_mapping.hpp) are not quarantined.
HEADERS: dict[str, str] = {
    "pulp/view/platform/uia_mapping.hpp": "win",
    "pulp/view/platform/win_plugin_drop_target.hpp": "win",
    "pulp/view/platform/win_plugin_input_router.hpp": "win",
    "pulp/view/platform/win_pointer_input.hpp": "win",
    "pulp/view/platform/win_surface_lifecycle.hpp": "win",
    "pulp/view/platform/atspi_geometry.hpp": "linux",
    "pulp/view/platform/atspi_mapping.hpp": "linux",
    "pulp/view/web/web_event_translate.hpp": "web",
}

# Literal directory markers: the guard reads every current and future file under
# these roots, so changed-surface input selection must take each whole root.
_SCAN_INPUT_ROOTS = ("apple/.", "bindings/.", "core/.", "docs/.", "examples/.",
                     "experimental/.", "fixtures/.", "forge-seam/.", "inspect/.",
                     "integrations/.", "ship/.", "test/.", "tools/.")
SCAN_ROOTS = tuple(r.rstrip("/.") for r in _SCAN_INPUT_ROOTS)
SCAN_FILES = ("CMakeLists.txt",)
SKIP_DIRS = {"_deps", "node_modules", "external", ".git", "planning"}
SOURCE_SUFFIXES = include_boundary.SUFFIXES

# Variables that name a fixed directory. Anything else is resolved by suffix.
_ROOT_VARS = ("CMAKE_SOURCE_DIR", "PROJECT_SOURCE_DIR", "PULP_SOURCE_DIR", "PULP_ROOT")
_HERE_VARS = ("CMAKE_CURRENT_SOURCE_DIR", "CMAKE_CURRENT_LIST_DIR")
_PATH_TOKEN_RE = re.compile(r"[A-Za-z0-9_.${}*/+-]*/[A-Za-z0-9_.${}*/+-]*")
_BARE_NAME_RE = re.compile(r"(?<![A-Za-z0-9_.${}*/+-])[A-Za-z0-9_+-]+"
                           r"\.(?:c|cc|cpp|cxx|m|mm|h|hh|hpp|hxx)(?![A-Za-z0-9_./-])")
_VAR_PREFIX_RE = re.compile(r"^\$\{([A-Za-z0-9_]+)\}/?")
_DEFINE_RE = re.compile(r"^(add_library|add_executable|add_custom_target|"
                        r"pulp_add_test_suite)$", re.I)

_REASON_HOST = "shared target compiles the platform host until that platform builds UI-off"
_REASON_TEST = "shared test registration or test that covers a platform helper"
_REASON_SDK = "installed SDK ships the iOS AUv3 view controller source"
_REASON_DEMO = "web demo drives the browser host's event translation"

# (referencing file, platform, kind) -> (count, reason). Each row is an edge
# that exists today; when the platform's subtree is removed, its rows go too.
ALLOWED: dict[tuple[str, str, str], tuple[int, str]] = {
    ("CMakeLists.txt", "android", "path"): (4, _REASON_HOST),
    ("core/view/CMakeLists.txt", "ios", "path"): (3, _REASON_HOST),
    ("core/view/CMakeLists.txt", "linux", "path"): (5, _REASON_HOST),
    ("core/view/CMakeLists.txt", "win", "path"): (10, _REASON_HOST),
    ("examples/web-demos/super-convolver-ui/ui_entry.cpp", "web", "include"): (1, _REASON_DEMO),
    ("test/cmake/motion_tests.cmake", "android", "path"): (1, _REASON_TEST),
    ("test/cmake/motion_tests.cmake", "tests", "path"): (1, _REASON_TEST),
    ("test/cmake/script_canvas_render_tests.cmake", "ios", "path"): (1, _REASON_TEST),
    ("test/cmake/test_pulp_install_format_sources.cmake", "ios", "path"): (1, _REASON_SDK),
    ("test/test_accessibility_tree.cpp", "linux", "include"): (1, _REASON_TEST),
    ("test/test_drag_drop.cpp", "linux", "include"): (1, _REASON_TEST),
    ("test/test_plugin_frame_renderer.cpp", "win", "include"): (1, _REASON_TEST),
    ("test/test_view_pool.cpp", "win", "include"): (1, _REASON_TEST),
    ("tools/cmake/PulpAndroid.cmake", "android", "path"): (6, _REASON_HOST),
    ("tools/cmake/PulpAuv3.cmake", "ios", "path"): (1, _REASON_HOST),
    ("tools/cmake/PulpInstallRules.cmake", "ios", "path"): (1, _REASON_SDK),
    ("tools/cmake/PulpWebUi.cmake", "web", "path"): (2, _REASON_HOST),
}


def _member_kind(rel: str) -> str | None:
    """The platform whose quarantine contains repo-relative ``rel``, if any."""
    rel = rel.rstrip("/")
    for platform, members in MEMBERS.items():
        for member in members:
            if member.endswith("/"):
                if rel == member.rstrip("/") or rel.startswith(member):
                    return platform
            elif rel == member:
                return platform
    if rel == QUARANTINE_ROOT.rstrip("/") or rel.startswith(QUARANTINE_ROOT):
        return "root"
    return None


def _glob_member_kind(rel: str) -> str | None:
    """Like _member_kind, but ``rel`` may hold glob characters."""
    if not any(ch in rel for ch in "*?["):
        return _member_kind(rel)
    head = rel.split("*")[0].split("?")[0].split("[")[0]
    directory = head if head.endswith("/") else head.rsplit("/", 1)[0] + "/"
    return _member_kind(directory)


def _skip_dir(name: str) -> bool:
    return name in SKIP_DIRS or name.startswith(".") or name.startswith("build")


def _git_files(root: pathlib.Path) -> list[pathlib.Path] | None:
    """Tracked and untracked, non-ignored files under the scan roots, or None."""
    if not (root / ".git").exists():
        return None
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
             "--exclude-standard", "--", *SCAN_ROOTS, *SCAN_FILES],
            check=True, capture_output=True, timeout=120).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    files = []
    for raw in sorted(set(out.decode("utf-8", "surrogateescape").split("\0"))):
        if not raw:
            continue
        parts = raw.split("/")
        if any(_skip_dir(d) for d in parts[:-1]):
            continue
        path = root / raw
        if path.is_file():
            files.append(path)
    return files


def walk(root: pathlib.Path) -> list[pathlib.Path]:
    tracked = _git_files(root)
    if tracked is not None:
        return tracked
    files: list[pathlib.Path] = []
    for name in SCAN_ROOTS:
        base = root / name
        if not base.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(d for d in dirnames if not _skip_dir(d))
            files.extend(pathlib.Path(dirpath) / f for f in sorted(filenames))
    files.extend(p for p in (root / f for f in SCAN_FILES) if p.is_file())
    return files


def strip_cmake_comments(text: str) -> str:
    """CMake text without comments, with every newline kept for line numbers."""
    text = re.sub(r"#\[(=*)\[.*?\]\1\]",
                  lambda m: " " + "\n" * m.group(0).count("\n") + " ", text, flags=re.S)
    out = []
    for line in text.splitlines(keepends=True):
        body = line.rstrip("\r\n")
        out.append(cmake_lex.strip_cmake_comment(body) + line[len(body):])
    return "".join(out)


def _is_cmake(path: pathlib.Path) -> bool:
    return path.name == "CMakeLists.txt" or path.suffix == ".cmake" \
        or path.name.endswith(".cmake.in")


class Scan:
    def __init__(self, root: pathlib.Path):
        self.root = root
        self.files = walk(root)
        self.rel = {p: p.relative_to(root).as_posix() for p in self.files}
        self.member_files = {p for p in self.files if _member_kind(self.rel[p])}
        # A bare file name names a quarantine file only when no file outside the
        # quarantine shares it.
        outside = collections.Counter(p.name for p in self.files if p not in self.member_files)
        self.unique_names = {p.name: _member_kind(self.rel[p]) for p in self.member_files
                             if outside[p.name] == 0}
        self.member_paths = sorted(self.rel[p] for p in self.member_files)

    def present_platforms(self) -> set[str]:
        return {k for k in (_member_kind(r) for r in self.member_paths) if k}

    def _suffix_kind(self, tail: str) -> str | None:
        """Platform of the quarantine path that ``tail`` ends, for an unknown base.

        A bare name matches only a file name unique to the quarantine. A file
        tail needs two components and a directory or glob tail three, so
        ``platform/win/`` under ship/ or core/audio/ never matches.
        """
        tail = tail.strip("/")
        if not tail:
            return None
        globbed = any(ch in tail for ch in "*?[")
        parts = tail.split("/")
        if len(parts) == 1:
            return None if globbed else self.unique_names.get(tail)
        if globbed:
            directory, leaf = tail.rsplit("/", 1)
            if len(directory.split("/")) < 3:
                return None
            for rel in self.member_paths:
                head, name = rel.rsplit("/", 1)
                if ("/" + head).endswith("/" + directory) and fnmatch.fnmatch(name, leaf):
                    return _member_kind(rel)
            return None
        for rel in self.member_paths:
            if ("/" + rel).endswith("/" + tail):
                return _member_kind(rel)
        if len(parts) >= 3:
            for rel in self.member_paths:
                at = ("/" + rel + "/").find("/" + tail + "/")
                if at >= 0 and _member_kind(rel[:at + len(tail)]):
                    return _member_kind(rel)
        return None

    def resolve_cmake_token(self, token: str, cmake_dir: str) -> str | None:
        token = token.strip('"').rstrip(")")
        if QUARANTINE_ROOT.rstrip("/") in token:
            return _glob_member_kind(token[token.find("integrations/"):]) or "root"
        var = _VAR_PREFIX_RE.match(token)
        if var:
            rest = token[var.end():]
            if "${" in rest:
                return None
            bases: list[str] = []
            if var.group(1) in _ROOT_VARS:
                bases = [""]
            elif var.group(1) in _HERE_VARS:
                # In an included .cmake file CMAKE_CURRENT_SOURCE_DIR is the
                # includer's directory, so try the file's and the root's.
                bases = [cmake_dir, ""]
            for base in bases:
                joined = os.path.normpath(os.path.join(base, rest)).replace(os.sep, "/")
                kind = None if joined.startswith("..") else _glob_member_kind(joined)
                if kind:
                    return kind
            return self._suffix_kind(rest)
        if "${" in token or token.startswith("/"):
            return None
        for base in (cmake_dir, ""):
            joined = os.path.normpath(os.path.join(base, token)).replace(os.sep, "/")
            if joined.startswith(".."):
                continue
            kind = _glob_member_kind(joined)
            if kind:
                return kind
        return None

    def defined_targets(self) -> dict[str, str]:
        targets: dict[str, str] = {}
        for p in self.member_files:
            if not _is_cmake(p):
                continue
            text = strip_cmake_comments(p.read_text(encoding="utf-8", errors="replace"))
            for _line, command, body in cmake_lex.cmake_commands(text):
                if _DEFINE_RE.match(command):
                    name = cmake_lex.first_cmake_argument(body)
                    if name and "${" not in name:
                        targets[name] = _member_kind(self.rel[p]) or "root"
        return targets

    def cmake_hits(self, p: pathlib.Path, targets: dict[str, str]) -> list[tuple[int, str, str, str]]:
        rel = self.rel[p]
        cmake_dir = rel.rsplit("/", 1)[0] if "/" in rel else ""
        text = strip_cmake_comments(p.read_text(encoding="utf-8", errors="replace"))
        hits = []
        for m in _PATH_TOKEN_RE.finditer(text):
            kind = self.resolve_cmake_token(m.group(0), cmake_dir)
            if kind:
                hits.append((text.count("\n", 0, m.start()) + 1, kind, "path", m.group(0)))
        # A slash-free directory argument (add_subdirectory(legacy-platform-hosts)).
        for line, command, body in cmake_lex.cmake_commands(text):
            if command.lower() in {"add_subdirectory", "include", "include_directories"}:
                first = cmake_lex.first_cmake_argument(body)
                if first and "/" not in first and "${" not in first:
                    kind = self.resolve_cmake_token(first + "/", cmake_dir)
                    if kind:
                        hits.append((line, kind, "path", first))
        for m in _BARE_NAME_RE.finditer(text):
            kind = self.unique_names.get(m.group(0))
            if kind:
                hits.append((text.count("\n", 0, m.start()) + 1, kind, "path", m.group(0)))
        for name, kind in targets.items():
            for m in re.finditer(r"(?<![A-Za-z0-9_.:/-])" + re.escape(name) + r"(?![A-Za-z0-9_.:/-])",
                                 text):
                hits.append((text.count("\n", 0, m.start()) + 1, kind, "link", name))
        return hits

    def include_hits(self, p: pathlib.Path) -> list[tuple[int, str, str, str]]:
        rel_dir = self.rel[p].rsplit("/", 1)[0]
        hits = []
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return hits
        # Cheap pre-filter: every reportable include names one of these.
        if "platform/" not in text and "pulp/view/web/" not in text \
                and "legacy-platform-hosts" not in text \
                and not any(name in text for name in self.unique_names):
            return hits
        for n, delim, target in include_boundary.include_edges(p):
            target = target.replace("\\", "/")
            kind = HEADERS.get(target)
            if kind is None and QUARANTINE_ROOT.rstrip("/") in target:
                kind = _member_kind(target[target.find("integrations/"):]) or "root"
            if kind is None and "/" in target:
                bases = [rel_dir] if delim == '"' else []
                for base in bases:
                    joined = os.path.normpath(os.path.join(base, target)).replace(os.sep, "/")
                    if not joined.startswith(".."):
                        kind = _member_kind(joined)
                    if kind:
                        break
                if kind is None:
                    kind = self._suffix_kind(target.lstrip("./")) if target.count("/") >= 2 \
                        else None
            if kind:
                hits.append((n, kind, "include", target))
        return hits


def scan(root: pathlib.Path) -> tuple[int, dict[tuple[str, str, str], list[str]], str]:
    s = Scan(root)
    outside = [p for p in s.files if p not in s.member_files]
    cmake_files = [p for p in outside if _is_cmake(p)]
    sources = [p for p in outside if p.suffix in SOURCE_SUFFIXES]
    missing = sorted(set(MEMBERS) - {"root"} - s.present_platforms())
    if not cmake_files or not sources or missing:
        why = []
        if not cmake_files:
            why.append("no CMake files")
        if not sources:
            why.append("no C/C++/ObjC sources")
        if missing:
            why.append("no quarantine member for " + ", ".join(missing))
        return 2, {}, "; ".join(why)
    targets = s.defined_targets()
    hits: dict[tuple[str, str, str], list[str]] = collections.defaultdict(list)
    for p in cmake_files:
        for n, kind, how, what in s.cmake_hits(p, targets):
            hits[(s.rel[p], kind, how)].append(f"{n}: {what}")
    for p in sources:
        for n, kind, how, what in s.include_hits(p):
            hits[(s.rel[p], kind, how)].append(f"{n}: {what}")
    summary = (f"{len(cmake_files)} CMake files, {len(sources)} sources, "
               f"{len(s.member_files)} quarantine files, {len(targets)} quarantine targets")
    return 0, hits, summary


def check(root: pathlib.Path, allowed: dict[tuple[str, str, str], tuple[int, str]],
          report: bool = False) -> int:
    rc, hits, summary = scan(root.resolve())
    if rc:
        print(f"legacy-platform-hosts-guard: scan error in {root}: {summary}; "
              "refusing to report a clean tree", file=sys.stderr)
        return 2
    if report:
        for key, where in sorted(hits.items()):
            print(f"{key[0]}  [{key[1]}/{key[2]}] x{len(where)}")
            for w in where:
                print(f"    {w}")
        print(f"legacy-platform-hosts-guard: report ({summary})")
        return 0
    failures: list[str] = []
    for key, where in sorted(hits.items()):
        limit = allowed.get(key, (0, ""))[0]
        if len(where) > limit:
            failures.append(f"{key[0]}: {len(where)} {key[2]} reference(s) to the {key[1]} "
                            f"quarantine, {limit} allowlisted")
            failures.extend(f"    line {w}" for w in where)
    for key, (count, _reason) in sorted(allowed.items()):
        seen = len(hits.get(key, []))
        if seen < count:
            failures.append(f"{key[0]}: allowlisted {key[2]} reference(s) to the {key[1]} "
                            f"quarantine occur {seen} time(s), not {count}; shrink or remove "
                            "the ALLOWED entry")
    if failures:
        print("legacy-platform-hosts-guard: code outside integrations/legacy-platform-hosts "
              "may not include, name or link the quarantined platform hosts\n")
        for failure in failures:
            print(f"  {failure}")
        return 1
    allowlisted = sum(len(v) for k, v in hits.items() if k in allowed)
    print(f"legacy-platform-hosts-guard: ok ({summary}, {allowlisted} allowlisted reference(s))")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    parser.add_argument("--report", action="store_true",
                        help="list every reference and exit 0 (inventory only)")
    args = parser.parse_args(argv)
    return check(args.root, ALLOWED, report=args.report)


if __name__ == "__main__":
    raise SystemExit(main())
