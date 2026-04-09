#if defined(__ANDROID__)

#include <android/log.h>
#include <cstdint>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)

// ADPF requires API 31+. Since we build with minSdk 26 but use weak linking
// (ANDROID_WEAK_API_DEFS), we check availability at runtime via dlsym.
// This avoids compile-time API level issues.

#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

namespace pulp::audio {

// Function pointers for ADPF APIs (loaded at runtime via dlsym)
using PFN_getManager = void* (*)();
using PFN_createSession = void* (*)(void*, const int32_t*, size_t, int64_t);
using PFN_reportDuration = void (*)(void*, int64_t);
using PFN_updateTarget = void (*)(void*, int64_t);
using PFN_closeSession = void (*)(void*);

class AdpfHintSession {
public:
    bool init(int64_t target_duration_ns) {
        // Try to load ADPF functions at runtime
        fn_getManager_ = reinterpret_cast<PFN_getManager>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_getManager"));
        fn_createSession_ = reinterpret_cast<PFN_createSession>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_createSession"));
        fn_reportDuration_ = reinterpret_cast<PFN_reportDuration>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_reportActualWorkDuration"));
        fn_updateTarget_ = reinterpret_cast<PFN_updateTarget>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_updateTargetWorkDuration"));
        fn_closeSession_ = reinterpret_cast<PFN_closeSession>(
            dlsym(RTLD_DEFAULT, "APerformanceHint_closeSession"));

        if (!fn_getManager_ || !fn_createSession_ || !fn_reportDuration_ || !fn_closeSession_) {
            PULP_LOGW("ADPF: Not available on this device (API < 31 or unsupported)");
            return false;
        }

        void* manager = fn_getManager_();
        if (!manager) {
            PULP_LOGW("ADPF: getManager returned null");
            return false;
        }

        target_duration_ns_ = target_duration_ns;
        int32_t tids[] = { static_cast<int32_t>(gettid()) };
        session_ = fn_createSession_(manager, tids, 1, target_duration_ns_);

        if (!session_) {
            PULP_LOGW("ADPF: Failed to create hint session");
            return false;
        }

        PULP_LOGI("ADPF: Hint session created (target=%lldns = %.1fms)",
                  static_cast<long long>(target_duration_ns_),
                  target_duration_ns_ / 1e6);
        return true;
    }

    void report_actual_duration(int64_t actual_ns) {
        if (session_ && fn_reportDuration_)
            fn_reportDuration_(session_, actual_ns);
    }

    void update_target(int64_t new_target_ns) {
        if (session_ && fn_updateTarget_) {
            target_duration_ns_ = new_target_ns;
            fn_updateTarget_(session_, new_target_ns);
        }
    }

    ~AdpfHintSession() {
        if (session_ && fn_closeSession_)
            fn_closeSession_(session_);
    }

    AdpfHintSession() = default;
    AdpfHintSession(const AdpfHintSession&) = delete;
    AdpfHintSession& operator=(const AdpfHintSession&) = delete;

    bool is_active() const { return session_ != nullptr; }

    static int64_t calculate_target_ns(int32_t buffer_size, int32_t sample_rate) {
        return static_cast<int64_t>(buffer_size) * 1'000'000'000LL / sample_rate;
    }

private:
    void* session_ = nullptr;
    int64_t target_duration_ns_ = 0;

    PFN_getManager fn_getManager_ = nullptr;
    PFN_createSession fn_createSession_ = nullptr;
    PFN_reportDuration fn_reportDuration_ = nullptr;
    PFN_updateTarget fn_updateTarget_ = nullptr;
    PFN_closeSession fn_closeSession_ = nullptr;
};

} // namespace pulp::audio

#endif // __ANDROID__
