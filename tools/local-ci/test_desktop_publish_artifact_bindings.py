#!/usr/bin/env python3
"""Tests for desktop publish artifact dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_publish_artifact_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopPublishArtifactBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_publish_exports_match_wrappers(self) -> None:
        expected = (
            "desktop_publish_root",
            "create_desktop_publish_bundle",
        )

        self.assertEqual(self.mod.DESKTOP_PUBLISH_ARTIFACT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_publish_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        artifacts = types.SimpleNamespace(
            desktop_publish_root=capture("publish_root", Path("/artifacts/_published")),
            create_desktop_publish_bundle=capture("publish_bundle", Path("/artifacts/_published/run")),
        )
        bindings = {"_desktop_artifacts": artifacts}
        config = {"desktop_automation": {"artifact_root": "/artifacts"}}

        self.assertEqual(self.mod.desktop_publish_root(bindings, config), Path("/artifacts/_published"))
        self.assertEqual(captured["publish_root"][0], (config,))
        self.assertEqual(self.mod.create_desktop_publish_bundle(bindings, config), Path("/artifacts/_published/run"))
        self.assertEqual(captured["publish_bundle"][0], (config,))

    def test_publish_installer_wires_named_export(self) -> None:
        bindings = {
            "_desktop_artifacts": types.SimpleNamespace(desktop_publish_root=lambda config: Path("/published")),
        }

        self.mod.install_desktop_publish_artifact_helpers(bindings, ("desktop_publish_root",))

        self.assertEqual(bindings["desktop_publish_root"]({}), Path("/published"))
        self.assertNotIn("create_desktop_publish_bundle", bindings)

    def test_publish_installer_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_publish_artifact_helper = lambda _bindings: "future"

        self.mod.install_desktop_publish_artifact_helpers(bindings, ("future_desktop_publish_artifact_helper",))

        self.assertEqual(bindings["future_desktop_publish_artifact_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
