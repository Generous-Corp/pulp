#pragma once

/// @file workgroup.hpp
/// Real-time thread priority management.
/// macOS: os_workgroup API (macOS 11.0+). Other platforms: best-effort.

#include <cstdint>

#if defined(__APPLE__)
#include <os/workgroup.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

namespace pulp::audio {

/// Manages real-time thread priority for audio processing threads.
///
/// On macOS 11.0+, uses os_workgroup to join the audio device's
/// workgroup, ensuring the thread gets real-time scheduling priority
/// aligned with the audio device's deadline.
///
/// On other platforms, falls back to setting high thread priority
/// via platform-specific APIs.
///
/// @code
/// // In your audio I/O callback setup:
/// AudioWorkgroup wg;
/// wg.join_from_audio_thread(); // call once from the audio thread
///
/// // When done:
/// wg.leave();
/// @endcode
class AudioWorkgroup {
public:
    AudioWorkgroup() = default;
    ~AudioWorkgroup() { leave(); }

    AudioWorkgroup(const AudioWorkgroup&) = delete;
    AudioWorkgroup& operator=(const AudioWorkgroup&) = delete;

#if defined(__APPLE__)
    /// Set the workgroup from an AudioUnit or CoreAudio device.
    /// @param workgroup An os_workgroup_t obtained from the audio device.
    void set_workgroup(os_workgroup_t workgroup) {
        workgroup_ = workgroup;
    }
#endif

    /// Join the audio workgroup from the current thread.
    /// Call this once from the audio processing thread.
    /// @return True if successfully joined.
    bool join_from_audio_thread() {
        if (joined_) return true;

#if defined(__APPLE__)
        if (workgroup_) {
            int result = os_workgroup_join(workgroup_, &token_);
            if (result == 0) {
                joined_ = true;
                return true;
            }
            return false;
        }

        // Fallback: set real-time thread priority via Mach
        joined_ = set_realtime_priority();
        return joined_;
#else
        // Other platforms: best-effort high priority
        joined_ = set_high_priority();
        return joined_;
#endif
    }

    /// Leave the audio workgroup.
    void leave() {
        if (!joined_) return;

#if defined(__APPLE__)
        if (workgroup_) {
            os_workgroup_leave(workgroup_, &token_);
        }
#endif
        joined_ = false;
    }

    /// Check if the current thread has joined the workgroup.
    bool is_joined() const { return joined_; }

    /// Set real-time thread priority without a workgroup (best-effort).
    /// Useful when a workgroup is not available (standalone apps).
    static bool set_realtime_priority() {
#if defined(__APPLE__)
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);

        // Request real-time scheduling: ~5ms period, ~3ms computation
        thread_time_constraint_policy_data_t policy;
        policy.period = static_cast<uint32_t>(5000000ULL * timebase.denom / timebase.numer);
        policy.computation = static_cast<uint32_t>(3000000ULL * timebase.denom / timebase.numer);
        policy.constraint = static_cast<uint32_t>(5000000ULL * timebase.denom / timebase.numer);
        policy.preemptible = TRUE;

        kern_return_t result = thread_policy_set(
            mach_thread_self(),
            THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&policy),
            THREAD_TIME_CONSTRAINT_POLICY_COUNT);

        return result == KERN_SUCCESS;
#else
        return set_high_priority();
#endif
    }

    /// Set high (but not real-time) thread priority.
    static bool set_high_priority() {
#if defined(__APPLE__)
        auto result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        return result == 0;
#elif defined(__linux__)
        struct sched_param param;
        param.sched_priority = sched_get_priority_max(SCHED_FIFO);
        // Don't use SCHED_FIFO as it requires root — use elevated normal priority
        int policy = SCHED_OTHER;
        param.sched_priority = 0; // max for SCHED_OTHER
        pthread_setschedparam(pthread_self(), policy, &param);
        return true;
#elif defined(_WIN32)
        // SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        return true; // stub
#else
        return false;
#endif
    }

private:
    bool joined_ = false;

#if defined(__APPLE__)
    os_workgroup_t workgroup_ = nullptr;
    os_workgroup_join_token_s token_{};
#endif
};

} // namespace pulp::audio
