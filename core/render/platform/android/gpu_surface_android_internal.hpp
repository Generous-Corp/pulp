// gpu_surface_android_internal.hpp — PRIVATE shared declarations for the
// Android GPU-surface translation units.
//
// Created in the P8-1 refactor that split the JNI `extern "C"` bridge out
// of gpu_surface_android.cpp into gpu_surface_android_jni.cpp. The JNI
// layer is a thin forwarder: every export calls one of the `android_*`
// entry points below (already external-linkage free functions in
// gpu_surface_android.cpp) — except nativeOnTouchCancel, which clears the
// shared touch-capture pointer directly. That one global (g_captured_view)
// is the only file-local static promoted to external linkage for the
// split; all paint/lifecycle state stays private to gpu_surface_android.cpp.
//
// PRIVATE: lives under core/render/platform/android/, not the public
// include tree. Not part of the installed SDK surface — do not reference
// from headers outside core/render/platform/android/.

#pragma once

#if defined(__ANDROID__)

struct ANativeWindow;

namespace pulp::view {
class View;
}

namespace pulp::render {

// ── Android GPU-surface entry points ─────────────────────────────────────
// Defined in gpu_surface_android.cpp. The JNI `extern "C"` exports in
// gpu_surface_android_jni.cpp forward directly to these.

// Display density — set from Kotlin before the surface is created.
void android_set_display_density(float density);

// Safe-area insets (dp) — status bar, nav bar, notch.
void android_set_safe_area_insets(float top, float bottom,
                                  float left, float right);

// Surface lifecycle — ANativeWindow create / resize / destroy.
void android_surface_created(ANativeWindow* window);
void android_surface_resized(int width, int height);
void android_surface_destroyed();

// Touch routing into the View hierarchy.
void android_touch_down(int pointer_id, float px_x, float px_y, float pressure);
void android_touch_move(int pointer_id, float px_x, float px_y, float pressure);
void android_touch_up(int pointer_id, float px_x, float px_y);

// Shared touch-capture pointer. Defined in gpu_surface_android.cpp; cleared
// directly by the nativeOnTouchCancel JNI export. Non-owning — valid only
// while g_root_view exists.
extern pulp::view::View* g_captured_view;

}  // namespace pulp::render

#endif  // __ANDROID__
