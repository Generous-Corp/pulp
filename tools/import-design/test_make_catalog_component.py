#!/usr/bin/env python3
"""Regression checks for make_catalog_component.py's registration guidance."""
import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile


ROOT = pathlib.Path(__file__).parent
SPEC = importlib.util.spec_from_file_location("make_catalog_component", ROOT / "make_catalog_component.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def run_generator(output_root):
    with tempfile.TemporaryDirectory(prefix="catalog_contract_") as td:
        repo = pathlib.Path(td)
        scene = 'data:image/svg+xml;base64,PHN2Zy8+'  # <svg/>

        def fake_export(cmd, check):
            pathlib.Path(cmd[cmd.index("--out") + 1]).write_text(
                '{"image":"' + scene + '"}', encoding="utf-8"
            )

        old_run = MODULE.subprocess.run
        old_argv = sys.argv
        MODULE.subprocess.run = fake_export
        sys.argv = [
            str(ROOT / "make_catalog_component.py"),
            "--name", "Audio Probe", "--class", "AudioProbeView", "--node", "1:2",
            "--category", "audio", "--output-root", output_root, "--repo", str(repo),
        ]
        try:
            captured = io.StringIO()
            with contextlib.redirect_stdout(captured):
                MODULE.main()
            return captured.getvalue()
        finally:
            MODULE.subprocess.run = old_run
            sys.argv = old_argv


audio = run_generator("./core/view/audio/")
assert "core/view/cmake/PulpViewSources.cmake" in audio
assert "target_sources(pulp-view-audio PRIVATE)" in audio
assert "audio/src/audio_probe_view.cpp" in audio
assert "core/view/audio/include/pulp/view/audio_widgets.hpp" in audio
assert "core/view/include/pulp/design/design_system.hpp" not in audio
assert "core/view/audio/CMakeLists.txt" not in audio

generic = run_generator("core/view")
assert "target_sources(pulp-view-core PRIVATE)" in generic
assert "core/view/include/pulp/design/design_system.hpp" in generic

print("make_catalog_component registration guidance: all checks passed")
