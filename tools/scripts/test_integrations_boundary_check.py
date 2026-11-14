#!/usr/bin/env python3
"""Self-test for integrations_boundary_check.py, including its red controls."""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

CHECK = Path(__file__).with_name("integrations_boundary_check.py")
REPO = Path(__file__).resolve().parents[2]


def run(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(CHECK), "--root", str(root)],
                          text=True, capture_output=True, check=False, encoding="utf-8")


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def main() -> int:
    failures = []

    def expect(name: str, ok: bool, detail: str = "") -> None:
        if not ok:
            failures.append(f"{name}: {detail}")

    # The real tree is clean, and the check really looked at core.
    real = run(REPO)
    expect("real tree clean", real.returncode == 0, real.stderr)
    expect("real tree scanned core", "integrations_boundary_verified=" in real.stdout
           and int(real.stdout.split("=")[1].split()[0]) > 100, real.stdout)

    planted = {
        "target link": ("core/format/CMakeLists.txt", "target_link_libraries(x PUBLIC pulp::vellum)\n"),
        "raw target": ("core/format/CMakeLists.txt", "target_link_libraries(x PRIVATE pulp-vellum)\n"),
        "path": ("core/state/CMakeLists.txt", "add_subdirectory(../../integrations/pulp-vellum)\n"),
        "header": ("core/view/src/a.cpp", '#include <pulp/vellum/param_port_model.hpp>\n'),
        "vellum header": ("core/view/src/a.cpp", '#include <vellum/value/param_model.hpp>\n'),
        "sdk target": ("core/render/CMakeLists.txt", "target_link_libraries(x PRIVATE Vellum::Value)\n"),
        "option": ("core/format/CMakeLists.txt", "if(PULP_ENABLE_PULP_VELLUM)\n"),
    }
    for name, (rel, text) in planted.items():
        with tempfile.TemporaryDirectory(prefix="pulp-integrations-boundary-") as raw:
            root = Path(raw)
            write(root / "integrations/CMakeLists.txt", "")
            write(root / "core/format/CMakeLists.txt", "add_library(x)\n")
            write(root / rel, text)
            result = run(root)
            expect(f"planted {name} fails", result.returncode == 1, f"rc={result.returncode} {result.stderr}")

    # A tree with nothing to scan is an error, not a pass.
    with tempfile.TemporaryDirectory(prefix="pulp-integrations-boundary-") as raw:
        root = Path(raw)
        (root / "core").mkdir()
        (root / "integrations").mkdir()
        expect("blind scan is an error", run(root).returncode == 2)
        expect("missing core is an error", run(root / "nowhere").returncode == 2)

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
