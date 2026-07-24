#pragma once

/// @file midi_parameter_map.hpp
/// Map incoming MIDI Control Change messages to plugin parameters, with a
/// "learn" mode — the equivalent of a host's MIDI-mapping panel, owned by
/// the plugin so it works in every host.
///
/// Thread model: mappings are added / armed from the UI thread and applied
/// from the audio thread. UI calls go through a lock-free command queue;
/// the audio thread drains it once per block via `pump()` and then routes
/// each incoming CC through `handle_cc()`, which writes the parameter with
/// `set_normalized_rt`. The value change is picked up by the format
/// adapter's post-process diff and pushed to the host, so a CC-driven move
/// is recorded as parameter automation.
///
/// Value scaling: a controller need not drive its parameter across the whole
/// range. Each mapping carries a `MidiMapScale` — the normalized [0, 1] window
/// the CC sweep is mapped onto. CC 0..127 spans `min`..`max`; the endpoints
/// are clamped into [0, 1], and `min > max` inverts the response (a knob turned
/// up lowers the parameter). The default window is the full range, so a mapping
/// created without a scale behaves exactly as an unscaled 0..127 → 0..1 sweep.
/// Shaping (log/skew) is a property of the target parameter's own `ParamRange`,
/// not of the map — the map only supplies the normalized window.
///
/// Usage (in the processor):
///   midi_map_.pump();                       // top of process()
///   for (auto& e : midi_in)
///     if (e.is_cc())
///       midi_map_.handle_cc(state(), e.channel(), e.cc_number(), e.cc_value());
/// And from the UI:
///   midi_map_.arm_learn(kCutoff);           // next CC binds to kCutoff
///   midi_map_.set_mapping(0, 74, kCutoff);  // or map explicitly
///   midi_map_.set_mapping(0, 1, kMix, {0.25f, 0.75f}); // sweep only the middle
///   midi_map_.set_mapping(0, 7, kGain, {1.0f, 0.0f});  // inverted

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/state/store.hpp>

namespace pulp::state {

/// Normalized [0, 1] output window a CC sweep is mapped onto. `min > max`
/// inverts. Defaults to the full range, i.e. an identity CC → normalized map.
struct MidiMapScale {
    float min = 0.0f;
    float max = 1.0f;

    /// Map a raw 7-bit CC value onto this window. The result is always within
    /// [0, 1] because both endpoints are clamped there on insertion.
    float apply(uint8_t value) const {
        return min + (value / 127.0f) * (max - min);
    }
};

class MidiParameterMap {
  public:
    /// Match any MIDI channel.
    static constexpr uint8_t kOmni = 0xFF;

    // ── UI thread ──

    /// Map (channel, cc) → parameter across the full range. `channel` may be kOmni.
    void set_mapping(uint8_t channel, uint8_t cc, ParamID id) {
        set_mapping(channel, cc, id, MidiMapScale{});
    }

    /// Map (channel, cc) → parameter, sweeping only `scale`'s normalized window.
    void set_mapping(uint8_t channel, uint8_t cc, ParamID id, MidiMapScale scale) {
        commands_.try_push({Command::Set, channel, cc, id, clamp_scale(scale)});
    }

    /// Arm learn: the next incoming CC binds to `id` across the full range.
    void arm_learn(ParamID id) {
        arm_learn(id, MidiMapScale{});
    }

    /// Arm learn: the next incoming CC binds to `id` with `scale`'s window.
    void arm_learn(ParamID id, MidiMapScale scale) {
        commands_.try_push({Command::Arm, kOmni, 0, id, clamp_scale(scale)});
    }

    /// Remove any mapping that targets `id`.
    void clear(ParamID id) {
        commands_.try_push({Command::Clear, kOmni, 0, id, MidiMapScale{}});
    }

    // ── audio thread ──

    /// Drain queued UI commands. Call once at the top of `process()`.
    void pump() {
        while (auto cmd = commands_.try_pop()) {
            switch (cmd->type) {
            case Command::Set:
                insert(cmd->channel, cmd->cc, cmd->id, cmd->scale);
                break;
            case Command::Arm:
                learn_armed_ = true;
                learn_target_ = cmd->id;
                learn_scale_ = cmd->scale;
                break;
            case Command::Clear:
                remove_target(cmd->id);
                break;
            }
        }
    }

    /// Route one incoming CC. Binds it if learn is armed, then applies any
    /// mapping for (channel, cc) to its parameter through the mapping's scale.
    void handle_cc(StateStore& store, uint8_t channel, uint8_t cc, uint8_t value) {
        if (learn_armed_) {
            insert(channel, cc, learn_target_, learn_scale_);
            learn_armed_ = false;
        }
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& m = map_[i];
            if (m.cc == cc && (m.channel == kOmni || m.channel == channel))
                store.set_normalized_rt(m.id, m.scale.apply(value));
        }
    }

    bool learn_armed() const {
        return learn_armed_;
    }

  private:
    struct Command {
        enum Type { Set, Arm, Clear } type;
        uint8_t channel;
        uint8_t cc;
        ParamID id;
        MidiMapScale scale;
    };
    struct Mapping {
        uint8_t channel;
        uint8_t cc;
        ParamID id;
        MidiMapScale scale;
    };

    static MidiMapScale clamp_scale(MidiMapScale scale) {
        scale.min = std::isfinite(scale.min) ? std::clamp(scale.min, 0.0f, 1.0f) : 0.0f;
        scale.max = std::isfinite(scale.max) ? std::clamp(scale.max, 0.0f, 1.0f) : 1.0f;
        return scale;
    }

    void insert(uint8_t channel, uint8_t cc, ParamID id, MidiMapScale scale) {
        for (std::size_t i = 0; i < count_; ++i) {
            if (map_[i].channel == channel && map_[i].cc == cc) {
                map_[i].id = id;
                map_[i].scale = scale;
                return;
            }
        }
        if (count_ < kMaxMappings)
            map_[count_++] = {channel, cc, id, scale};
    }

    void remove_target(ParamID id) {
        std::size_t w = 0;
        for (std::size_t i = 0; i < count_; ++i)
            if (map_[i].id != id)
                map_[w++] = map_[i];
        count_ = w;
    }

    static constexpr std::size_t kMaxMappings = 64;
    std::array<Mapping, kMaxMappings> map_{};
    std::size_t count_ = 0;
    bool learn_armed_ = false;
    ParamID learn_target_ = 0;
    MidiMapScale learn_scale_{};
    pulp::runtime::SpscQueue<Command, 64> commands_;
};

} // namespace pulp::state
