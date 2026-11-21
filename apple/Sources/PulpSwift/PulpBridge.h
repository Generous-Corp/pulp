// PulpBridge.h — C interface for Swift interop
// Swift can call these C functions directly without Objective-C bridging.
// This is the narrow bridge between the C++ core and Swift UI layer.

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Pull in the iOS AVAudioSession C ABI so Swift (PulpAudioSession.swift)
// can call pulp_ios_audio_session_emit() directly through this umbrella
// header. Underlying declarations live with the format module so C++
// consumers keep a clean <pulp/format/ios_audio_session.h> include path.
#include <pulp/format/ios_audio_session.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Parameter access ────────────────────────────────────────────────────

typedef uint32_t PulpParamID;

typedef struct {
    PulpParamID id;
    const char* name;
    const char* unit;
    float min_value;
    float max_value;
    float default_value;
    float step;
} PulpParamInfo;

/// Get the number of registered parameters.
int pulp_param_count(void);

/// Get parameter info by index (0-based). Returns false if out of range.
bool pulp_param_info(int index, PulpParamInfo* out_info);

/// Get a parameter's current value.
float pulp_param_get(PulpParamID id);

/// Set a parameter's value.
void pulp_param_set(PulpParamID id, float value);

/// Get a parameter's normalized value (0-1).
float pulp_param_get_normalized(PulpParamID id);

/// Set a parameter's normalized value (0-1).
void pulp_param_set_normalized(PulpParamID id, float normalized);

/// Begin a parameter gesture (for undo grouping).
void pulp_param_begin_gesture(PulpParamID id);

/// End a parameter gesture.
void pulp_param_end_gesture(PulpParamID id);

/// Reset a parameter to its default value.
void pulp_param_reset(PulpParamID id);

// ── Plugin info ─────────────────────────────────────────────────────────

typedef struct {
    const char* name;
    const char* manufacturer;
    const char* version;
    const char* bundle_id;
    int category;  // 0=Effect, 1=Instrument, 2=MidiEffect
    bool accepts_midi;
    bool produces_midi;
} PulpPluginInfo;

/// Get the plugin descriptor.
bool pulp_plugin_info(PulpPluginInfo* out_info);

// ── State ───────────────────────────────────────────────────────────────

/// Serialize current state. Caller must free the returned buffer with pulp_free().
/// Returns NULL on failure. Sets *out_size to the data length.
uint8_t* pulp_state_serialize(int* out_size);

/// Deserialize state from a buffer. Returns true on success.
bool pulp_state_deserialize(const uint8_t* data, int size);

/// Free a buffer allocated by pulp_state_serialize().
void pulp_free(void* ptr);

// ── Motion observability ────────────────────────────────────────────────
//
// Swift bridge for `vellum::view::motion` in the pulp-vellum integration.
// Mirrors the publish channel + ambient-provenance API for SwiftUI / UIKit /
// AppKit callers (Pulp's iOS AUv3 editors, Swift host apps, `PulpSwift/`).
//
// All entry points are no-ops when the process-wide motion Coordinator has
// tracing disabled (the default). The Swift facade is expected to gate hot
// paths on `pulp_motion_tracing_enabled()` to avoid even the string-copy
// cost when motion is off.

/// True when `motion::Coordinator::tracing_enabled()` is true. Cheap;
/// safe to call per-frame from the Swift side as a gate.
bool pulp_motion_tracing_enabled(void);

/// Single-scalar publish (mirrors `motion::publish_value`).
/// `view` and `metric` must be NUL-terminated UTF-8. `epsilon` and `precision`
/// follow `motion::PublishOptions`.
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

/// Set the process-wide ambient provenance envelope. Subsequent publishes
/// with an empty `PublishOptions::provenance` will be stamped with this.
/// Pass NULL strings to clear individual fields.
void pulp_motion_set_ambient_provenance(const char* kind,
                                        const char* id,
                                        const char* file,
                                        int line);

/// Clear the ambient provenance slot.
void pulp_motion_clear_ambient_provenance(void);

/// Register a geometry trace for `view_name`. Returns a positive trace_id
/// on success, or 0 if tracing is disabled / registration failed. The
/// Swift-side probe should call `pulp_motion_update_geometry()` whenever
/// the underlying view's frame changes, and `pulp_motion_detach_trace()`
/// on dismantle. `fps` mirrors `motion::TraceOptions::fps`.
int pulp_motion_register_geometry_trace(const char* view_name, int fps);

/// Push a new geometry frame for a trace registered via
/// `pulp_motion_register_geometry_trace`. The coordinator samples the
/// most recently-published rect at each FrameClock tick.
/// `metric_name` is the per-rect label (e.g. "frame"). Coordinates are in
/// the view's root / window space — they ride straight through as
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
