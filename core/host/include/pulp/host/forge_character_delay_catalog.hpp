#pragma once

// Multi-character delay — bake-layer catalog node.
//
// Wraps pulp::signal::CharacterDelayT as a lowerable custom node with
// injectable baked params, following the same shape as the lo-fi kit's
// make_delay_node() / make_filter_node(). That kit's simple feedback delay
// stays exactly as it is; this is a different, larger instrument and complements
// rather than replaces it.
//
// Per-character type ids mirror the per-mode "svf" realization pattern: the
// CHARACTER is a registration-time choice, not an injectable param. A baked
// build authored as tape is tape for the artifact's life, and no control-thread
// write can turn it into a BBD mid-render. The characters differ in buffer
// topology and in which filters exist at all, so making the choice a parameter
// would mean either running all five and crossfading (five times the cost) or
// switching topology under the audio thread (a click at best).
//
// Stereo, two ports in and two out, as ONE logical stereo wire — the same
// convention the lo-fi kit's ping-pong node uses. The module is genuinely
// stereo (crossfeed, per-channel instability decorrelation, per-channel
// segment lengths in reverse), so instancing it dual-mono would silently drop
// half of what it does.
//
// WET ONLY, like the DSP: compose with make_drywet_node() for a mix control.
// This differs from the lo-fi delay node, which outputs dry + wet; it is the
// deliberate choice, because at five characters and 1.1 feedback the useful
// dry/wet balance is a per-patch decision, not a fixed sum.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/character_delay.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace pulp::host::character_delay {

using Character = signal::CharacterDelay::Character;
using TapeTier = signal::CharacterDelay::TapeTier;

// ── Stable type ids ───────────────────────────────────────────────────────
inline constexpr const char* kCleanTypeId = "delay.clean";
inline constexpr const char* kVintageTypeId = "delay.vintage";
inline constexpr const char* kTapeTypeId = "delay.tape";
inline constexpr const char* kBbdTypeId = "delay.bbd";
inline constexpr const char* kDiffusionTypeId = "delay.diffusion";
/// The physical tape tier is its own realization of the same factory: the tier
/// is a construction-time config choice, not a baked param, because switching
/// it swaps the saturation and loss STAGES rather than a coefficient.
inline constexpr const char* kTapePhysicalTypeId = "delay.tape_physical";

// ── Injectable param ids ──────────────────────────────────────────────────
// Node-local; the framework namespaces per node so two nodes never collide.
inline constexpr state::ParamID kTimeMs = 1;       // ms, log-tapered by the host
inline constexpr state::ParamID kTimeOffset = 2;   // × on the right channel's time
inline constexpr state::ParamID kFeedback = 3;     // 0–1.1, displayed 0–110%
inline constexpr state::ParamID kCrossfeed = 4;    // %
inline constexpr state::ParamID kCharacter = 5;    // %
inline constexpr state::ParamID kModRate = 6;      // %, internally log-mapped
inline constexpr state::ParamID kModDepth = 7;     // %
inline constexpr state::ParamID kDuck = 8;         // %
inline constexpr state::ParamID kFreeze = 9;       // stepped 0/1
inline constexpr state::ParamID kReverse = 10;     // stepped 0/1

/// Longest addressable delay, in ms. Matches the DSP's own maximum.
inline constexpr float kMaxTimeMs = 2000.0f;

struct CharacterDelayInstance {
    signal::CharacterDelay delay;
};

/// Stable type id for a (character, tier) pair.
inline const char* character_delay_type_id(Character character,
                                           TapeTier tier = TapeTier::standard) {
    switch (character) {
        case Character::vintage_digital: return kVintageTypeId;
        case Character::tape:
            return tier == TapeTier::physical ? kTapePhysicalTypeId : kTapeTypeId;
        case Character::bbd: return kBbdTypeId;
        case Character::diffusion: return kDiffusionTypeId;
        case Character::clean:
        default: return kCleanTypeId;
    }
}

inline const char* character_delay_default_name(Character character, TapeTier tier) {
    switch (character) {
        case Character::vintage_digital: return "Vintage Delay";
        case Character::tape:
            return tier == TapeTier::physical ? "Tape Delay (Physical)" : "Tape Delay";
        case Character::bbd: return "BBD Delay";
        case Character::diffusion: return "Diffusion Delay";
        case Character::clean:
        default: return "Clean Delay";
    }
}

/// One factory, six registered realizations.
///
/// `tape_speed_ips` only matters for the physical tier, where it drives the
/// Wallace loss geometry and the head-bump centre; like the tier itself it is a
/// construction-time config choice, because changing it redesigns a bank of
/// FIRs and that is not audio-thread work.
inline CustomNodeType make_character_delay_node(Character character,
                                                TapeTier tier = TapeTier::standard,
                                                float tape_speed_ips = 7.5f) {
    CustomNodeType t;
    t.type_id = character_delay_type_id(character, tier);
    t.version = 1;
    t.num_input_ports = 2;  // 0 = left, 1 = right (ONE logical stereo wire)
    t.num_output_ports = 2;
    t.default_name = character_delay_default_name(character, tier);
    t.lowerable = true;

    t.create = []() -> void* { return new CharacterDelayInstance{}; };
    t.destroy = [](void* p) { delete static_cast<CharacterDelayInstance*>(p); };
    t.prepare = [character, tier, tape_speed_ips](void* p, double sr, int /*max_block*/) {
        auto* s = static_cast<CharacterDelayInstance*>(p);
        s->delay.set_character(character);
        s->delay.set_tape_tier(tier);
        s->delay.set_tape_speed_ips(tape_speed_ips);
        s->delay.set_sample_rate(sr);
    };
    t.reset = [](void* p) { static_cast<CharacterDelayInstance*>(p)->delay.reset(); };

    // Ranges and defaults are the module's canonical contract; the Forge layer
    // mirrors them and is responsible for the display units and for converting
    // musical divisions to milliseconds (the DSP has no tempo input by design).
    t.baked_params.push_back({kTimeMs, 1.0f, kMaxTimeMs, 350.0f});
    t.baked_params.push_back({kTimeOffset, 0.5f, 1.5f, 1.0f});
    t.baked_params.push_back({kFeedback, 0.0f, 1.1f, 0.35f});
    t.baked_params.push_back({kCrossfeed, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kCharacter, 0.0f, 1.0f, 0.5f});
    t.baked_params.push_back({kModRate, 0.0f, 1.0f, 0.3f});
    t.baked_params.push_back({kModDepth, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kDuck, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kFreeze, 0.0f, 1.0f, 0.0f});
    t.baked_params.push_back({kReverse, 0.0f, 1.0f, 0.0f});

    t.process_instance_baked_param = [](void* p, audio::BufferView<float>& out,
                                        const audio::BufferView<const float>& in, int n,
                                        const BakedParamView& params) {
        auto* s = static_cast<CharacterDelayInstance*>(p);
        const float* in_left = in.channel_ptr(0);
        const float* in_right = in.channel_ptr(1);
        float* out_left = out.channel_ptr(0);
        float* out_right = out.channel_ptr(1);

        // One sample at a time so every knob is sample-accurate. The setters
        // are a clamp and a store — the module smooths internally and only
        // recomputes coefficients at its own control rate — so this costs an
        // inlined call per param rather than a filter redesign per sample.
        for (int k = 0; k < n; ++k) {
            const auto offset = static_cast<std::int32_t>(k);
            s->delay.set_time_ms(params.value_at(kTimeMs, offset));
            s->delay.set_time_offset(params.value_at(kTimeOffset, offset));
            s->delay.set_feedback(params.value_at(kFeedback, offset));
            s->delay.set_crossfeed(params.value_at(kCrossfeed, offset));
            s->delay.set_character_amount(params.value_at(kCharacter, offset));
            s->delay.set_mod(params.value_at(kModRate, offset),
                             params.value_at(kModDepth, offset));
            s->delay.set_duck(params.value_at(kDuck, offset));
            s->delay.set_freeze(params.value_at(kFreeze, offset) >= 0.5f);
            s->delay.set_reverse(params.value_at(kReverse, offset) >= 0.5f);

            float left = in_left[static_cast<std::size_t>(k)];
            float right = in_right[static_cast<std::size_t>(k)];
            s->delay.process(&left, &right, 1);
            out_left[static_cast<std::size_t>(k)] = left;
            out_right[static_cast<std::size_t>(k)] = right;
        }
    };
    return t;
}

}  // namespace pulp::host::character_delay
