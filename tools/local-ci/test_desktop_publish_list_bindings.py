#!/usr/bin/env python3
"""Tests for desktop publish listing dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_publish_list_bindings.py")


class DesktopPublishListBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
        }
        for name in [
            "desktop_publish_root",
            "desktop_publish_reports",
            "atomic_write_text",
        ]:
            bindings[name] = object()
        return bindings

    def test_list_exports_match_wrappers(self):
        expected = (
            "desktop_publish_reports",
            "write_desktop_publish_rollups",
        )

        self.assertEqual(self.mod.DESKTOP_PUBLISH_LIST_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_report_listing_and_publish_rollups_bind_dependencies(self):
        cases = [
            (
                "desktop_publish_reports",
                self.mod.desktop_publish_reports,
                {"limit": 2},
                {"desktop_publish_root_fn": "desktop_publish_root"},
                [{"ok": True}],
            ),
            (
                "write_desktop_publish_rollups",
                self.mod.write_desktop_publish_rollups,
                {},
                {
                    "desktop_publish_root_fn": "desktop_publish_root",
                    "desktop_publish_reports_fn": "desktop_publish_reports",
                    "atomic_write_text_fn": "atomic_write_text",
                },
                None,
            ),
        ]
        for runner_name, wrapper, kwargs, expected_bindings, expected in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **runner_kwargs):
                    captured["args"] = args
                    captured["kwargs"] = runner_kwargs
                    return expected

                bindings = self._bindings(runner_name, runner)
                config = {"desktop_automation": {}}
                self.assertEqual(wrapper(bindings, config, **kwargs), expected)
                self.assertEqual(captured["args"], (config,))
                for kwarg, binding_name in expected_bindings.items():
                    self.assertIs(captured["kwargs"][kwarg], bindings[binding_name])
                for key, value in kwargs.items():
                    self.assertEqual(captured["kwargs"][key], value)

if __name__ == "__main__":
    unittest.main()
