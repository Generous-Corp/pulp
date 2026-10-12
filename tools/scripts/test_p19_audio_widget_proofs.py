#!/usr/bin/env python3
"""G2c proof controls for the P19 audio-widget seam."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import tempfile


def run_compile(source_root: pathlib.Path, text: str, audio: bool) -> bool:
    with tempfile.TemporaryDirectory(prefix="p19-m4-") as td:
        unit = pathlib.Path(td) / "probe.cpp"
        unit.write_text(text, encoding="utf-8")
        command = [
            "c++", "-std=c++20", "-fsyntax-only", str(unit),
            "-I", str(source_root / "core/view/include"),
            "-I", str(source_root / "core/view/audio/include"),
            "-I", str(source_root / "core/canvas/include"),
            "-I", str(source_root / "core/runtime/include"),
        ]
        if audio:
            command.append("-DPULP_VIEW_HAS_AUDIO_WIDGETS=1")
        return subprocess.run(command, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0


def m4(source_root: pathlib.Path) -> None:
    probe = "#include <pulp/view/widgets.hpp>\nint main() { pulp::view::Meter meter; }\n"
    if run_compile(source_root, probe, True) is not True:
        raise AssertionError("M4 positive extracted-unit twin did not compile")
    if run_compile(source_root, probe, False) is not False:
        raise AssertionError("M4 negative extracted-unit naming Meter compiled")


def contracts(source_root: pathlib.Path) -> None:
    widgets = (source_root / "core/view/include/pulp/view/widgets.hpp").read_text(encoding="utf-8")
    if "#endif  // PULP_VIEW_HAS_AUDIO_WIDGETS" not in widgets:
        raise AssertionError("compatibility block is not anchored by the required closing comment")
    codegen = (source_root / "core/view/src/design_cpp_tree_codegen.cpp").read_text(encoding="utf-8")
    if re.search(r"pulp::view::(?:Meter|WaveformView|SpectrumView)", codegen):
        raise AssertionError("F3 codegen still names a concrete audio widget")
    if "make_data_display" not in codegen or "LevelDisplay" not in codegen:
        raise AssertionError("F3 codegen interface path is missing")
    planted = codegen + "\nstd::unique_ptr<pulp::view::Meter> planted;\n"
    if not re.search(r"pulp::view::(?:Meter|WaveformView|SpectrumView)", planted):
        raise AssertionError("A4 planted concrete-name negative control is ineffective")
    cmake = (source_root / "core/view/CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"target_compile_definitions\(pulp-view-audio[^\n]+INTERFACE\s+([^\s)]+)", cmake)
    if not match or match.group(1).strip() != "PULP_VIEW_HAS_AUDIO_WIDGETS=1":
        raise AssertionError("P7 interface definitions expose more than the audio-widget gate")
    if "register_audio_widgets();" not in (source_root / "core/format/src/view_bridge.cpp").read_text(encoding="utf-8"):
        raise AssertionError("format-view does not explicitly register audio widgets")
    if "register_audio_widgets();" not in (source_root / "core/format/src/standalone.cpp").read_text(encoding="utf-8"):
        raise AssertionError("standalone does not explicitly register audio widgets")
    design = (source_root / "core/view/include/pulp/view/design_import.hpp").read_text(encoding="utf-8")
    for token in ("class Meter;", "class WaveformView;", "bind_meter(Meter&", "bind_waveform_display(WaveformView&"):
        if token not in design:
            raise AssertionError(f"Phase-6 debt row pattern missing: {token}")
    ledger = json.loads((source_root / "docs/status/p19-phase6-debts.json").read_text(encoding="utf-8"))
    ids = {entry["id"] for entry in ledger["entries"]}
    if ids != {"vellum-header-audio-widgets-include", "design-import-audio-forward-decls"}:
        raise AssertionError("Phase-6 debt ledger does not contain exactly the two P19 rows")


def runtime(source_root: pathlib.Path) -> None:
    archive = source_root / "build/core/view/libpulp-view-audio.a"
    if not archive.exists():
        raise AssertionError("M6.1 installed-SDK prerequisite archive is missing")
    nm = subprocess.run(["nm", "-g", str(archive)], text=True, encoding="utf-8",
                        capture_output=True, check=False).stdout
    if "register_audio_widgets" not in nm:
        raise AssertionError("M6.1 register_audio_widgets is absent from view-audio")
    if not list((source_root / "build/examples").rglob("*.vst3")):
        raise AssertionError("M6.2 built-plugin proof has no validator plugin artifact")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--case", choices=("m4", "contracts", "runtime"), required=True)
    args = parser.parse_args()
    {"m4": m4, "contracts": contracts, "runtime": runtime}[args.case](args.source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
