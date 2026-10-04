#!/usr/bin/env python3
"""Self-test for the Vellum design-import boundary lint, including its red control."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

LINT = Path(__file__).with_name("vellum_boundary_lint.py")


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def run(root: Path, *manifests: Path) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(LINT), "--root", str(root)]
    for manifest in manifests:
        command.extend(("--manifest", str(manifest)))
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pulp-vellum-boundary-") as raw:
        root = Path(raw)
        write(root / "tools/import-design/pulp-package.json", json.dumps({
            "schema": "pulp.ui.package.v1", "name": "import-design",
            "root": "tools/import-design", "kind": "importer",
            "dependencies": ["pulp::view::design_import"], "test_targets": ["import-design"]
        }))
        write(root / "tools/ui-build/pulp-package.json", json.dumps({
            "schema": "pulp.ui.package.v1", "name": "ui-build",
            "root": "tools/ui-build", "kind": "compiler",
            "dependencies": ["import-design"], "test_targets": ["ui-build"]
        }))
        write(root / "packages/pulp-react/pulp-package.json", json.dumps({
            "schema": "pulp.ui.package.v1", "name": "pulp-react",
            "root": "packages/pulp-react", "kind": "sdk",
            "dependencies": ["pulp::view::WidgetBridge"], "test_targets": ["pulp-react"]
        }))
        write(root / "core/view/include/pulp/view/public.hpp", "// public\n")
        write(root / "tools/import-design/good.cpp", '#include <pulp/view/public.hpp>\n')
        write(root / "tools/ui-build/good.ts", "export const build = true;\n")
        write(root / "packages/pulp-react/good.ts", "export const sdk = true;\n")
        valid = run(root)
        if valid.returncode != 0:
            print(valid.stdout, valid.stderr, file=sys.stderr)
            return 1
        # Planted violation: this must turn the same instrument red.  A test
        # that only checks the current tree would let the boundary silently rot.
        write(root / "tools/import-design/planted.cpp", '#include "core/view/src/private.hpp"\n')
        invalid = run(root)
        if invalid.returncode == 0 or "private core/view include" not in invalid.stderr:
            print("boundary negative control did not fail closed", file=sys.stderr)
            print(invalid.stdout, invalid.stderr, file=sys.stderr)
            return 1
        print("vellum_boundary_contract_verified=valid-current;planted-private-include")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
