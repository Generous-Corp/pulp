// gpu_surface_android_internal.hpp — PRIVATE shared declarations for the
// Android GPU-surface translation units.
//
// Shared by gpu_surface_android.cpp and gpu_surface_android_jni.cpp. This
// header declares the render entry points that the JNI bridge calls after any
// Java-boundary work it owns, such as GlobalRef management, ANativeWindow
// conversion, drag-backend registration, and drop-path marshalling.
// nativeOnTouchCancel routes through android_touch_cancel() before clearing
// shared legacy capture state. Demo paint/touch state stays private to
// gpu_surface_android.cpp; SurfaceRuntime owns native GPU lifecycle state.
//
// PRIVATE: lives under core/render/platform/android/, not the public
// include tree. Not part of the installed SDK surface — do not reference
// from headers outside core/render/platform/android/.

#pragma once

#if defined(__ANDROID__)

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ANativeWindow;
struct AChoreographer;

namespace pulp::view {
class View;
}

namespace pulp::render {

class GpuSurface;
class SkiaSurface;

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

// ── GPU adapter identity ─────────────────────────────────────────────────
// Snapshot of the adapter Dawn actually initialized, for the Kotlin driver
// policy. `available` is false before android_surface_created() finished
// initialization and after android_surface_destroyed(), so a caller can tell
// "no adapter yet" apart from "an adapter that reported blank strings".
struct AndroidGpuAdapterIdentity {
    bool available = false;
    std::string name;
    std::string vendor;
    std::string driver;
};

// Owns all Android surface resources and the thread-affine frame scheduler.
//
// The Kotlin/JNI boundary remains responsible for converting a Java Surface to
// ANativeWindow and for forwarding lifecycle events. Demo state (the view
// hierarchy, script bridge, synth, and touch routing) remains in
// gpu_surface_android.cpp and is reached through frame_callback. Keeping the
// ownership here makes the lifecycle contract explicit:
//
//   * surface_created acquires exactly one ANativeWindow reference;
//   * a failed Dawn setup (including an exception) releases that reference
//     before returning; a Skia failure degrades to the documented Dawn-only
//     mode;
//   * destroy marks the surface invalid, stops Choreographer, waits for active
//     frames, then tears down Skia, Dawn, and the window in that order;
//   * callbacks posted before stop are weak-token callbacks and therefore safe
//     no-ops after teardown.
class SurfaceRuntime {
public:
    using FrameCallback = std::function<void(float)>;

    explicit SurfaceRuntime(FrameCallback frame_callback);
    ~SurfaceRuntime();

    SurfaceRuntime(const SurfaceRuntime&) = delete;
    SurfaceRuntime& operator=(const SurfaceRuntime&) = delete;

    // These methods are called from the Android UI/render thread, the same
    // thread that owns the ALooper required by AChoreographer.
    void surface_created(ANativeWindow* window, float scale_factor);
    void surface_resized(int width, int height);
    void surface_destroyed();

    // Frame access is guarded against surface teardown. begin_frame() keeps
    // an active-frame count until the matching end_frame() call returns.
    bool begin_frame();
    void end_frame();

    GpuSurface* gpu_surface() const;
    SkiaSurface* skia_surface() const;
    AndroidGpuAdapterIdentity adapter_identity() const;

private:
    struct LoopState;

    static void on_vsync(long frame_time_nanos, void* data);
    static void post_next_frame(const std::shared_ptr<LoopState>& state);
    void start_render_loop();
    void stop_render_loop();

    FrameCallback frame_callback_;
    std::unique_ptr<GpuSurface> gpu_surface_;
    std::unique_ptr<SkiaSurface> skia_surface_;
    ANativeWindow* native_window_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable frame_cv_;
    bool surface_valid_ = false;
    std::size_t active_frames_ = 0;

    std::shared_ptr<LoopState> loop_state_;
};

AndroidGpuAdapterIdentity android_gpu_adapter_identity();

// Shared touch-capture pointer. Defined in gpu_surface_android.cpp. Non-owning
// — valid only while g_root_view exists.
extern pulp::view::View* g_captured_view;

}  // namespace pulp::render

#endif  // __ANDROID__
