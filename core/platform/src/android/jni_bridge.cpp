#if defined(__ANDROID__)

#include <pulp/audio/audio_focus.hpp>
#include <pulp/platform/android/jni.hpp>
#include <pulp/platform/environment.hpp>
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
        // Publish to the unified environment notifier (#342). Callers
        // subscribed via Environment::subscribe receive the new lifecycle
        // state and can resume rendering / restore GPU caches.
        auto s = pulp::platform::Environment::instance().snapshot();
        s.lifecycle = pulp::platform::LifecycleState::foreground;
        pulp::platform::Environment::instance().publish(s);
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
        // Publish via the unified environment notifier (#342). Callers
        // reduce rendering / drop optional caches in response.
        auto s = pulp::platform::Environment::instance().snapshot();
        s.lifecycle = pulp::platform::LifecycleState::background;
        pulp::platform::Environment::instance().publish(s);
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
        // Translate Android onTrimMemory level into Pulp's coarser
        // MemoryPressure tiers. The Android level values are stable
        // public API constants on ComponentCallbacks2:
        //   TRIM_MEMORY_COMPLETE   = 80  (critical — next to be killed)
        //   TRIM_MEMORY_MODERATE   = 60  (significant — foreground at risk)
        //   TRIM_MEMORY_BACKGROUND = 40  (background — start releasing)
        //   TRIM_MEMORY_UI_HIDDEN  = 20  (UI no longer visible)
        //   TRIM_MEMORY_RUNNING_*   (5/10/15 — foreground running low)
        namespace pp = pulp::platform;
        auto pressure = pp::MemoryPressure::normal;
        if (level >= 80) {
            pressure = pp::MemoryPressure::critical;
        } else if (level >= 40) {
            pressure = pp::MemoryPressure::moderate;
        } else if (level >= 10) {
            // RUNNING_LOW / RUNNING_CRITICAL on a still-foreground app.
            pressure = pp::MemoryPressure::moderate;
        }
        auto s = pp::Environment::instance().snapshot();
        s.memory_pressure = pressure;
        pp::Environment::instance().publish(s);
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
        // Publish display + color scheme together (#342). width/height
        // from the Kotlin side arrive as CSS-pixel equivalents: Android
        // reports physical pixels, so we divide by density for the
        // logical size and keep physical_* for the raw resolution.
        namespace pp = pulp::platform;
        auto s = pp::Environment::instance().snapshot();
        s.display.physical_width  = static_cast<int>(width);
        s.display.physical_height = static_cast<int>(height);
        s.display.scale = density > 0.0f ? static_cast<float>(density) : 1.0f;
        s.display.width  = s.display.scale > 0.0f
            ? static_cast<float>(width)  / s.display.scale : static_cast<float>(width);
        s.display.height = s.display.scale > 0.0f
            ? static_cast<float>(height) / s.display.scale : static_cast<float>(height);
        s.color_scheme = darkMode ? pp::ColorScheme::dark
                                  : pp::ColorScheme::light;
        pp::Environment::instance().publish(s);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnDisplayChanged");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnOrientationChanged(
    JNIEnv* env, jobject thiz, jint orientation) {
    try {
        PULP_LOGI("nativeOnOrientationChanged: %d", orientation);
        // Kotlin side's orientationToEnum() maps Android's
        // Configuration.ORIENTATION_* into Pulp's Orientation enum
        // (declared in environment.hpp). The mapping is documented
        // there; this is just a numeric pass-through.
        namespace pp = pulp::platform;
        auto s = pp::Environment::instance().snapshot();
        s.orientation = static_cast<pp::Orientation>(orientation);
        pp::Environment::instance().publish(s);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnOrientationChanged");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnSafeAreaChanged(
    JNIEnv* env, jobject thiz,
    jfloat top, jfloat bottom, jfloat left, jfloat right) {
    try {
        PULP_LOGI("nativeOnSafeAreaChanged: top=%.1f bottom=%.1f left=%.1f right=%.1f",
                  top, bottom, left, right);
        namespace pp = pulp::platform;
        auto s = pp::Environment::instance().snapshot();
        s.safe_area.top    = static_cast<float>(top);
        s.safe_area.bottom = static_cast<float>(bottom);
        s.safe_area.left   = static_cast<float>(left);
        s.safe_area.right  = static_cast<float>(right);
        pp::Environment::instance().publish(s);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnSafeAreaChanged");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_PulpActivity_nativeOnKeyboardChanged(
    JNIEnv* env, jobject thiz, jfloat bottom) {
    try {
        PULP_LOGI("nativeOnKeyboardChanged: bottom=%.1f", bottom);
        namespace pp = pulp::platform;
        auto s = pp::Environment::instance().snapshot();
        s.keyboard.bottom = static_cast<float>(bottom);
        // Android's basic WindowInsetsCompat doesn't surface animation
        // duration here — that lives on WindowInsetsAnimationCompat,
        // which is a frame-by-frame callback API. Leave 0 until the
        // animation plumbing is added in a follow-up PR.
        s.keyboard.animation_duration = 0.0f;
        pp::Environment::instance().publish(s);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnKeyboardChanged");
    }
}

// ── Audio Focus JNI Callbacks ─────────────────────────────────────────────

// #334: forward AudioManager.OnAudioFocusChangeListener events into the
// cross-platform AudioFocusRegistry. Subscribers (OboeDevice, standalone
// adapter, tooling) react without caring whether the signal came from JNI,
// AVAudioSession, or a desktop stub — same observer pattern as Environment.

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_audio_PulpAudioFocus_nativeOnAudioFocusLost(JNIEnv*, jobject) {
    PULP_LOGI("Audio focus lost");
    pulp::audio::AudioFocusRegistry::instance().publish(
        pulp::audio::AudioFocusState::lost);
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
