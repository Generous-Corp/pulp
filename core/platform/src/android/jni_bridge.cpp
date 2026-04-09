#if defined(__ANDROID__)

#include <pulp/platform/android/jni.hpp>
#include <android/log.h>
#include <stdexcept>
#include <string>
#include "../../audio/platform/android/demo_synth.hpp"

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGF(...) __android_log_print(ANDROID_LOG_FATAL, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::android {

// Cached method IDs (safe to cache — they never change once the class is loaded)
namespace methods {
    jmethodID PulpActivity_nativeOnDisplayChanged = nullptr;
    jmethodID PulpActivity_nativeOnMemoryPressure = nullptr;
}

static jclass safe_cache_class(JNIEnv* env, const char* name) {
    jclass local = env->FindClass(name);
    if (!local) {
        PULP_LOGF("JNI_OnLoad: FindClass failed for %s", name);
        std::abort();
    }
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    PULP_LOGI("JNI_OnLoad: cached class %s", name);
    return global;
}

} // namespace pulp::android

// ── JNI_OnLoad ────────────────────────────────────────────────────────────
// Called by the JVM when System.loadLibrary("pulp") executes.
// This runs on the main thread with the app ClassLoader — the ONLY place
// where FindClass("com/pulp/...") works reliably. Native-spawned threads
// (audio, render, workers) use the System ClassLoader and would fail.
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    using namespace pulp::android;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    g_vm = vm;
    PULP_LOGI("JNI_OnLoad: initializing Pulp native bridge");

    // Cache all app classes as Global References.
    // These are valid from any thread for the lifetime of the process.
    classes::PulpBridge       = safe_cache_class(env, "com/pulp/bridge/PulpBridge");
    classes::PulpActivity     = safe_cache_class(env, "com/pulp/PulpActivity");

    // Optional classes — may not exist in minimal builds.
    // Use FindClass + check for null instead of aborting.
    jclass local;

    local = env->FindClass("com/pulp/audio/PulpAudioEngine");
    if (local) {
        classes::PulpAudioEngine = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        PULP_LOGI("JNI_OnLoad: cached class com/pulp/audio/PulpAudioEngine");
    }

    local = env->FindClass("com/pulp/midi/PulpMidiManager");
    if (local) {
        classes::PulpMidiManager = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        PULP_LOGI("JNI_OnLoad: cached class com/pulp/midi/PulpMidiManager");
    }

    local = env->FindClass("com/pulp/PulpFileProvider");
    if (local) {
        classes::PulpFileProvider = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        PULP_LOGI("JNI_OnLoad: cached class com/pulp/PulpFileProvider");
    }

    // Clear any pending exception from optional class lookups
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    PULP_LOGI("JNI_OnLoad: Pulp native bridge initialized");
    return JNI_VERSION_1_6;
}

// ── JNI Exports — Called from Kotlin ──────────────────────────────────────
// Every export is wrapped in try/catch to prevent C++ exceptions from
// crashing the JVM with SIGABRT.

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_bridge_PulpBridge_nativeInit(JNIEnv* env, jobject thiz, jobject context) {
    try {
        PULP_LOGI("nativeInit: Pulp engine starting");
        // Store application context for later use
        // TODO: Initialize Pulp engine subsystems
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeInit");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnForeground(JNIEnv* env, jobject thiz) {
    try {
        PULP_LOGI("nativeOnForeground");
        // TODO: Resume rendering, restore GPU caches
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnForeground");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnBackground(JNIEnv* env, jobject thiz) {
    try {
        PULP_LOGI("nativeOnBackground");
        // TODO: Reduce rendering, prepare for possible LMK
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnBackground");
    }
}

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

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnMemoryPressure(JNIEnv* env, jobject thiz, jint level) {
    try {
        PULP_LOGI("nativeOnMemoryPressure: level=%d", level);
        // TODO: Wire to on_memory_pressure() for tiered cache release
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnMemoryPressure");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnDisplayChanged(
    JNIEnv* env, jobject thiz, jint width, jint height, jfloat density, jboolean darkMode) {
    try {
        PULP_LOGI("nativeOnDisplayChanged: %dx%d density=%.1f dark=%d",
                  width, height, density, darkMode);
        // TODO: Update view hierarchy layout and theme
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnDisplayChanged");
    }
}

// ── Audio Focus JNI Callbacks ─────────────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_audio_PulpAudioFocus_nativeOnAudioFocusLost(JNIEnv*, jobject) {
    PULP_LOGI("Audio focus lost");
    // TODO: pause synth output
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_audio_PulpAudioFocus_nativeOnAudioFocusDuck(JNIEnv*, jobject) {
    PULP_LOGI("Audio focus duck");
    // TODO: reduce volume
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_audio_PulpAudioFocus_nativeOnAudioFocusGained(JNIEnv*, jobject) {
    PULP_LOGI("Audio focus gained");
    // TODO: resume synth output
}

#endif // __ANDROID__
