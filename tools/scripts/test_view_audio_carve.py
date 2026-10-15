#!/usr/bin/env python3
"""Check the G2h view-audio ownership split.

This is a source-level gate for the part of the carve that a link test cannot
prove: the selected implementation files and public headers must be owned by
the Pulp-kept audio target while the generic view source list stays free of
them.  Include paths remain stable, so installed consumers do not need a path
rewrite; the target boundary is the contract being checked here.
"""
from __future__ import annotations

import argparse
import pathlib
import sys


HEADERS = (
    "audio_bridge.hpp",
    "midi_keyboard.hpp",
    "musical_typing.hpp",
    "musical_typing_keyboard.hpp",
    "step_grid_view.hpp",
    "waveform_recorder_view.hpp",
)
SOURCES = (
    "midi_keyboard.cpp",
    "musical_typing_keyboard.cpp",
    "musical_typing_keyboard_svg.cpp",
    "step_grid_view.cpp",
    "waveform_recorder_view.cpp",
    "waveform_recorder_view_svg.cpp",
)


def problems(root: pathlib.Path) -> list[str]:
    found: list[str] = []
    source_list = (root / "core/view/cmake/PulpViewSources.cmake").read_text(encoding="utf-8")
    widgets = (root / "core/view/include/pulp/view/widgets.hpp").read_text(encoding="utf-8")
    view_all = (root / "core/view/include/pulp/view/view_all.hpp").read_text(encoding="utf-8")
    plugin_host = (root / "core/view/include/pulp/view/plugin_view_host.hpp").read_text(encoding="utf-8")
    window_host = (root / "core/view/platform/mac/window_host_mac.mm").read_text(encoding="utf-8")

    for name in HEADERS:
        audio = root / "core/view/audio/include/pulp/view" / name
        generic = root / "core/view/include/pulp/view" / name
        if not audio.is_file():
            found.append(f"missing audio header: {audio.relative_to(root)}")
        if generic.exists():
            found.append(f"audio header still resides in view-core: {generic.relative_to(root)}")

    for name in SOURCES:
        audio = root / "core/view/audio/src" / name
        generic = root / "core/view/src" / name
        if not audio.is_file():
            found.append(f"missing audio source: {audio.relative_to(root)}")
        if generic.exists():
            found.append(f"audio source still resides in view-core: {generic.relative_to(root)}")
        if f"audio/src/{name}" not in source_list:
            found.append(f"audio source is not listed for pulp-view-audio: {name}")
        if f"    src/{name}" in source_list:
            found.append(f"audio source remains listed for pulp-view-core: {name}")

    if "#include <pulp/view/audio_bridge.hpp>" in widgets:
        found.append("widgets.hpp directly includes audio_bridge.hpp")
    if "#if defined(PULP_VIEW_HAS_AUDIO_WIDGETS)" not in widgets or \
            "#include <pulp/view/audio_widgets.hpp>" not in widgets:
        found.append("widgets.hpp has no guarded audio compatibility include")
    if "#include <pulp/view/audio_bridge.hpp>" in view_all:
        found.append("view_all.hpp directly includes audio_bridge.hpp")
    if "register_platform_plugin_view_host()" not in plugin_host:
        found.append("PluginViewHost debt comment omits register_platform_plugin_view_host()")
    if "DEFINED in pulp-view-tooling" not in plugin_host:
        found.append("PluginViewHost declaration does not record the tooling definition owner")
    if "MusicalTypingKeyboard popout" in window_host:
        found.append("view-platform window host comment names a Pulp-kept widget")
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    found = problems(args.root)
    if found:
        for item in found:
            print(f"view-audio-carve: {item}", file=sys.stderr)
        return 1
    print("view-audio-carve: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
