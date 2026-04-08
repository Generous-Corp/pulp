#if defined(__ANDROID__)

#include <pulp/platform/android/jni.hpp>
#include <android/log.h>
#include <stdexcept>
#include <cmath>
#include <atomic>

// Oboe headers
#include <oboe/Oboe.h>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::demo {

// ── Simple Sine Tone Generator ────────────────────────────────────────────
// Demonstrates Oboe audio output with a pure sine wave.
// Lock-free: frequency changes via std::atomic, no mutex in callback.

class ToneGenerator : public oboe::AudioStreamDataCallback,
                      public oboe::AudioStreamErrorCallback {
public:
    bool start(float frequency_hz) {
        frequency_.store(frequency_hz, std::memory_order_relaxed);
        phase_ = 0.0;

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Mono)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Best)
            ->setDataCallback(this)
            ->setErrorCallback(this);

        auto result = builder.openManagedStream(stream_);
        if (result != oboe::Result::OK) {
            PULP_LOGI("ToneGenerator: failed to open stream: %s",
                      oboe::convertToText(result));
            return false;
        }

        sample_rate_ = stream_->getSampleRate();
        buffer_size_ = stream_->getFramesPerBurst();

        PULP_LOGI("ToneGenerator: stream opened — %d Hz, %d frames/burst",
                  sample_rate_, buffer_size_);

        result = stream_->requestStart();
        if (result != oboe::Result::OK) {
            PULP_LOGI("ToneGenerator: failed to start: %s",
                      oboe::convertToText(result));
            return false;
        }

        playing_.store(true, std::memory_order_release);
        PULP_LOGI("ToneGenerator: playing %.0f Hz", frequency_hz);
        return true;
    }

    void stop() {
        playing_.store(false, std::memory_order_release);
        if (stream_) {
            stream_->requestStop();
            stream_->close();
            stream_.reset();
        }
        PULP_LOGI("ToneGenerator: stopped");
    }

    void set_frequency(float hz) {
        frequency_.store(hz, std::memory_order_relaxed);
    }

    bool is_playing() const {
        return playing_.load(std::memory_order_acquire);
    }

    int32_t sample_rate() const { return sample_rate_; }
    int32_t buffer_size() const { return buffer_size_; }
    int64_t xrun_count() const { return xrun_count_.load(std::memory_order_relaxed); }

private:
    // Audio callback — MUST be lock-free
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* stream, void* audio_data, int32_t num_frames) override {

        if (!playing_.load(std::memory_order_acquire)) {
            // Fill with silence
            auto* out = static_cast<float*>(audio_data);
            for (int i = 0; i < num_frames; ++i) out[i] = 0.0f;
            return oboe::DataCallbackResult::Continue;
        }

        float freq = frequency_.load(std::memory_order_relaxed);
        double phase_inc = 2.0 * M_PI * freq / sample_rate_;
        auto* out = static_cast<float*>(audio_data);

        for (int i = 0; i < num_frames; ++i) {
            out[i] = static_cast<float>(std::sin(phase_)) * 0.3f;  // -10dB
            phase_ += phase_inc;
            if (phase_ > 2.0 * M_PI) phase_ -= 2.0 * M_PI;
        }

        // Track xruns
        auto xruns = stream->getXRunCount();
        if (xruns.value() > last_xruns_) {
            xrun_count_.fetch_add(xruns.value() - last_xruns_, std::memory_order_relaxed);
            last_xruns_ = xruns.value();
        }

        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override {
        PULP_LOGI("ToneGenerator: stream error — %s", oboe::convertToText(error));
        if (playing_.load(std::memory_order_acquire)) {
            // Auto-restart
            start(frequency_.load(std::memory_order_relaxed));
        }
    }

    oboe::ManagedStream stream_;
    std::atomic<float> frequency_{440.0f};
    std::atomic<bool> playing_{false};
    double phase_ = 0.0;
    int32_t sample_rate_ = 48000;
    int32_t buffer_size_ = 256;
    std::atomic<int64_t> xrun_count_{0};
    int32_t last_xruns_ = 0;
};

static ToneGenerator g_tone;

} // namespace pulp::demo

// ── JNI Exports ───────────────────────────────────────────────────────────

extern "C" {

JNIEXPORT void JNICALL
Java_com_pulp_PulpActivityKt_nativeStartTone(JNIEnv* env, jclass, jfloat freq) {
    try {
        pulp::demo::g_tone.start(freq);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown exception in nativeStartTone");
    }
}

JNIEXPORT void JNICALL
Java_com_pulp_PulpActivityKt_nativeStopTone(JNIEnv* env, jclass) {
    try {
        pulp::demo::g_tone.stop();
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown exception in nativeStopTone");
    }
}

JNIEXPORT void JNICALL
Java_com_pulp_PulpActivityKt_nativeSetFrequency(JNIEnv*, jclass, jfloat freq) {
    pulp::demo::g_tone.set_frequency(freq);
}

JNIEXPORT jboolean JNICALL
Java_com_pulp_PulpActivityKt_nativeIsPlaying(JNIEnv*, jclass) {
    return pulp::demo::g_tone.is_playing();
}

JNIEXPORT jint JNICALL
Java_com_pulp_PulpActivityKt_nativeGetSampleRate(JNIEnv*, jclass) {
    return pulp::demo::g_tone.sample_rate();
}

JNIEXPORT jint JNICALL
Java_com_pulp_PulpActivityKt_nativeGetBufferSize(JNIEnv*, jclass) {
    return pulp::demo::g_tone.buffer_size();
}

JNIEXPORT jlong JNICALL
Java_com_pulp_PulpActivityKt_nativeGetXrunCount(JNIEnv*, jclass) {
    return pulp::demo::g_tone.xrun_count();
}

} // extern "C"

#endif // __ANDROID__
