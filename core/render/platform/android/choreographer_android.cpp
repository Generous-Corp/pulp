#if defined(__ANDROID__)

#include <android/choreographer.h>
#include <android/looper.h>
#include <android/log.h>
#include <atomic>
#include <functional>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::render {

// ── AChoreographer Frame Pacing ───────────────────────────────────────────
// Synchronizes Dawn/Skia frame rendering with the device's VSYNC signal.
// Prevents stutter on 90Hz/120Hz/VRR displays and avoids wasting GPU
// cycles rendering frames that won't be displayed.
//
// Must be called from a thread with an ALooper attached.

class ChoreographerFramePacer {
public:
    using FrameCallback = std::function<void(int64_t frame_time_nanos)>;

    bool init() {
        // AChoreographer_getInstance() requires a thread with an ALooper
        choreographer_ = AChoreographer_getInstance();
        if (!choreographer_) {
            PULP_LOGI("AChoreographer not available — using manual frame pacing");
            return false;
        }
        PULP_LOGI("AChoreographer frame pacing initialized");
        return true;
    }

    void set_render_callback(FrameCallback cb) {
        render_callback_ = std::move(cb);
    }

    void request_frame() {
        if (choreographer_ && !frame_requested_.exchange(true)) {
            // AChoreographer_postFrameCallback (API 24+) uses long, not int64_t
            AChoreographer_postFrameCallback(choreographer_, on_vsync, this);
        }
    }

    void stop() {
        running_.store(false);
    }

    bool is_running() const {
        return running_.load();
    }

private:
    static void on_vsync(long frame_time_nanos, void* data) {
        auto* self = static_cast<ChoreographerFramePacer*>(data);
        self->frame_requested_.store(false);

        if (!self->running_.load()) return;

        if (self->render_callback_) {
            self->render_callback_(frame_time_nanos);
        }

        // Request next frame (continuous rendering loop)
        if (self->running_.load()) {
            self->request_frame();
        }
    }

    AChoreographer* choreographer_ = nullptr;
    FrameCallback render_callback_;
    std::atomic<bool> frame_requested_{false};
    std::atomic<bool> running_{true};
};

} // namespace pulp::render

#endif // __ANDROID__
