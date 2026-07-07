// gpu_surface_android_jni.cpp — JNI `extern "C"` bridge for the Android
// GPU surface.
//
// Owns the Kotlin-facing `Java_com_pulp_render_*` symbol surface for the GPU
// host. Simple setters and touch down/move/up events forward directly to the
// `android_*` entry points declared in gpu_surface_android_internal.hpp;
// nativeOnTouchCancel routes through android_touch_cancel so recognizers see
// a cancelled release before capture clears. Surface creation, destruction,
// and file drops also manage JNI references, drag backend state, and Java-array
// marshalling at this boundary. Lifecycle exports bridge C++ exceptions to
// Java; nativeOnDrop logs instead of throwing, and trivial setters/touch
// forwarders rely on their callees to stay non-throwing.

#if defined(__ANDROID__)

#include "gpu_surface_android_internal.hpp"

#include <pulp/platform/android/jni.hpp>
#include <pulp/view/drag_drop.hpp>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_LOG_TAG, __VA_ARGS__)

// ── Outbound file drag (host-less Android: JNI up-call into Kotlin) ──────────
//
// Android's view tree is the bare global g_root_view with no Window/PluginView
// host, so View::start_file_drag() falls back to the process-global drag
// backend (drag_drop.hpp). We register a backend that up-calls the cached
// PulpSurfaceView's `startFileDrag(String[]): Boolean`, which builds a ClipData
// of FileProvider content URIs and runs View.startDragAndDrop. The surface ref
// is cached at surfaceCreated; the up-call runs on whichever thread initiated
// the drag (touch dispatch → UI thread), and Kotlin re-posts to the UI thread.

namespace {

// g_surface_view is written on the render thread (nativeOnSurfaceCreated) and
// cleared on the UI thread (nativeOnSurfaceDestroyed) while a drag may read it
// on the UI thread — guard all three with a mutex.
std::mutex g_surface_view_mutex;
pulp::android::GlobalRef g_surface_view;  // PulpSurfaceView

bool android_start_file_drag(const std::vector<std::string>& paths) {
    if (paths.empty()) return false;
    // Hold the lock for the whole up-call so the GlobalRef can't be deleted by a
    // concurrent surfaceDestroyed mid-use. The call is short (Kotlin posts the
    // actual startDragAndDrop to the UI loop).
    std::lock_guard<std::mutex> lock(g_surface_view_mutex);
    if (!g_surface_view) return false;
    JNIEnv* env = pulp::android::get_env();
    jobject view = g_surface_view.get();
    jclass cls = env->GetObjectClass(view);
    jmethodID mid = env->GetMethodID(cls, "startFileDrag", "([Ljava/lang/String;)Z");
    env->DeleteLocalRef(cls);
    if (!mid) { env->ExceptionClear(); return false; }

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray arr =
        env->NewObjectArray(static_cast<jsize>(paths.size()), str_cls, nullptr);
    for (jsize i = 0; i < static_cast<jsize>(paths.size()); ++i) {
        jstring s = env->NewStringUTF(paths[static_cast<size_t>(i)].c_str());
        if (!s) {  // OOM → a pending exception; bail before the next JNI call aborts
            env->ExceptionClear();
            env->DeleteLocalRef(arr);
            env->DeleteLocalRef(str_cls);
            return false;
        }
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(str_cls);

    const jboolean ok = env->CallBooleanMethod(view, mid, arr);
    env->DeleteLocalRef(arr);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); return false; }
    return ok == JNI_TRUE;
}

}  // namespace

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
        // Cache the PulpSurfaceView for outbound-drag up-calls and register the
        // process-global drag backend View::start_file_drag falls back to.
        {
            std::lock_guard<std::mutex> lock(g_surface_view_mutex);
            g_surface_view = pulp::android::GlobalRef(env, thiz);
        }
        pulp::view::set_file_drag_backend([](const pulp::view::FileDragRequest& req) {
            return android_start_file_drag(req.file_paths);
        });

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
        // Drop the outbound-drag backend + cached view: the surface (and the
        // view it up-calls) is going away. Symmetric with the surfaceCreated
        // registration; avoids a post-destroy up-call into a detached view and
        // leaving a stale GlobalRef whose dtor would DeleteGlobalRef at teardown.
        pulp::view::set_file_drag_backend(nullptr);
        {
            std::lock_guard<std::mutex> lock(g_surface_view_mutex);
            g_surface_view = pulp::android::GlobalRef{};
        }
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
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchCancel(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y) {
    pulp::render::android_touch_cancel(pointerId, x, y);
}

// Native file drop — Kotlin's OnDragListener resolved the drag's ClipData
// content URIs into absolute cache-file paths and passes them here with the
// drop point (physical px). Forwards into the shared dispatch core.
JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnDrop(
    JNIEnv* env, jobject, jobjectArray jpaths, jfloat x, jfloat y) {
    std::vector<std::string> paths;
    if (jpaths) {
        const jsize n = env->GetArrayLength(jpaths);
        paths.reserve(static_cast<size_t>(n));
        for (jsize i = 0; i < n; ++i) {
            auto js = static_cast<jstring>(env->GetObjectArrayElement(jpaths, i));
            if (!js) continue;
            if (const char* c = env->GetStringUTFChars(js, nullptr)) {
                paths.emplace_back(c);
                env->ReleaseStringUTFChars(js, c);
            }
            env->DeleteLocalRef(js);
        }
    }
    try {
        pulp::render::android_on_drop(paths, x, y);
    } catch (const std::exception& e) {
        PULP_LOGE("nativeOnDrop threw: %s", e.what());
    } catch (...) {
        PULP_LOGE("Unknown C++ exception in nativeOnDrop");
    }
}

// ── Accessibility (TalkBack) ─────────────────────────────────────────────
// REMOVED: the accessibility JNI exports have moved to
// core/view/platform/android/accessibility_android.cpp which uses the
// correct C++ enum values directly (no role mapping table). The old
// code here had wrong role mappings that caused TalkBack to announce
// controls as the wrong widget type.

} // extern "C"

#endif // __ANDROID__
