// PulpBridge.h — C interface for Swift interop
// Swift can call these C functions directly without Objective-C bridging.
// This is the narrow bridge between the C++ core and Swift UI layer.

#pragma once

#include <stdint.h>
#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
