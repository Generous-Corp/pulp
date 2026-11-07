#pragma once

// Control-side, value-owned G-4 voice-bank template and binding schema.  This
// surface declares authored slot/port/parameter identity and persistence only;
// it does not create graph nodes, schedule events, or implement voice runtime.

#include <pulp/host/voice_identity.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::host {

enum class VoiceBindingTarget : std::uint8_t { Port = 0, Parameter = 1 };
enum class VoiceBindingType : std::uint8_t { Audio = 0, Event = 1, Control = 2 };
enum class VoiceBindingDirection : std::uint8_t { Input = 0, Output = 1 };
enum class VoiceBindingShape : std::uint8_t { Scalar = 0, Mono = 1, Stereo = 2 };
enum class VoiceBindingRate : std::uint8_t { Block = 0, Sample = 1, Event = 2 };

/// One authored binding relative to a voice slot.  Path segments are stable
/// authored names; they are never runtime node IDs or allocator indexes.
struct VoiceBindingSpec {
    std::vector<std::string> path;
    std::string target;
    VoiceBindingTarget target_kind = VoiceBindingTarget::Parameter;
    VoiceBindingType type = VoiceBindingType::Control;
    VoiceBindingDirection direction = VoiceBindingDirection::Input;
    VoiceBindingShape shape = VoiceBindingShape::Scalar;
    VoiceBindingRate rate = VoiceBindingRate::Block;
    // A binding shared by slots would permit one slot to mutate another.  The
    // first bank schema admits only isolated per-slot bindings.
    bool shared_across_slots = false;
    std::uint32_t ordinal = 0; // meaningful for port targets
};

struct VoiceTemplateSchema {
    std::string template_namespace;
    std::string template_name;
    std::uint32_t template_revision = 0;
    std::uint32_t bank_schema_version = 1;
    std::vector<VoiceBindingSpec> bindings;
};

struct VoiceSlotBinding {
    std::uint32_t slot_ordinal = 0;
    VoiceBindingSpec spec;
    VoiceIdentityResult identity;
};

struct VoiceBankManifest {
    VoiceTemplateSchema schema;
    std::vector<VoiceSlotBinding> bindings; // exactly eight copies per declaration
    std::string sha256;
};

struct VoiceBankPrepareResult {
    bool valid = false;
    std::uint32_t failing_slot = 0;
    std::string failing_path;
    std::string error;
    VoiceBankManifest manifest;
};

/// Validate and deterministically expand a template into the eight-slot bank.
/// Every failure occurs before a manifest is returned, so a caller can retain
/// its previously published bank unchanged.
VoiceBankPrepareResult prepare_voice_bank(const VoiceTemplateSchema& schema);

struct VoiceParameterValue {
    std::uint32_t slot_ordinal = 0;
    std::vector<std::string> path;
    std::string target;
    float value = 0.0f;
};

/// Persistent state is deliberately limited to declared template identity and
/// finite per-slot parameter values.  Active notes, generations, pedals,
/// epochs, queues and release tails are not representable in this schema.
struct VoiceBankState {
    std::uint32_t schema_version = 1;
    std::string manifest_sha256;
    std::vector<VoiceParameterValue> parameters;
};

struct VoiceBankPersistenceResult {
    bool valid = false;
    std::vector<std::uint8_t> encoding;
    std::string sha256;
    std::string error;
};

/// Encode state in a deterministic tagged, length-delimited wire form.  The
/// manifest is checked first; malformed or stale state produces no bytes.
VoiceBankPersistenceResult encode_voice_bank_state(const VoiceBankManifest& manifest,
                                                   const VoiceBankState& state);

/// Transactionally validate and restore state.  `destination` is untouched on
/// any failure, including a mismatched manifest, unsupported migration, alias,
/// duplicate, or non-finite value.
bool restore_voice_bank_state(const VoiceBankManifest& manifest, const VoiceBankState& candidate,
                              VoiceBankState& destination, std::string* error = nullptr);

} // namespace pulp::host
