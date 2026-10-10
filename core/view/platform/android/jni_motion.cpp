// jni_motion.cpp — Android JNI bridge for `pulp::view::motion`.
//
// Mirrors the Swift C ABI in apple/Sources/PulpSwift/PulpBridge.cpp so a
// Kotlin / Compose / View probe sees the same shape an iOS AUv3 editor
// does (Path H of the motion skill, sibling to Path G).
//
// Two layers live in this file:
//
//   1. A C ABI (`pulp_motion_*`) that mirrors PulpBridge.h. The Kotlin
//      facade speaks this directly via `external fun` — `Java_*` JNI
//      shims forward into the same functions, so the bridge logic is
//      single-source for both the C++ test target and the Kotlin
//      runtime.
//
//   2. `Java_com_pulp_motion_PulpMotionNative_*` JNI exports the
//      Kotlin facade declares as `external fun`. Every export is
//      wrapped in try/catch so a C++ exception cannot crash the JVM.
//
// Off-by-default contract: every entry point short-circuits when
// `motion::Coordinator::tracing_enabled()` is false, so the bridge is
// cheap to leave linked into production builds.
//
// Deadlock-safety: the geometry registry stores the `TraceHandle` in a
// sibling struct, NOT inside the `shared_ptr` the sampler lambdas
// capture. `Coordinator::reset()` holds the Coordinator mutex while
// destructing samplers; letting that destruction release a TraceHandle
// would trigger a recursive `Coordinator::detach()` and self-deadlock.
// Matches the fix used in PulpBridge.cpp.

#if defined(__ANDROID__)
#include <jni.h>
#include <android/log.h>
#define PULP_MOTION_HAVE_JNI 1
#else
#define PULP_MOTION_HAVE_JNI 0
#endif

#include <pulp/view/motion.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if PULP_MOTION_HAVE_JNI
#define PULP_MOTION_TAG "PulpMotion"
#define PULP_MOTION_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  PULP_MOTION_TAG, __VA_ARGS__)
#define PULP_MOTION_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_MOTION_TAG, __VA_ARGS__)
#else
#define PULP_MOTION_LOGI(...) ((void)0)
#define PULP_MOTION_LOGE(...) ((void)0)
#endif

namespace {

/// Per-geometry-trace storage. The atomics carry the most-recently
/// published rect; the motion Coordinator's `multi(...)` samplers read
/// them on each FrameClock tick. The samplers capture
/// `shared_ptr<MotionGeometryState>` by value so they keep working
/// even after the registry entry is detached concurrently.
struct MotionGeometryState {
    std::atomic<double> minx{0.0};
    std::atomic<double> miny{0.0};
    std::atomic<double> width{0.0};
    std::atomic<double> height{0.0};
};

/// The owning `TraceHandle` lives in this sibling struct, intentionally
/// NOT inside the shared_ptr the lambdas capture. See file-level
/// comment for the deadlock rationale.
struct MotionGeometryEntry {
    std::shared_ptr<MotionGeometryState> state;
    pulp::view::motion::TraceHandle handle;
};

std::mutex& geometry_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<int, MotionGeometryEntry>& geometry_registry() {
    static std::unordered_map<int, MotionGeometryEntry> r;
    return r;
}

std::atomic<int>& next_geometry_token() {
    static std::atomic<int> n{1};
    return n;
}

std::string safe_string(const char* s) {
    return s ? std::string(s) : std::string();
}

} // namespace

// ── C ABI (single source of truth) ──────────────────────────────────────

extern "C" {

bool pulp_motion_tracing_enabled(void) {
    return pulp::view::motion::Coordinator::instance().tracing_enabled();
}

void pulp_motion_publish_value(const char* view,
                               const char* metric,
                               double value,
                               double epsilon,
                               int precision) {
    if (!pulp::view::motion::Coordinator::instance().tracing_enabled()) return;
    pulp::view::motion::PublishOptions opts;
    opts.epsilon = epsilon;
    opts.precision = precision;
    pulp::view::motion::publish_value(safe_string(view),
                                      safe_string(metric),
                                      value, opts);
}

void pulp_motion_publish_components(const char* view,
                                    const char* metric,
                                    const char* const* keys,
                                    const double* values,
                                    int count,
                                    double epsilon,
                                    int precision) {
    if (!pulp::view::motion::Coordinator::instance().tracing_enabled()) return;
    if (count < 0 || !keys || !values) return;
    std::vector<std::pair<std::string, double>> comps;
    comps.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        comps.emplace_back(safe_string(keys[i]), values[i]);
    }
    pulp::view::motion::PublishOptions opts;
    opts.epsilon = epsilon;
    opts.precision = precision;
    pulp::view::motion::publish_components(safe_string(view),
                                           safe_string(metric),
                                           std::move(comps), opts);
}

void pulp_motion_set_ambient_provenance(const char* kind,
                                        const char* id,
                                        const char* file,
                                        int line) {
    pulp::view::motion::Provenance p;
    p.source_kind = safe_string(kind);
    p.source_id   = safe_string(id);
    p.source_file = safe_string(file);
    p.source_line = line;
    pulp::view::motion::set_ambient_provenance(std::move(p));
}

void pulp_motion_clear_ambient_provenance(void) {
    pulp::view::motion::clear_ambient_provenance();
}

int pulp_motion_register_geometry_trace(const char* view_name, int fps) {
    auto& coord = pulp::view::motion::Coordinator::instance();
    if (!coord.tracing_enabled()) return 0;
    auto state = std::make_shared<MotionGeometryState>();

    pulp::view::motion::TraceOptions opts;
    opts.fps = fps > 0 ? fps : 30;

    using Component = pulp::view::motion::TraceBuilder::Component;
    std::vector<Component> components;
    components.emplace_back("minX",   [state] { return state->minx.load(std::memory_order_relaxed); });
    components.emplace_back("minY",   [state] { return state->miny.load(std::memory_order_relaxed); });
    components.emplace_back("width",  [state] { return state->width.load(std::memory_order_relaxed); });
    components.emplace_back("height", [state] { return state->height.load(std::memory_order_relaxed); });

    pulp::view::motion::Provenance prov;
    prov.source_kind = "android";
    prov.source_id   = safe_string(view_name);

    auto handle = coord.trace(safe_string(view_name), opts)
        .multi("frame", std::move(components), /*precision*/ 2, /*epsilon*/ 0.1)
        .with_provenance(std::move(prov))
        .attach();

    if (!handle.is_attached()) return 0;

    const int token = next_geometry_token().fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(geometry_mutex());
        MotionGeometryEntry entry;
        entry.state  = state;
        entry.handle = std::move(handle);
        geometry_registry().emplace(token, std::move(entry));
    }
    return token;
}

void pulp_motion_update_geometry(int trace_id,
                                 const char* metric_name,
                                 double minX,
                                 double minY,
                                 double width,
                                 double height) {
    std::shared_ptr<MotionGeometryState> state;
    {
        std::lock_guard<std::mutex> lock(geometry_mutex());
        auto it = geometry_registry().find(trace_id);
        if (it == geometry_registry().end()) return;
        state = it->second.state;
    }
    state->minx.store(minX, std::memory_order_relaxed);
    state->miny.store(minY, std::memory_order_relaxed);
    state->width.store(width, std::memory_order_relaxed);
    state->height.store(height, std::memory_order_relaxed);

    // Mirror the Swift bridge: when the metric is something other than
    // the registered "frame" name, route the rect through the publish
    // channel under the "android" view so out-of-band metrics still ride
    // an event stream. The registered "frame" sampler still fires on
    // the next FrameClock tick — we deliberately do NOT double-emit
    // here for `name == "frame"`.
    const std::string name = safe_string(metric_name);
    if (!name.empty() && name != "frame") {
        if (!pulp::view::motion::Coordinator::instance().tracing_enabled()) return;
        std::vector<std::pair<std::string, double>> comps;
        comps.reserve(4);
        comps.emplace_back("minX",   minX);
        comps.emplace_back("minY",   minY);
        comps.emplace_back("width",  width);
        comps.emplace_back("height", height);
        pulp::view::motion::PublishOptions opts;
        opts.epsilon = 0.1;
        opts.precision = 2;
        pulp::view::motion::publish_components(
            std::string("android"), name, std::move(comps), opts);
    }
}

void pulp_motion_detach_trace(int trace_id) {
    MotionGeometryEntry entry;
    {
        std::lock_guard<std::mutex> lock(geometry_mutex());
        auto it = geometry_registry().find(trace_id);
        if (it == geometry_registry().end()) return;
        entry = std::move(it->second);
        geometry_registry().erase(it);
    }
    // `entry` falls out of scope here. The TraceHandle destructor calls
    // Coordinator::detach(), which takes the coordinator mutex — fine,
    // because the geometry mutex is no longer held.
}

} // extern "C"

// ── JNI exports ─────────────────────────────────────────────────────────
//
// These mirror the C ABI 1:1 and live on
// `com.pulp.motion.PulpMotionNative` (an `internal object` declared in
// PulpMotionNative.kt). Every entry point is wrapped in try/catch to
// keep C++ exceptions from killing the JVM with SIGABRT.

#if PULP_MOTION_HAVE_JNI

namespace {

void throw_runtime(JNIEnv* env, const char* msg) {
    jclass cls = env->FindClass("java/lang/RuntimeException");
    if (cls) env->ThrowNew(cls, msg);
}

/// RAII helper to copy a `jstring` into a `std::string` without leaking
/// the UTF char buffer on the exception path. `nullptr` jstrings round-
/// trip to empty strings — matching `safe_string()` above.
class JniUtf8 {
public:
    JniUtf8(JNIEnv* env, jstring js) : env_(env), js_(js) {
        if (js_) raw_ = env_->GetStringUTFChars(js_, nullptr);
    }
    ~JniUtf8() { if (js_ && raw_) env_->ReleaseStringUTFChars(js_, raw_); }
    JniUtf8(const JniUtf8&) = delete;
    JniUtf8& operator=(const JniUtf8&) = delete;
    const char* c_str() const { return raw_ ? raw_ : ""; }
    std::string str() const { return raw_ ? std::string(raw_) : std::string(); }
private:
    JNIEnv* env_ = nullptr;
    jstring js_ = nullptr;
    const char* raw_ = nullptr;
};

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pulp_motion_PulpMotionNative_nativeTracingEnabled(
    JNIEnv* env, jobject /*thiz*/) {
    try {
        return pulp_motion_tracing_enabled() ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
    } catch (...) {
        throw_runtime(env, "Unknown C++ exception in nativeTracingEnabled");
    }
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_motion_PulpMotionNative_nativePublishValue(
    JNIEnv* env, jobject /*thiz*/,
    jstring viewName, jstring metricName,
    jdouble value, jdouble epsilon, jint precision) {
    try {
        JniUtf8 v(env, viewName);
        JniUtf8 m(env, metricName);
        pulp_motion_publish_value(v.c_str(), m.c_str(),
                                  value, epsilon, static_cast<int>(precision));
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
    } catch (...) {
        throw_runtime(env, "Unknown C++ exception in nativePublishValue");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_motion_PulpMotionNative_nativePublishComponents(
    JNIEnv* env, jobject /*thiz*/,
    jstring viewName, jstring metricName,
    jobjectArray keys, jdoubleArray values,
    jdouble epsilon, jint precision) {
    try {
        if (!keys || !values) return;
        const jsize key_count = env->GetArrayLength(keys);
        const jsize val_count = env->GetArrayLength(values);
        if (key_count != val_count) {
            throw_runtime(env, "nativePublishComponents: keys/values length mismatch");
            return;
        }
        if (key_count <= 0) return;

        // Snapshot strings into a stable storage so the const char**
        // we hand to the C ABI stays valid for the duration of the
        // call. We must keep the std::strings alive for that span.
        std::vector<std::string> key_storage;
        key_storage.reserve(static_cast<size_t>(key_count));
        std::vector<const char*> key_ptrs;
        key_ptrs.reserve(static_cast<size_t>(key_count));
        for (jsize i = 0; i < key_count; ++i) {
            jstring js = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
            JniUtf8 k(env, js);
            key_storage.emplace_back(k.str());
            env->DeleteLocalRef(js);
        }
        for (const auto& s : key_storage) key_ptrs.push_back(s.c_str());

        std::vector<double> val_storage(static_cast<size_t>(val_count));
        env->GetDoubleArrayRegion(values, 0, val_count, val_storage.data());

        JniUtf8 v(env, viewName);
        JniUtf8 m(env, metricName);
        pulp_motion_publish_components(v.c_str(), m.c_str(),
                                       key_ptrs.data(), val_storage.data(),
                                       static_cast<int>(key_count),
                                       epsilon, static_cast<int>(precision));
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
    } catch (...) {
        throw_runtime(env, "Unknown C++ exception in nativePublishComponents");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_motion_PulpMotionNative_nativeSetAmbientProvenance(
    JNIEnv* env, jobject /*thiz*/,
    jstring kind, jstring id, jstring file, jint line) {
    try {
        JniUtf8 k(env, kind);
        JniUtf8 i(env, id);
        JniUtf8 f(env, file);
        pulp_motion_set_ambient_provenance(k.c_str(), i.c_str(), f.c_str(),
                                           static_cast<int>(line));
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
    } catch (...) {
        throw_runtime(env, "Unknown C++ exception in nativeSetAmbientProvenance");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_motion_PulpMotionNative_nativeClearAmbientProvenance(
    JNIEnv* env, jobject /*thiz*/) {
    try {
        pulp_motion_clear_ambient_provenance();
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
    } catch (...) {
        throw_runtime(env, "Unknown C++ exception in nativeClearAmbientProvenance");
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_pulp_motion_PulpMotionNative_nativeRegisterGeometryTrace(
    JNIEnv* env, jobject /*thiz*/,
    jstring viewName, jint fps) {
    try {
        JniUtf8 v(env, viewName);
        return static_cast<jint>(
            pulp_motion_register_geometry_trace(v.c_str(),
                                                static_cast<int>(fps)));
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
    } catch (...) {
        throw_runtime(env, "Unknown C++ exception in nativeRegisterGeometryTrace");
    }
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_motion_PulpMotionNative_nativeUpdateGeometry(
    JNIEnv* env, jobject /*thiz*/,
    jint traceId, jstring metricName,
    jdouble minX, jdouble minY,
    jdouble width, jdouble height) {
    try {
        JniUtf8 m(env, metricName);
        pulp_motion_update_geometry(static_cast<int>(traceId), m.c_str(),
                                    minX, minY, width, height);
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
    } catch (...) {
        throw_runtime(env, "Unknown C++ exception in nativeUpdateGeometry");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_motion_PulpMotionNative_nativeDetachTrace(
    JNIEnv* env, jobject /*thiz*/, jint traceId) {
    try {
        pulp_motion_detach_trace(static_cast<int>(traceId));
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
    } catch (...) {
        throw_runtime(env, "Unknown C++ exception in nativeDetachTrace");
    }
}

#endif  // PULP_MOTION_HAVE_JNI
