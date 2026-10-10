#if defined(__ANDROID__)

// Audio-side JNI exports for libpulp.so.
//
// These entry points depend on pulp-audio, so they live with the audio
// sources rather than in core/platform's jni_bridge.cpp, which keeps
// pulp-platform free of audio includes. Like jni_bridge.cpp, this file is
// compiled into the pulp-jni shared library (root CMakeLists.txt), not into
// the pulp-audio archive. The exported symbol names are the JNI contract
// with the Kotlin classes and must not change.

#include <pulp/audio/audio_focus.hpp>
#include <jni.h>
#include <android/log.h>
#include <stdexcept>
#include "demo_synth.hpp"

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnShutdown(JNIEnv* env, jobject thiz) {
    try {
        PULP_LOGI("nativeOnShutdown — stopping synth");
        pulp::demo::synth_stop();
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnShutdown");
    }
}

// ── Audio Focus JNI Callbacks ─────────────────────────────────────────────

// Forward AudioManager.OnAudioFocusChangeListener events into the
// cross-platform AudioFocusRegistry. Subscribers (OboeDevice,
// standalone adapter, tooling) react without caring whether the signal
// came from JNI, AVAudioSession, or a desktop stub — same observer
// pattern as Environment.

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_audio_PulpAudioFocus_nativeOnAudioFocusLost(JNIEnv*, jobject) {
    PULP_LOGI("Audio focus lost");
    pulp::audio::AudioFocusRegistry::instance().publish(
        pulp::audio::AudioFocusState::lost);
}

// AUDIOFOCUS_LOSS_TRANSIENT has its own entry point so subscribers can
// differentiate pause-transiently (we WILL get focus back shortly) from
// duck (attenuate but keep playing).
extern "C" JNIEXPORT void JNICALL
Java_com_pulp_audio_PulpAudioFocus_nativeOnAudioFocusLostTransient(JNIEnv*, jobject) {
    PULP_LOGI("Audio focus lost transiently");
    pulp::audio::AudioFocusRegistry::instance().publish(
        pulp::audio::AudioFocusState::lost_transient);
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_audio_PulpAudioFocus_nativeOnAudioFocusDuck(JNIEnv*, jobject) {
    PULP_LOGI("Audio focus duck");
    pulp::audio::AudioFocusRegistry::instance().publish(
        pulp::audio::AudioFocusState::duck);
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_audio_PulpAudioFocus_nativeOnAudioFocusGained(JNIEnv*, jobject) {
    PULP_LOGI("Audio focus gained");
    pulp::audio::AudioFocusRegistry::instance().publish(
        pulp::audio::AudioFocusState::gained);
}

#endif // __ANDROID__
