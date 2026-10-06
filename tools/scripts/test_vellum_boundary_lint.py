#!/usr/bin/env python3
"""Self-test for the Vellum design-import boundary lint, including its red control."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

LINT = Path(__file__).with_name("vellum_boundary_lint.py")
GATES = Path(__file__).with_name("gates.sh")


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def run(root: Path, *manifests: Path) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(LINT), "--root", str(root)]
    for manifest in manifests:
        command.extend(("--manifest", str(manifest)))
    return subprocess.run(command, text=True, capture_output=True, check=False)


def main() -> int:
    # The cheap gate must fail closed if the boundary instrument disappears;
    # otherwise a renamed or omitted linter silently turns the ownership check
    # green. Keep this as a planted wiring control beside the runtime lint
    # controls below.
    gates = GATES.read_text(encoding="utf-8")
    missing_guard = 'if [ ! -f "$VELLUM_BOUNDARY" ]; then'
    if missing_guard not in gates or 'fail=1' not in gates.split(missing_guard, 1)[1].split('fi', 1)[0]:
        print("gates.sh does not fail closed when the Vellum boundary linter is missing",
              file=sys.stderr)
        return 1
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
        manifest_path = root / "tools/import-design/pulp-package.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for escaped_root in (str(root.parent), "../outside"):
            manifest["root"] = escaped_root
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            invalid_root = run(root)
            if invalid_root.returncode != 2 or "root must stay beneath" not in invalid_root.stderr:
                print("manifest-root negative control did not fail closed", file=sys.stderr)
                print(invalid_root.stdout, invalid_root.stderr, file=sys.stderr)
                return 1
        manifest["root"] = "tools/import-design"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        # Planted violation: this must turn the same instrument red.  A test
        # that only checks the current tree would let the boundary silently rot.
        write(root / "tools/import-design/planted.cpp", '#include "core/view/src/private.hpp"\n')
        invalid = run(root)
        if invalid.returncode == 0 or "private core/view include" not in invalid.stderr:
            print("boundary negative control did not fail closed", file=sys.stderr)
            print(invalid.stdout, invalid.stderr, file=sys.stderr)
            return 1
        write(root / "tools/ui-build/planted-dynamic.ts",
              'const load = () => import("../../core/view/src/private.js");\n')
        dynamic = run(root)
        if dynamic.returncode == 0 or "private core/view module reference" not in dynamic.stderr:
            print("dynamic import negative control did not fail closed", file=sys.stderr)
            print(dynamic.stdout, dynamic.stderr, file=sys.stderr)
            return 1
        write(root / "packages/pulp-react/planted-require.js",
              'const privateView = require("../../core/view/src/private.js");\n')
        require = run(root)
        if require.returncode == 0 or "private core/view module reference" not in require.stderr:
            print("require negative control did not fail closed", file=sys.stderr)
            print(require.stdout, require.stderr, file=sys.stderr)
            return 1
        print("vellum_boundary_contract_verified=valid-current;planted-private-include")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
