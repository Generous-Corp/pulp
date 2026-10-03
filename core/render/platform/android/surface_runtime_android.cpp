#if defined(__ANDROID__)

#include "gpu_surface_android_internal.hpp"

#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

#include <android/choreographer.h>
#include <android/native_window.h>
#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::render {

// A callback already posted to AChoreographer cannot be cancelled. The token
// therefore carries only a weak reference to this state. Once SurfaceRuntime
// stops and releases its strong reference, a trailing callback is a safe no-op.
struct SurfaceRuntime::LoopState {
    std::mutex mutex;
    std::atomic<bool> running{false};
    AChoreographer* choreographer = nullptr;
    int64_t last_frame_nanos = 0;
    SurfaceRuntime::FrameCallback callback;
};

SurfaceRuntime::SurfaceRuntime(FrameCallback frame_callback)
    : frame_callback_(std::move(frame_callback)) {}

SurfaceRuntime::~SurfaceRuntime() {
    surface_destroyed();
}

void SurfaceRuntime::post_next_frame(const std::shared_ptr<LoopState>& state) {
    std::lock_guard lock(state->mutex);
    if (!state->running.load(std::memory_order_acquire) || !state->choreographer)
        return;

    // The token is freed at callback entry. It does not keep the runtime alive
    // and therefore cannot leak when the surface is destroyed between frames.
    auto* token = new std::weak_ptr<LoopState>(state);
    AChoreographer_postFrameCallback(state->choreographer, &SurfaceRuntime::on_vsync,
                                     token);
}

void SurfaceRuntime::on_vsync(long frame_time_nanos, void* data) {
    std::unique_ptr<std::weak_ptr<LoopState>> token(
        static_cast<std::weak_ptr<LoopState>*>(data));
    auto state = token->lock();
    if (!state) return;

    FrameCallback callback;
    float dt = 0.016f;
    {
        std::lock_guard lock(state->mutex);
        if (!state->running.load(std::memory_order_acquire)) return;
        if (state->last_frame_nanos > 0) {
            dt = static_cast<float>(frame_time_nanos - state->last_frame_nanos) / 1e9f;
            dt = std::clamp(dt, 0.001f, 0.1f);
        }
        state->last_frame_nanos = frame_time_nanos;
        callback = state->callback;
    }

    if (callback) callback(dt);
    post_next_frame(state);
}

void SurfaceRuntime::start_render_loop() {
    auto state = std::make_shared<LoopState>();
    {
        std::lock_guard lock(state->mutex);
        state->choreographer = AChoreographer_getInstance();
        if (!state->choreographer) {
            PULP_LOGW("SurfaceRuntime: AChoreographer unavailable — touch-only repaints");
            return;
        }
        state->callback = frame_callback_;
        state->last_frame_nanos = 0;
        state->running.store(true, std::memory_order_release);
    }
    loop_state_ = state;
    post_next_frame(state);
    PULP_LOGI("SurfaceRuntime: Choreographer render loop started");
}

void SurfaceRuntime::stop_render_loop() {
    auto state = std::move(loop_state_);
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        state->running.store(false, std::memory_order_release);
        state->choreographer = nullptr;
        state->callback = {};
    }
    PULP_LOGI("SurfaceRuntime: Choreographer render loop stopped");
}

void SurfaceRuntime::surface_created(ANativeWindow* window, float scale_factor) {
    if (!window) {
        PULP_LOGE("SurfaceRuntime: surface_created received a null ANativeWindow");
        return;
    }

    // SurfaceView can deliver a replacement before a matching destroy callback
    // on some Activity transitions. Releasing the old owner first prevents an
    // ANativeWindow reference and Dawn context from being leaked.
    if (native_window_ || gpu_surface_ || skia_surface_ || loop_state_)
        surface_destroyed();

    PULP_LOGI("SurfaceRuntime: creating surface (%dx%d)",
              ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
    native_window_ = window;
    ANativeWindow_acquire(native_window_);

    auto fail = [this](const char* message) {
        PULP_LOGE("SurfaceRuntime: %s", message);
        stop_render_loop();
        {
            std::lock_guard lock(mutex_);
            surface_valid_ = false;
            skia_surface_.reset();
            gpu_surface_.reset();
        }
        if (native_window_) {
            ANativeWindow_release(native_window_);
            native_window_ = nullptr;
        }
        PULP_LOGI("SurfaceRuntime: cleanup complete after failed create");
    };

    try {
        gpu_surface_ = GpuSurface::create_dawn();
        if (!gpu_surface_) {
            fail("failed to create Dawn GpuSurface");
            return;
        }

        GpuSurface::Config config;
        config.width = static_cast<uint32_t>(ANativeWindow_getWidth(native_window_));
        config.height = static_cast<uint32_t>(ANativeWindow_getHeight(native_window_));
        config.native_surface_handle = native_window_;
        config.vsync = true;

        if (!gpu_surface_->initialize(config)) {
            fail("Dawn initialization failed");
            return;
        }

        PULP_LOGI("SurfaceRuntime: Dawn initialized (%ux%u)", config.width, config.height);
        const auto info = gpu_surface_->adapter_info();
        PULP_LOGI("SurfaceRuntime: adapter=%s backend=%s", info.name.c_str(), info.backend.c_str());

        SkiaSurface::Config skia_config;
        skia_config.width = config.width;
        skia_config.height = config.height;
        skia_config.scale_factor = scale_factor;
        skia_surface_ = SkiaSurface::create(*gpu_surface_, skia_config);
        if (skia_surface_ && skia_surface_->is_available())
            PULP_LOGI("SurfaceRuntime: Skia Graphite context created");
        else
            PULP_LOGW("SurfaceRuntime: Skia Graphite failed — Dawn-only mode");
    } catch (const std::exception& e) {
        PULP_LOGE("SurfaceRuntime: GPU setup exception: %s", e.what());
        fail("GPU setup exception cleanup");
        return;
    } catch (...) {
        PULP_LOGE("SurfaceRuntime: GPU setup exception (unknown)");
        fail("GPU setup exception cleanup");
        return;
    }

    {
        std::lock_guard lock(mutex_);
        surface_valid_ = true;
    }
    PULP_LOGI("SurfaceRuntime: surface ready");
    start_render_loop();
}

void SurfaceRuntime::surface_resized(int width, int height) {
    std::unique_lock lock(mutex_);
    if (!surface_valid_ || !gpu_surface_ || !gpu_surface_->is_initialized()) return;
    frame_cv_.wait(lock, [this] { return active_frames_ == 0; });
    gpu_surface_->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    PULP_LOGI("SurfaceRuntime: resized to %dx%d", width, height);
}

void SurfaceRuntime::surface_destroyed() {
    stop_render_loop();

    {
        std::unique_lock lock(mutex_);
        if (!surface_valid_ && !native_window_ && !gpu_surface_ && !skia_surface_)
            return;
        PULP_LOGI("SurfaceRuntime: destroy begin");
        surface_valid_ = false;
        frame_cv_.wait(lock, [this] { return active_frames_ == 0; });
        // Skia holds Dawn resources, so it must be released first.
        skia_surface_.reset();
        gpu_surface_.reset();
    }

    if (native_window_) {
        ANativeWindow_release(native_window_);
        native_window_ = nullptr;
    }
    PULP_LOGI("SurfaceRuntime: destroy complete");
}

bool SurfaceRuntime::begin_frame() {
    std::lock_guard lock(mutex_);
    if (!surface_valid_ || !gpu_surface_ || !gpu_surface_->is_initialized()) return false;
    ++active_frames_;
    if (gpu_surface_->begin_frame()) return true;
    --active_frames_;
    frame_cv_.notify_all();
    return false;
}

void SurfaceRuntime::end_frame() {
    // A matching begin_frame() pins the GPU owner until this call returns.
    GpuSurface* gpu = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (active_frames_ == 0 || !gpu_surface_) return;
        gpu = gpu_surface_.get();
    }
    gpu->end_frame();
    {
        std::lock_guard lock(mutex_);
        --active_frames_;
        if (active_frames_ == 0) frame_cv_.notify_all();
    }
}

GpuSurface* SurfaceRuntime::gpu_surface() const {
    return gpu_surface_.get();
}

SkiaSurface* SurfaceRuntime::skia_surface() const {
    return skia_surface_.get();
}

AndroidGpuAdapterIdentity SurfaceRuntime::adapter_identity() const {
    std::lock_guard lock(mutex_);
    AndroidGpuAdapterIdentity identity;
    if (!gpu_surface_ || !gpu_surface_->is_initialized()) return identity;

    const auto info = gpu_surface_->adapter_info();
    if (!info.available) return identity;
    identity.available = true;
    identity.name = info.name;
    identity.vendor = info.vendor;
    identity.driver = info.description;
    return identity;
}

} // namespace pulp::render

#endif // __ANDROID__
