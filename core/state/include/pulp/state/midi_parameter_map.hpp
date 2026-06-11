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
/// Usage (in the processor):
///   midi_map_.pump();                       // top of process()
///   for (auto& e : midi_in)
///     if (e.is_cc())
///       midi_map_.handle_cc(state(), e.channel(), e.cc_number(), e.cc_value());
/// And from the UI:
///   midi_map_.arm_learn(kCutoff);           // next CC binds to kCutoff
///   midi_map_.set_mapping(0, 74, kCutoff);  // or map explicitly

#include <pulp/state/store.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::state {

class MidiParameterMap {
public:
    /// Match any MIDI channel.
    static constexpr uint8_t kOmni = 0xFF;

    // ── UI thread ──

    /// Map (channel, cc) → parameter. `channel` may be kOmni.
    void set_mapping(uint8_t channel, uint8_t cc, ParamID id) {
        commands_.try_push({Command::Set, channel, cc, id});
    }

    /// Arm learn: the next incoming CC binds to `id`.
    void arm_learn(ParamID id) {
        commands_.try_push({Command::Arm, kOmni, 0, id});
    }

    /// Remove any mapping that targets `id`.
    void clear(ParamID id) {
        commands_.try_push({Command::Clear, kOmni, 0, id});
    }

    // ── audio thread ──

    /// Drain queued UI commands. Call once at the top of `process()`.
    void pump() {
        while (auto cmd = commands_.try_pop()) {
            switch (cmd->type) {
                case Command::Set:   insert(cmd->channel, cmd->cc, cmd->id); break;
                case Command::Arm:   learn_armed_ = true; learn_target_ = cmd->id; break;
                case Command::Clear: remove_target(cmd->id); break;
            }
        }
    }

    /// Route one incoming CC. Binds it if learn is armed, then applies any
    /// mapping for (channel, cc) to its parameter.
    void handle_cc(StateStore& store, uint8_t channel, uint8_t cc, uint8_t value) {
        if (learn_armed_) {
            insert(channel, cc, learn_target_);
            learn_armed_ = false;
        }
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& m = map_[i];
            if (m.cc == cc && (m.channel == kOmni || m.channel == channel))
                store.set_normalized_rt(m.id, value / 127.0f);
        }
    }

    bool learn_armed() const { return learn_armed_; }

private:
    struct Command {
        enum Type { Set, Arm, Clear } type;
        uint8_t channel;
        uint8_t cc;
        ParamID id;
    };
    struct Mapping {
        uint8_t channel;
        uint8_t cc;
        ParamID id;
    };

    void insert(uint8_t channel, uint8_t cc, ParamID id) {
        for (std::size_t i = 0; i < count_; ++i) {
            if (map_[i].channel == channel && map_[i].cc == cc) {
                map_[i].id = id;
                return;
            }
        }
        if (count_ < kMaxMappings) map_[count_++] = {channel, cc, id};
    }

    void remove_target(ParamID id) {
        std::size_t w = 0;
        for (std::size_t i = 0; i < count_; ++i)
            if (map_[i].id != id) map_[w++] = map_[i];
        count_ = w;
    }

    static constexpr std::size_t kMaxMappings = 64;
    std::array<Mapping, kMaxMappings> map_{};
    std::size_t count_ = 0;
    bool learn_armed_ = false;
    ParamID learn_target_ = 0;
    pulp::runtime::SpscQueue<Command, 64> commands_;
};

} // namespace pulp::state
