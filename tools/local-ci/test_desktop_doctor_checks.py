#!/usr/bin/env python3
"""Tests for macOS-local desktop doctor check builders (video-proof readiness)."""

from module_test_utils import load_local_ci_module
import unittest


def load_module():
    return load_local_ci_module("desktop_doctor_checks.py")


def _check(name, ok, detail, required=True):
    return {"name": name, "ok": ok, "detail": detail, "required": required}


def _which(found):
    return lambda tool: f"/usr/bin/{tool}" if tool in found else None


class MacosLocalDoctorChecksTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _run(self, **kw):
        defaults = dict(
            platform="darwin",
            which_fn=_which({"screencapture", "osascript"}),
            macos_accessibility_trusted_fn=lambda: True,
            desktop_check_fn=_check,
        )
        defaults.update(kw)
        return {c["name"]: c for c in self.mod.macos_local_doctor_checks(**defaults)}

    def test_screencapture_missing(self):
        checks = self._run(which_fn=_which({"osascript"}))
        self.assertFalse(checks["screencapture"]["ok"])
        self.assertEqual(checks["screencapture"]["detail"], "missing")

    def test_screencapture_probe_failure_reports_detail(self):
        checks = self._run(probe_macos_screencapture_fn=lambda: (False, "no Screen Recording grant"))
        self.assertFalse(checks["screencapture"]["ok"])
        self.assertEqual(checks["screencapture"]["detail"], "no Screen Recording grant")

    def test_screencapture_probe_ok_reports_path(self):
        checks = self._run(probe_macos_screencapture_fn=lambda: (True, "ok"))
        self.assertTrue(checks["screencapture"]["ok"])
        self.assertEqual(checks["screencapture"]["detail"], "/usr/bin/screencapture")

    def test_screencapture_path_without_probe(self):
        checks = self._run()
        self.assertTrue(checks["screencapture"]["ok"])
        self.assertEqual(checks["screencapture"]["detail"], "/usr/bin/screencapture")

    def test_accessibility_untrusted_message(self):
        checks = self._run(macos_accessibility_trusted_fn=lambda: False)
        self.assertFalse(checks["accessibility"]["ok"])
        self.assertIn("Pulp app automation clicks still work without it", checks["accessibility"]["detail"])

    def test_video_capture_and_avfoundation_checks(self):
        checks = self._run(
            resolve_ffmpeg_path_fn=lambda: "/opt/ffmpeg",
            probe_macos_avfoundation_screen_fn=lambda path: (True, f"Capture screen 0 via {path}"),
        )
        self.assertTrue(checks["video_capture"]["ok"])
        self.assertEqual(checks["video_capture"]["detail"], "/opt/ffmpeg")
        self.assertTrue(checks["avfoundation_screen"]["ok"])
        self.assertIn("/opt/ffmpeg", checks["avfoundation_screen"]["detail"])

    def test_video_capture_ffmpeg_resolution_error(self):
        def boom():
            raise RuntimeError("ffmpeg not found")

        checks = self._run(resolve_ffmpeg_path_fn=boom)
        self.assertFalse(checks["video_capture"]["ok"])
        self.assertEqual(checks["video_capture"]["detail"], "ffmpeg not found")
        self.assertNotIn("avfoundation_screen", checks)


if __name__ == "__main__":
    unittest.main()
