#!/usr/bin/env python3
"""Tests for check_web_plugin_docs.py."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINT = ROOT / "tools/scripts/check_web_plugin_docs.py"


def run(root: Path) -> int:
    return subprocess.run(
        [sys.executable, str(LINT), "--root", str(root)],
        capture_output=True, text=True,
    ).returncode


def test_passes_on_real_tree():
    assert run(ROOT) == 0


def test_fails_on_false_path_claim(tmp_path):
    # Minimal fake tree: a doc that references a path that does not exist.
    doc = tmp_path / "docs/reference/web-plugin-support.md"
    doc.parent.mkdir(parents=True)
    doc.write_text("Runtime at `core/format/src/wasm/missing.mjs`.\n")
    # Provide the required helper so only the bad path trips it.
    helper = tmp_path / "tools/cmake/PulpWam.cmake"
    helper.parent.mkdir(parents=True)
    helper.write_text("function(pulp_add_wam_plugin NAME)\nendfunction()\n")
    (tmp_path / "docs/guides").mkdir(parents=True)
    (tmp_path / "docs/guides/web-plugins.md").write_text("ok\n")
    assert run(tmp_path) == 1


def test_fails_when_helper_missing(tmp_path):
    (tmp_path / "docs/reference").mkdir(parents=True)
    (tmp_path / "docs/reference/web-plugin-support.md").write_text("no paths\n")
    (tmp_path / "docs/guides").mkdir(parents=True)
    (tmp_path / "docs/guides/web-plugins.md").write_text("no paths\n")
    # No PulpWam.cmake -> helper-missing error.
    assert run(tmp_path) == 1


if __name__ == "__main__":
    test_passes_on_real_tree()
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        test_fails_on_false_path_claim(Path(d))
    with tempfile.TemporaryDirectory() as d:
        test_fails_when_helper_missing(Path(d))
    print("all tests passed")
