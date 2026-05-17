// motion_bridge.h — C ABI for the Android JNI motion bridge.
//
// Declares the `pulp_motion_*` C entry points implemented in
// `core/platform/src/android/jni_motion.cpp`. The Kotlin facade in
// `com.pulp.motion.PulpMotionNative` reaches these via `external fun`
// (through the matching `Java_com_pulp_motion_PulpMotionNative_*` JNI
// shims). The C symbols are also reachable directly from C/C++ tests
// (`test/test_motion_android_bridge.cpp`) so we can exercise the
// bridge logic without an NDK build.
//
// The signatures intentionally mirror `apple/Sources/PulpSwift/PulpBridge.h`
// — same shapes, same order, no timestamp argument. See the file-level
// comment in `jni_motion.cpp` for the deadlock-safety rationale on
// register/detach.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// True when `motion::Coordinator::tracing_enabled()` is true. Cheap;
/// safe to call per-frame from the Kotlin side as a gate.
bool pulp_motion_tracing_enabled(void);

/// Single-scalar publish (mirrors `motion::publish_value`).
/// `view` and `metric` must be NUL-terminated UTF-8. `epsilon` and
/// `precision` follow `motion::PublishOptions`.
void pulp_motion_publish_value(const char* view,
                               const char* metric,
                               double value,
                               double epsilon,
                               int precision);

/// Multi-component publish (mirrors `motion::publish_components`).
/// `keys[i]` pairs with `values[i]` for `count` entries. Components are
/// sorted by name inside the coordinator.
void pulp_motion_publish_components(const char* view,
                                    const char* metric,
                                    const char* const* keys,
                                    const double* values,
                                    int count,
                                    double epsilon,
                                    int precision);

/// Set the process-wide ambient provenance envelope.
void pulp_motion_set_ambient_provenance(const char* kind,
                                        const char* id,
                                        const char* file,
                                        int line);

/// Clear the ambient provenance slot.
void pulp_motion_clear_ambient_provenance(void);

/// Register a geometry trace for `view_name`. Returns a positive
/// trace_id on success, or 0 if tracing is disabled / registration
/// failed. `fps` mirrors `motion::TraceOptions::fps`.
int pulp_motion_register_geometry_trace(const char* view_name, int fps);

/// Push a new geometry frame for a previously registered trace.
/// `metric_name` is the per-rect label (e.g. "frame"). Coordinates are
/// in the view's root / window space — they ride straight through as
/// `minX` / `minY` / `width` / `height` components.
void pulp_motion_update_geometry(int trace_id,
                                 const char* metric_name,
                                 double minX,
                                 double minY,
                                 double width,
                                 double height);

/// Detach a trace previously registered via
/// `pulp_motion_register_geometry_trace`. Idempotent.
void pulp_motion_detach_trace(int trace_id);

#ifdef __cplusplus
}
#endif
