#!/usr/bin/env python3
"""Enforce the pulp-format-core / pulp-format-view header boundary.

pulp-format-core is exactly what compiles and links without the view layer.
That rule only holds if no header a core consumer can reach drags pulp/view in,
so this walks the transitive include closure of every header under
core/format/include and classifies it.

A header is view-coupled if its closure touches pulp/view, pulp/canvas or
pulp/render. Those headers are listed in VIEW_HEADERS below; every other header
must stay view-free. A header that starts reaching view without being added to
the list fails this check, which is the signal that either the include is
unintended or the target boundary has moved.

The walk deliberately ignores preprocessor conditionals: it follows every
#include regardless of #if. That over-approximates -- clap_adapter.hpp is
view-free when PULP_CLAP_GUI is off, and is still listed here -- which is the
safe direction, because it can only classify a header as more coupled than it
is, never less.

Exit codes: 0 clean, 1 boundary violated, 2 the walker could not resolve part of
the include graph and its verdicts cannot be trusted.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
VIEW_PREFIXES = ("pulp/view/", "pulp/canvas/", "pulp/render/")

# Headers whose include closure legitimately reaches the view layer. These are
# the pulp-format-view surface. Keep sorted.
VIEW_HEADERS = {
    "pulp/format/clap_adapter.hpp",
    "pulp/format/clap_entry.hpp",
    "pulp/format/detail/au_v2_editor_resize.hpp",
    "pulp/format/detail/standalone_audio_capture_rolling_wav.hpp",
    "pulp/format/detail/standalone_audio_capture_wav.hpp",
    "pulp/format/detail/standalone_audio_scope_json.hpp",
    "pulp/format/detail/standalone_editor_chrome.hpp",
    "pulp/format/detail/standalone_environment.hpp",
    "pulp/format/detail/standalone_musical_typing.hpp",
    "pulp/format/editor_idle_pump.hpp",
    "pulp/format/editor_ui.hpp",
    "pulp/format/gpu_host_select.hpp",
    "pulp/format/reload/pack_build.hpp",
    "pulp/format/reload/reloadable_shell.hpp",
    "pulp/format/reload/remote_update.hpp",
    "pulp/format/reload/remote_update_fetch.hpp",
    "pulp/format/reload/scripted_ui_swap_unit.hpp",
    "pulp/format/settings_panel.hpp",
    "pulp/format/standalone.hpp",
    "pulp/format/standalone_settings.hpp",
    "pulp/format/view_bridge.hpp",
    "pulp/format/vst3_plug_view.hpp",
}

# The authoring headers pulp::dsl and pulp::sequence consume. If any of these
# ever becomes view-coupled the split has failed at its stated purpose, so they
# are named rather than left to the general rule.
AUTHORING_HEADERS = {
    "pulp/format/processor.hpp",
    "pulp/format/process_context.hpp",
    "pulp/format/graph_runtime_executor.hpp",
}


def include_dirs(repo: str) -> list[str]:
    core = os.path.join(repo, "core")
    dirs = []
    for entry in sorted(os.listdir(core)):
        candidate = os.path.join(core, entry, "include")
        if os.path.isdir(candidate):
            dirs.append(candidate)
    return dirs


def build_walker(dirs: list[str]):
    cache: dict[str, tuple[frozenset, frozenset]] = {}

    def resolve(name: str) -> str | None:
        for d in dirs:
            path = os.path.join(d, name)
            if os.path.isfile(path):
                return path
        return None

    def walk(path: str, stack: frozenset = frozenset()) -> tuple[frozenset, frozenset]:
        if path in cache:
            return cache[path]
        if path in stack:
            return frozenset(), frozenset()
        views: set[str] = set()
        unresolved: set[str] = set()
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                lines = handle.read().splitlines()
        except OSError:
            return frozenset(), frozenset()
        for line in lines:
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            name = match.group(1)
            if not name.startswith("pulp/"):
                continue
            if name.startswith(VIEW_PREFIXES):
                views.add(name)
                continue
            target = resolve(name)
            if target is None:
                unresolved.add(name)
                continue
            sub_views, sub_unresolved = walk(target, stack | {path})
            views |= sub_views
            unresolved |= sub_unresolved
        result = (frozenset(views), frozenset(unresolved))
        cache[path] = result
        return result

    return walk


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, help="repository root")
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    fmt_include = os.path.join(repo, "core", "format", "include")
    if not os.path.isdir(fmt_include):
        print(f"FAIL: {fmt_include} is not a directory", file=sys.stderr)
        return 2

    dirs = include_dirs(repo)
    walk = build_walker(dirs)

    headers = []
    for dirpath, _dirnames, filenames in os.walk(fmt_include):
        for filename in filenames:
            if filename.endswith((".hpp", ".h")):
                headers.append(os.path.join(dirpath, filename))
    headers.sort()

    if not headers:
        print("FAIL: no format headers found; the walker is misconfigured",
              file=sys.stderr)
        return 2

    reaching: dict[str, list[str]] = {}
    unresolved_any: dict[str, list[str]] = {}
    for path in headers:
        rel = os.path.relpath(path, fmt_include)
        views, unresolved = walk(path)
        if views:
            reaching[rel] = sorted(views)
        if unresolved:
            unresolved_any[rel] = sorted(unresolved)

    # An unresolved pulp/* include means the walker could not see part of the
    # graph, so a "view-free" verdict below would be an artefact of not looking.
    # Refuse to report either way.
    if unresolved_any:
        print("INCONCLUSIVE: unresolved pulp/* includes; verdicts are not "
              "trustworthy", file=sys.stderr)
        for rel, names in sorted(unresolved_any.items()):
            print(f"  {rel}: {', '.join(names)}", file=sys.stderr)
        return 2

    # Control: the walker must be able to SEE view coupling. If nothing at all
    # reaches view, the resolver is broken rather than the tree being clean.
    if not reaching:
        print("INCONCLUSIVE: no format header reaches pulp/view at all, which "
              "means the include resolver is not working -- view_bridge.hpp "
              "alone should always reach it", file=sys.stderr)
        return 2

    failures = []

    unexpected = sorted(set(reaching) - VIEW_HEADERS)
    for rel in unexpected:
        failures.append(
            f"{rel} reaches the view layer but is classified to "
            f"pulp-format-core (via {', '.join(reaching[rel])})")

    stale = sorted(VIEW_HEADERS - set(reaching))
    for rel in stale:
        failures.append(
            f"{rel} is listed as view-coupled but no longer reaches the view "
            f"layer; move it to pulp-format-core and drop it from the list")

    for rel in sorted(AUTHORING_HEADERS):
        if rel in reaching:
            failures.append(
                f"{rel} is an authoring header consumed by pulp::dsl and "
                f"pulp::sequence and must never reach the view layer "
                f"(via {', '.join(reaching[rel])})")

    if failures:
        print("FAIL: pulp-format header boundary violated", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print(f"OK: {len(headers)} format headers checked, "
          f"{len(reaching)} view-coupled as declared, "
          f"{len(headers) - len(reaching)} view-free")
    return 0


if __name__ == "__main__":
    sys.exit(main())
