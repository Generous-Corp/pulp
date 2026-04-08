#if defined(__ANDROID__)

#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/platform/android/jni.hpp>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::render {

// ── Android GPU Surface Manager ───────────────────────────────────────────
// Manages the lifecycle of Dawn/Vulkan rendering on Android.
// Bridges ANativeWindow from Kotlin SurfaceView to GpuSurface.

static void android_render_test_frame();
static std::unique_ptr<GpuSurface> g_gpu_surface;
static std::unique_ptr<SkiaSurface> g_skia_surface;
static ANativeWindow* g_native_window = nullptr;
static std::mutex g_surface_mutex;
static std::condition_variable g_surface_cv;
static bool g_render_stopped = true;
static bool g_surface_valid = false;

void android_surface_created(ANativeWindow* window) {
    PULP_LOGI("Android GPU surface: ANativeWindow received (%dx%d)",
              ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));

    g_native_window = window;
    ANativeWindow_acquire(g_native_window);

    // Create and initialize Dawn GPU surface
    g_gpu_surface = GpuSurface::create_dawn();
    if (!g_gpu_surface) {
        PULP_LOGE("Android GPU surface: failed to create Dawn GpuSurface");
        return;
    }

    GpuSurface::Config config;
    config.width = static_cast<uint32_t>(ANativeWindow_getWidth(window));
    config.height = static_cast<uint32_t>(ANativeWindow_getHeight(window));
    config.native_surface_handle = g_native_window;
    config.vsync = true;

    if (g_gpu_surface->initialize(config)) {
        PULP_LOGI("Android GPU surface: Dawn initialized (%ux%u)",
                  config.width, config.height);

        auto info = g_gpu_surface->adapter_info();
        PULP_LOGI("Android GPU surface: adapter=%s backend=%s",
                  info.name.c_str(), info.backend.c_str());

        // Create Skia Graphite surface wrapping the Dawn device
        SkiaSurface::Config skia_config;
        skia_config.width = config.width;
        skia_config.height = config.height;
        skia_config.scale_factor = 2.0f;  // typical Android density

        g_skia_surface = SkiaSurface::create(*g_gpu_surface, skia_config);
        if (g_skia_surface && g_skia_surface->is_available()) {
            PULP_LOGI("Android GPU surface: Skia Graphite context created");
        } else {
            PULP_LOGW("Android GPU surface: Skia Graphite failed — Dawn-only mode");
        }

        {
            std::lock_guard lock(g_surface_mutex);
            g_surface_valid = true;
            g_render_stopped = false;
        }

        // Render an initial test frame
        android_render_test_frame();
    } else {
        PULP_LOGW("Android GPU surface: Dawn initialization failed — no GPU rendering");
        g_gpu_surface.reset();
    }
}

void android_surface_resized(int width, int height) {
    if (g_gpu_surface && g_gpu_surface->is_initialized()) {
        g_gpu_surface->resize(static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height));
        PULP_LOGI("Android GPU surface: resized to %dx%d", width, height);

        // Render a test frame — prove the full pipeline works
        android_render_test_frame();
    }
}

static float g_hue = 0.0f;

void android_render_test_frame() {
    if (!g_gpu_surface || !g_gpu_surface->is_initialized()) return;

    if (g_gpu_surface->begin_frame()) {
        if (g_skia_surface && g_skia_surface->is_available()) {
            auto* canvas = g_skia_surface->begin_frame();
            if (canvas) {
                using Color = canvas::Color;
                float w = static_cast<float>(g_gpu_surface->width());
                float h = static_cast<float>(g_gpu_surface->height());

                // Bright red background — impossible to miss
                canvas->set_fill_color(Color::rgba(255, 0, 0));
                canvas->fill_rect(0, 0, w, h);

                // Green bar
                canvas->set_fill_color(Color::rgba(0, 255, 0));
                canvas->fill_rect(20, 20, w - 40, 80);

                // Blue rectangle
                canvas->set_fill_color(Color::rgba(0, 100, 255));
                canvas->fill_rect(50, 120, w - 100, 80);

                // White center square
                canvas->set_fill_color(Color::rgba(255, 255, 255));
                float cx = w / 2;
                float cy = h / 2;
                canvas->fill_rect(cx - 60, cy - 60, 120, 120);

                // Purple inner
                canvas->set_fill_color(Color::rgba(108, 92, 231));
                canvas->fill_rect(cx - 40, cy - 40, 80, 80);

                // Yellow bar at bottom
                canvas->set_fill_color(Color::rgba(255, 255, 0));
                canvas->fill_rect(20, h - 60, w - 40, 40);

                g_skia_surface->end_frame();
                PULP_LOGI("Android GPU surface: Skia frame rendered (%dx%d)",
                          g_gpu_surface->width(), g_gpu_surface->height());
            }
        }
        g_gpu_surface->end_frame();
    }
}

void android_surface_destroyed() {
    PULP_LOGI("Android GPU surface: destroying — waiting for render stop...");

    {
        std::lock_guard lock(g_surface_mutex);
        g_surface_valid = false;
    }

    // Wait for render to confirm stop (condition_variable, not spin)
    {
        std::unique_lock lock(g_surface_mutex);
        g_surface_cv.wait(lock, [] { return g_render_stopped; });
    }

    g_gpu_surface.reset();

    if (g_native_window) {
        ANativeWindow_release(g_native_window);
        g_native_window = nullptr;
    }

    PULP_LOGI("Android GPU surface: destroyed");
}

// Called by the render loop each frame
bool android_gpu_begin_frame() {
    std::lock_guard lock(g_surface_mutex);
    if (!g_surface_valid || !g_gpu_surface) return false;
    return g_gpu_surface->begin_frame();
}

void android_gpu_end_frame() {
    if (g_gpu_surface) g_gpu_surface->end_frame();
}

void android_gpu_signal_render_stopped() {
    {
        std::lock_guard lock(g_surface_mutex);
        g_render_stopped = true;
    }
    g_surface_cv.notify_one();
}

GpuSurface* android_gpu_surface() {
    return g_gpu_surface.get();
}

} // namespace pulp::render

// ── Touch Input ───────────────────────────────────────────────────────────

namespace pulp::input {
using TouchCallback = void(*)(int, float, float, float, int, void*);
static TouchCallback g_touch_callback = nullptr;
static void* g_touch_callback_data = nullptr;

void set_touch_callback(TouchCallback cb, void* user_data) {
    g_touch_callback = cb;
    g_touch_callback_data = user_data;
}
enum TouchAction { Down = 0, Move = 1, Up = 2, Cancel = 3 };
} // namespace pulp::input

// ── JNI Exports ───────────────────────────────────────────────────────────

extern "C" {

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

// Touch events
JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchDown(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y, jfloat pressure) {
    if (pulp::input::g_touch_callback)
        pulp::input::g_touch_callback(pointerId, x, y, pressure,
                                       pulp::input::Down, pulp::input::g_touch_callback_data);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchMove(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y, jfloat pressure) {
    if (pulp::input::g_touch_callback)
        pulp::input::g_touch_callback(pointerId, x, y, pressure,
                                       pulp::input::Move, pulp::input::g_touch_callback_data);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchUp(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y) {
    if (pulp::input::g_touch_callback)
        pulp::input::g_touch_callback(pointerId, x, y, 0.0f,
                                       pulp::input::Up, pulp::input::g_touch_callback_data);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchCancel(JNIEnv*, jobject) {
    if (pulp::input::g_touch_callback)
        pulp::input::g_touch_callback(-1, 0, 0, 0,
                                       pulp::input::Cancel, pulp::input::g_touch_callback_data);
}

} // extern "C"

#endif // __ANDROID__
