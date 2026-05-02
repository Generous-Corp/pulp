#!/usr/bin/env python3
"""Focused coverage for tools/packages/validate_registry.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "tools" / "packages" / "validate_registry.py"

_SPEC = importlib.util.spec_from_file_location(
    "validate_registry_under_test", MODULE_PATH
)
assert _SPEC and _SPEC.loader
vr = importlib.util.module_from_spec(_SPEC)
sys.modules["validate_registry_under_test"] = vr
_SPEC.loader.exec_module(vr)


@contextlib.contextmanager
def argv(args: list[str]):
    old = sys.argv[:]
    sys.argv = args[:]
    try:
        yield
    finally:
        sys.argv = old


def write_json(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload), encoding="utf-8")


class LoadJsonTests(unittest.TestCase):
    def test_load_json_returns_dict_and_reports_input_errors(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            valid = root / "valid.json"
            write_json(valid, {"ok": True})
            self.assertEqual(vr.load_json(valid), {"ok": True})

            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                vr.load_json(root / "missing.json")
            self.assertEqual(cm.exception.code, 1)
            self.assertIn("not found", err.getvalue())

            malformed = root / "bad.json"
            malformed.write_text("{bad", encoding="utf-8")
            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                vr.load_json(malformed)
            self.assertEqual(cm.exception.code, 1)
            self.assertIn("ERROR:", err.getvalue())


class SchemaValidationTests(unittest.TestCase):
    def test_validate_schema_formats_sorted_paths_and_import_fallback(self) -> None:
        class FakeError:
            def __init__(self, path: list[str], message: str) -> None:
                self.absolute_path = path
                self.path = path
                self.message = message

        class FakeValidator:
            def __init__(self, schema: dict) -> None:
                self.schema = schema

            def iter_errors(self, registry: dict):
                return [
                    FakeError(["packages", "bad"], "bad package"),
                    FakeError([], "root problem"),
                ]

        fake_jsonschema = SimpleNamespace(Draft7Validator=FakeValidator)
        with mock.patch.dict(sys.modules, {"jsonschema": fake_jsonschema}):
            self.assertEqual(
                vr.validate_schema({"packages": {}}, {}),
                ["  (root): root problem", "  packages.bad: bad package"],
            )

        real_import = __import__

        def blocked_import(name, *args, **kwargs):
            if name == "jsonschema":
                raise ImportError("missing")
            return real_import(name, *args, **kwargs)

        err = io.StringIO()
        with mock.patch("builtins.__import__", side_effect=blocked_import), \
             contextlib.redirect_stderr(err):
            self.assertEqual(vr.validate_schema({}, {}), [])
        self.assertIn("jsonschema not installed", err.getvalue())


class ValidationHelperTests(unittest.TestCase):
    def test_structural_validation_reports_all_error_and_warning_classes(self) -> None:
        registry = {
            "registry_version": 1,
            "packages": {
                "Bad_Slug": {
                    "version": "1.0.0",
                    "verification": {
                        "verified_version": "0.9.0",
                        "build_status": {"macOS-ARM64": "pass"},
                    },
                    "platforms": {},
                },
                "desktop-only": {
                    "version": "2.0.0",
                    "verification": {
                        "verified_version": "2.0.0",
                        "build_status": {"Linux-x64": "pass"},
                    },
                    "platforms": {"macOS": {}, "Windows": {}},
                },
                "mobile-ready": {
                    "version": "3.0.0",
                    "verification": {
                        "verified_version": "3.0.0",
                        "build_status": {"Android-arm64": "pass"},
                    },
                    "platforms": {"Android": {}, "iOS": {}},
                },
            },
        }

        errors, warnings = vr.validate_structural(registry)

        self.assertIn("registry_version must be 2", errors)
        self.assertTrue(any("Bad_Slug: invalid slug" in item for item in errors))
        self.assertTrue(any("Bad_Slug: must have at least one platform" in item for item in errors))
        self.assertTrue(any("verified_version" in item for item in warnings))
        self.assertTrue(any("build_status key 'macOS-ARM64'" in item for item in warnings))
        self.assertTrue(any("desktop-only: no Android or iOS" in item for item in warnings))
        self.assertFalse(any("mobile-ready" in item for item in warnings))

    def test_license_validation_splits_allowed_rejected_and_unknown(self) -> None:
        errors, warnings = vr.validate_licenses(
            {
                "packages": {
                    "ok": {"license": "MIT"},
                    "copyleft": {"license": "GPL-3.0-only"},
                    "custom": {"license": "Commercial"},
                }
            }
        )

        self.assertEqual(
            errors,
            ["  copyleft: license 'GPL-3.0-only' is incompatible with Pulp's MIT license"],
        )
        self.assertEqual(len(warnings), 1)
        self.assertIn("custom", warnings[0])
        self.assertIn("not in known-allowed list", warnings[0])


class MainTests(unittest.TestCase):
    def test_script_entrypoint_invokes_main_for_help(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(MODULE_PATH), "--help"],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )

        self.assertEqual(completed.returncode, 0)
        self.assertIn("Validate the Pulp package registry", completed.stdout)
        self.assertEqual(completed.stderr, "")

    def test_main_reports_success_with_overlap_note(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            registry = root / "registry.json"
            schema = root / "schema.json"
            write_json(
                registry,
                {
                    "registry_version": 2,
                    "packages": {
                        "mobile-ready": {
                            "version": "1.0.0",
                            "license": "MIT",
                            "verification": {
                                "verified_version": "1.0.0",
                                "build_status": {"iOS-arm64": "pass"},
                            },
                            "platforms": {"iOS": {}},
                            "overlaps_with_builtin": {"filters": ["biquad"]},
                        }
                    },
                },
            )
            write_json(schema, {})

            out = io.StringIO()
            with mock.patch.object(vr, "REGISTRY", registry), \
                 mock.patch.object(vr, "SCHEMA", schema), \
                 mock.patch.object(vr, "validate_schema", return_value=[]), \
                 argv(["validate_registry.py", "--strict", "--check-licenses"]), \
                 contextlib.redirect_stdout(out):
                rc = vr.main()

        self.assertEqual(rc, 0)
        text = out.getvalue()
        self.assertIn("mobile-ready", text)
        self.assertIn("OK (1 built-in overlaps noted)", text)
        self.assertIn("1 packages validated, 0 errors, 0 warnings", text)

    def test_main_reports_schema_structural_license_and_strict_failures(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            registry = root / "registry.json"
            schema = root / "schema.json"
            write_json(
                registry,
                {
                    "registry_version": 2,
                    "packages": {
                        "schema-bad": {
                            "version": "1.0.0",
                            "license": "MIT",
                            "verification": {
                                "verified_version": "1.0.0",
                                "build_status": {"Linux-x64": "pass"},
                            },
                            "platforms": {"Android": {}},
                        },
                        "warning-only": {
                            "version": "1.0.0",
                            "license": "Commercial",
                            "verification": {
                                "verified_version": "0.9.0",
                                "build_status": {"badkey": "pass"},
                            },
                            "platforms": {},
                        },
                        "bad-license": {
                            "version": "2.0.0",
                            "license": "GPL-2.0-only",
                            "verification": {
                                "verified_version": "2.0.0",
                                "build_status": {"Windows-x64": "pass"},
                            },
                            "platforms": {"iOS": {}},
                        },
                    },
                },
            )
            write_json(schema, {})

            out = io.StringIO()
            with mock.patch.object(vr, "REGISTRY", registry), \
                 mock.patch.object(vr, "SCHEMA", schema), \
                 mock.patch.object(vr, "validate_schema", return_value=["  packages.schema-bad: schema error"]), \
                 argv(["validate_registry.py", "--strict", "--check-licenses"]), \
                 contextlib.redirect_stdout(out):
                rc = vr.main()

        self.assertEqual(rc, 1)
        text = out.getvalue()
        self.assertIn("Schema validation errors:", text)
        self.assertIn("packages.schema-bad: schema error", text)
        self.assertIn("Structural errors:", text)
        self.assertIn("Warnings:", text)
        self.assertIn("warning-only", text)
        self.assertIn("License errors:", text)
        self.assertIn("bad-license", text)
        self.assertIn("License warnings:", text)
        self.assertIn("3 packages validated, 3 errors, 4 warnings", text)

    def test_main_strict_mode_fails_on_warnings_without_errors(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            registry = root / "registry.json"
            schema = root / "schema.json"
            write_json(
                registry,
                {
                    "registry_version": 2,
                    "packages": {
                        "warning-only": {
                            "version": "1.0.0",
                            "license": "MIT",
                            "verification": {
                                "verified_version": "0.9.0",
                                "build_status": {"Linux-x64": "pass"},
                            },
                            "platforms": {"macOS": {}},
                        },
                    },
                },
            )
            write_json(schema, {})

            out = io.StringIO()
            with mock.patch.object(vr, "REGISTRY", registry), \
                 mock.patch.object(vr, "SCHEMA", schema), \
                 mock.patch.object(vr, "validate_schema", return_value=[]), \
                 argv(["validate_registry.py", "--strict"]), \
                 contextlib.redirect_stdout(out):
                rc = vr.main()

        self.assertEqual(rc, 1)
        self.assertIn("1 packages validated, 0 errors, 2 warnings", out.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
