#!/usr/bin/env python3
"""Host-side contract checks for the private Android SurfaceRuntime seam.

These checks deliberately inspect the Android sources without pretending that a
host build proves an Android/NDK or emulator run.  They pin ownership and JNI
surface invariants that would otherwise only fail late in an APK build or
during surface recreation.
"""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
ANDROID_RENDER = ROOT / "core" / "render" / "platform" / "android"
DEMO = (ANDROID_RENDER / "gpu_surface_android.cpp").read_text(encoding="utf-8")
RUNTIME = (ANDROID_RENDER / "surface_runtime_android.cpp").read_text(encoding="utf-8")
INTERNAL = (ANDROID_RENDER / "gpu_surface_android_internal.hpp").read_text(encoding="utf-8")
JNI = (ANDROID_RENDER / "gpu_surface_android_jni.cpp").read_text(encoding="utf-8")
KOTLIN = (
    ROOT / "android" / "app" / "src" / "main" / "kotlin" / "com" / "pulp"
    / "render" / "PulpSurfaceView.kt"
).read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")


class SurfaceRuntimeOwnershipContract(unittest.TestCase):
    def test_runtime_owns_native_gpu_lifecycle(self) -> None:
        for token in (
            "ANativeWindow_acquire",
            "ANativeWindow_release",
            "GpuSurface::create_dawn",
            "AChoreographer_getInstance",
            "active_frames_",
            "surface_destroyed",
        ):
            self.assertIn(token, RUNTIME)

        # The demo TU reaches the lifecycle only through the private owner.
        self.assertIn("static SurfaceRuntime g_surface_runtime{android_render_frame};", DEMO)
        self.assertIn("g_surface_runtime.surface_created(window, g_display_density);", DEMO)
        self.assertIn("// Keep the old first-frame behavior", DEMO)
        self.assertIn("android_render_frame();", DEMO)
        for forbidden in (
            "static std::unique_ptr<GpuSurface>",
            "static std::unique_ptr<SkiaSurface>",
            "static ANativeWindow* g_native_window",
            "static AChoreographer* g_choreographer",
            "static std::mutex g_surface_mutex",
        ):
            self.assertNotIn(forbidden, DEMO)

    def test_failure_and_destroy_release_in_order(self) -> None:
        self.assertIn("surface_valid_ = false", RUNTIME)
        self.assertIn("skia_surface_.reset();", RUNTIME)
        self.assertIn("gpu_surface_.reset();", RUNTIME)
        self.assertIn("frame_cv_.wait(lock, [this] { return active_frames_ == 0; });", RUNTIME)
        self.assertLess(RUNTIME.index("skia_surface_.reset();"), RUNTIME.index("gpu_surface_.reset();"))
        self.assertIn("ANativeWindow_release(native_window_)", RUNTIME)

    def test_frame_access_is_guarded(self) -> None:
        self.assertIn("bool SurfaceRuntime::begin_frame()", RUNTIME)
        self.assertIn("++active_frames_;", RUNTIME)
        self.assertIn("struct FrameGuard", DEMO)
        self.assertIn("runtime.end_frame();", DEMO)

    def test_demo_fallback_and_interaction_boundary_remains(self) -> None:
        for token in (
            "create_js_ui",
            "kSynthUiScript",
            "create_demo_view_hierarchy",
            "android_touch_down",
            "android_touch_cancel",
            "android_on_drop",
        ):
            self.assertIn(token, DEMO)
        self.assertIn("script.empty()", DEMO)

    def test_jni_names_remain_stable(self) -> None:
        names = (
            "nativeSetDisplayDensity",
            "nativeSetSafeAreaInsets",
            "nativeOnSurfaceCreated",
            "nativeOnSurfaceResized",
            "nativeOnSurfaceDestroyed",
            "nativeGetGpuAdapterInfo",
            "nativeOnTouchDown",
            "nativeOnTouchMove",
            "nativeOnTouchUp",
            "nativeOnTouchCancel",
            "nativeOnDrop",
        )
        for name in names:
            self.assertIn(f"Java_com_pulp_render_PulpSurfaceView_{name}", JNI)

    def test_android_owner_and_callbacks_are_wired(self) -> None:
        self.assertIn("core/render/platform/android/surface_runtime_android.cpp", CMAKE)
        for token in (
            "HandlerThread(\"PulpRenderThread\")",
            "renderHandler",
            "lifecycleLock",
            "initComplete",
            "pendingSurfaceHolder",
            "scheduleDeferredSurfaceCreateLocked",
            "surfaceCreated while teardown is pending",
            "handler.post",
            "CountDownLatch(1)",
            "nativeOnSurfaceDestroyed()",
            "surfaceDestroyed during init",
        ):
            self.assertIn(token, KOTLIN)


if __name__ == "__main__":
    unittest.main()
