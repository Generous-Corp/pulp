// gpu_surface_android_jni.cpp — JNI `extern "C"` bridge for the Android
// GPU surface.
//
// Relocated from gpu_surface_android.cpp in the P8-1 refactor. Every
// JNIEXPORT below is a thin forwarder onto the `android_*` entry points
// declared in gpu_surface_android_internal.hpp (defined in
// gpu_surface_android.cpp). The exports stay welded together in their own
// TU because they share the Kotlin-facing `Java_com_pulp_render_*` symbol
// surface and the same exception-bridging contract.

#if defined(__ANDROID__)

#include "gpu_surface_android_internal.hpp"

#include <pulp/platform/android/jni.hpp>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <stdexcept>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_LOG_TAG, __VA_ARGS__)

// ── JNI Exports ───────────────────────────────────────────────────────────

extern "C" {

// Display density — call from Kotlin before surface is created
JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeSetDisplayDensity(
    JNIEnv*, jobject, jfloat density) {
    pulp::render::android_set_display_density(density);
}

// Safe area insets (dp) — status bar, nav bar, notch
JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeSetSafeAreaInsets(
    JNIEnv*, jobject, jfloat top, jfloat bottom, jfloat left, jfloat right) {
    pulp::render::android_set_safe_area_insets(top, bottom, left, right);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnSurfaceCreated(
    JNIEnv* env, jobject thiz, jobject surface) {
    try {
        ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
        if (window) {
            pulp::render::android_surface_created(window);
            ANativeWindow_release(window);  // android_surface_created acquires its own ref
        } else {
            PULP_LOGE("ANativeWindow_fromSurface returned null");
        }
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnSurfaceCreated");
    }
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnSurfaceResized(
    JNIEnv* env, jobject thiz, jint width, jint height) {
    try {
        pulp::render::android_surface_resized(width, height);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnSurfaceResized");
    }
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnSurfaceDestroyed(
    JNIEnv* env, jobject thiz) {
    try {
        pulp::render::android_surface_destroyed();
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnSurfaceDestroyed");
    }
}

// Touch events → View hierarchy
JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchDown(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y, jfloat pressure) {
    pulp::render::android_touch_down(pointerId, x, y, pressure);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchMove(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y, jfloat pressure) {
    pulp::render::android_touch_move(pointerId, x, y, pressure);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchUp(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y) {
    pulp::render::android_touch_up(pointerId, x, y);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchCancel(JNIEnv*, jobject) {
    pulp::render::g_captured_view = nullptr;
}

// ── Accessibility (TalkBack) ─────────────────────────────────────────────
// REMOVED: the accessibility JNI exports have moved to
// core/view/platform/android/accessibility_android.cpp which uses the
// correct C++ enum values directly (no role mapping table). The old
// code here had wrong role mappings that caused TalkBack to announce
// controls as the wrong widget type.

} // extern "C"

#endif // __ANDROID__
