from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools/ci/validate_windows_vm_manifest.py"
SPEC = importlib.util.spec_from_file_location("validate_windows_vm_manifest", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class WindowsVmManifestTests(unittest.TestCase):
    def manifest(self, directory: Path) -> Path:
        base = directory / "base.qcow2"
        code = directory / "code.fd"
        vars_path = directory / "vars.fd"
        for path, payload in ((base, b"base"), (code, b"code"), (vars_path, b"vars")):
            path.write_bytes(payload)
        manifest = directory / "manifest.toml"
        manifest.write_text(
            "\n".join(
                (
                    "schema = 1",
                    'strategy = "ephemeral-overlay"',
                    'provisioning = "ephemeral"',
                    'architecture = "arm64"',
                    "[base_image]",
                    f'path = "{base}"',
                    f'sha256 = "{digest(base)}"',
                    "[firmware]",
                    f'code_path = "{code}"',
                    f'code_sha256 = "{digest(code)}"',
                    f'vars_template_path = "{vars_path}"',
                    f'vars_template_sha256 = "{digest(vars_path)}"',
                    "[overlay]",
                    'state_dir = "/tmp/pulp-windows-test"',
                    'cleanup = "on-exit"',
                    "copy_on_write = true",
                    "[cache]",
                    'mode = "guest-local"',
                )
            )
            + "\n"
        )
        return manifest

    def test_valid_manifest_returns_all_pinned_paths(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            paths = MODULE.load_manifest(self.manifest(Path(tmp)))
            self.assertEqual(set(paths), {"base", "firmware", "vars", "state"})

    def test_digest_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self.manifest(Path(tmp))
            base = Path(tmp) / "base.qcow2"
            base.write_bytes(b"changed")
            with self.assertRaisesRegex(MODULE.ManifestError, "sha256 mismatch"):
                MODULE.load_manifest(manifest)

    def test_missing_base_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self.manifest(Path(tmp))
            (Path(tmp) / "base.qcow2").unlink()
            with self.assertRaisesRegex(MODULE.ManifestError, "base_image is missing"):
                MODULE.load_manifest(manifest)

    def test_override_still_uses_manifest_digest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest = self.manifest(Path(tmp))
            override = Path(tmp) / "other.qcow2"
            override.write_bytes(b"other")
            with self.assertRaisesRegex(MODULE.ManifestError, "sha256 mismatch"):
                MODULE.load_manifest(manifest, str(override))


if __name__ == "__main__":
    unittest.main()
