#pragma once

// pulp::android — JNI bridge utilities
//
// RAII wrappers for JNI references, thread-safe JNIEnv access,
// and class caching infrastructure. All com.pulp.* class lookups
// happen in JNI_OnLoad (main thread, app ClassLoader) and are cached
// as Global References for use by native-spawned threads.

#if defined(__ANDROID__)

#include <jni.h>
#include <cassert>
#include <atomic>

namespace pulp::android {

// ── Global JVM Reference ──────────────────────────────────────────────────
// Set in JNI_OnLoad, never changes. Safe to read from any thread.
inline JavaVM* g_vm = nullptr;

// ── Get JNIEnv for Current Thread ─────────────────────────────────────────
// Auto-attaches non-Java threads via AttachCurrentThread.
// NEVER cache the returned pointer across threads.
inline JNIEnv* get_env() {
    assert(g_vm && "JNI_OnLoad not called — System.loadLibrary missing?");
    JNIEnv* env = nullptr;
    auto result = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

// ── RAII Local Reference ──────────────────────────────────────────────────
// Wraps a JNI local reference that is deleted on scope exit.
template<typename T>
class LocalRef {
    JNIEnv* env_;
    T ref_;
public:
    LocalRef(JNIEnv* env, T ref) : env_(env), ref_(ref) {}
    ~LocalRef() { if (ref_) env_->DeleteLocalRef(ref_); }

    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    LocalRef(LocalRef&& other) noexcept : env_(other.env_), ref_(other.ref_) { other.ref_ = nullptr; }
    LocalRef& operator=(LocalRef&& other) noexcept {
        if (this != &other) {
            if (ref_) env_->DeleteLocalRef(ref_);
            env_ = other.env_;
            ref_ = other.ref_;
            other.ref_ = nullptr;
        }
        return *this;
    }

    T get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }
    T release() { T r = ref_; ref_ = nullptr; return r; }
};

// ── RAII Global Reference ─────────────────────────────────────────────────
// For Kotlin/Java objects that must outlive a single JNI call.
// Thread-safe: the reference itself is valid from any thread.
//
// WARNING: Android has a hard limit on Global References (~51,200).
// In debug builds, we track the count and assert if it gets too high.
class GlobalRef {
    jobject ref_ = nullptr;

#ifndef NDEBUG
    static inline std::atomic<int> g_count{0};
#endif

public:
    GlobalRef() = default;

    GlobalRef(JNIEnv* env, jobject local) {
        if (local) {
            ref_ = env->NewGlobalRef(local);
#ifndef NDEBUG
            int count = g_count.fetch_add(1) + 1;
            assert(count < 1000 && "Possible JNI Global Reference leak — over 1000 active refs");
#endif
        }
    }

    ~GlobalRef() {
        if (ref_) {
            get_env()->DeleteGlobalRef(ref_);
#ifndef NDEBUG
            g_count.fetch_sub(1);
#endif
        }
    }

    GlobalRef(const GlobalRef&) = delete;
    GlobalRef& operator=(const GlobalRef&) = delete;
    GlobalRef(GlobalRef&& other) noexcept : ref_(other.ref_) { other.ref_ = nullptr; }
    GlobalRef& operator=(GlobalRef&& other) noexcept {
        if (this != &other) {
            if (ref_) {
                get_env()->DeleteGlobalRef(ref_);
#ifndef NDEBUG
                g_count.fetch_sub(1);
#endif
            }
            ref_ = other.ref_;
            other.ref_ = nullptr;
        }
        return *this;
    }

    jobject get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

#ifndef NDEBUG
    static int active_count() { return g_count.load(); }
#endif
};

// ── RAII Float Array Pin ──────────────────────────────────────────────────
// Pins a jfloatArray for native access, releases on destruction.
class FloatArrayPin {
    JNIEnv* env_;
    jfloatArray array_;
    jfloat* data_;
public:
    FloatArrayPin(JNIEnv* env, jfloatArray array)
        : env_(env), array_(array)
        , data_(env->GetFloatArrayElements(array, nullptr)) {}

    ~FloatArrayPin() {
        if (data_) env_->ReleaseFloatArrayElements(array_, data_, 0);
    }

    FloatArrayPin(const FloatArrayPin&) = delete;
    FloatArrayPin& operator=(const FloatArrayPin&) = delete;

    jfloat* data() { return data_; }
    const jfloat* data() const { return data_; }
    explicit operator bool() const { return data_ != nullptr; }
    jsize size() const { return env_->GetArrayLength(array_); }
};

// ── Cached Class References ───────────────────────────────────────────────
// Looked up in JNI_OnLoad (main thread, app ClassLoader).
// Safe to use from any thread — they are Global References.
namespace classes {
    inline jclass PulpBridge = nullptr;
    inline jclass PulpAudioEngine = nullptr;
    inline jclass PulpMidiManager = nullptr;
    inline jclass PulpFileProvider = nullptr;
    inline jclass PulpActivity = nullptr;
}

// Cache a class as a Global Reference. Call only from JNI_OnLoad.
inline jclass cache_class(JNIEnv* env, const char* name) {
    jclass local = env->FindClass(name);
    if (!local) {
        // Fatal: class not found. App is misconfigured.
        // Can't use android logging here without <android/log.h> — just abort.
        std::abort();
    }
    jclass global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

} // namespace pulp::android

#endif // __ANDROID__
