// gpu_surface_android_internal.hpp — PRIVATE shared declarations for the
// Android GPU-surface translation units.
//
// Shared by gpu_surface_android.cpp and gpu_surface_android_jni.cpp. This
// header declares the render entry points that the JNI bridge calls after any
// Java-boundary work it owns, such as GlobalRef management, ANativeWindow
// conversion, drag-backend registration, and drop-path marshalling.
// nativeOnTouchCancel routes through android_touch_cancel() before clearing
// shared legacy capture state. All paint and lifecycle state stays private to
// gpu_surface_android.cpp.
//
// PRIVATE: lives under core/render/platform/android/, not the public
// include tree. Not part of the installed SDK surface — do not reference
// from headers outside core/render/platform/android/.

#pragma once

#if defined(__ANDROID__)

#include <string>
#include <vector>

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
void android_touch_cancel(int pointer_id, float px_x, float px_y);

// Native file drop into the View hierarchy. `paths` are absolute filesystem
// paths the Kotlin layer resolved from the drag's ClipData content URIs
// (copied into the app cache); `px_x/px_y` are the drop point in physical
// pixels (converted to dp here, like touch). Routes through the shared
// dispatch core (dispatch_drop) — the same path the mac/win/linux/iOS hosts use.
void android_on_drop(const std::vector<std::string>& paths, float px_x, float px_y);

// Shared touch-capture pointer. Defined in gpu_surface_android.cpp. Non-owning
// — valid only while g_root_view exists.
extern pulp::view::View* g_captured_view;

}  // namespace pulp::render

#endif  // __ANDROID__
