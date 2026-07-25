#pragma once

/// @file osc_parameter_map.hpp
/// Bind incoming OSC messages to plugin parameters, with a "learn" mode — the
/// OSC counterpart of `pulp::state::MidiParameterMap`. A tablet control surface
/// (TouchOSC, Lemur, Open Stage Control) or any OSC-speaking script reaches the
/// same parameter space a MIDI CC does, with no per-surface driver code.
///
/// Target identity: a mapping stores a `pulp::state::ParamID`. That is the same
/// stable 32-bit id a timeline `DeviceParameterTarget` carries in its `param_id`
/// field, so an address bound here lands on exactly the parameter the timeline
/// would automate — OSC, MIDI, and authored automation share one target space.
///
/// Thread model: mappings are added / armed from the UI thread and consumed on
/// the thread that dispatches OSC messages (typically the receiver's socket
/// thread). UI calls go through a lock-free command queue; the dispatching
/// thread drains it with `pump()` and then routes each message through
/// `handle_message()`. The parameter write itself lands as an atomic store the
/// audio thread picks up on its next read, and the format adapter's post-process
/// diff reports the change to the host as automation.
///
/// Which store write: `handle_message()` / `handle_value()` use the non-RT
/// `set_normalized()`, because an OSC receiver runs on its own thread and
/// `StateStore`'s RT path (`set_normalized_rt`) feeds a queue with a single
/// producer — the audio thread. A processor that would rather apply inside
/// `process()` (draining its own OSC queue there) uses `route()` and calls
/// `set_normalized_rt` from the audio thread itself, keeping that contract.
///
/// Address matching: a mapping's pattern is either a literal OSC address or an
/// OSC 1.0 address pattern (`*`, `?`, `[...]`, `{...}`). Literal patterns are
/// matched by direct byte compare and allocate nothing; wildcard patterns go
/// through `pulp::osc::address_matches`, which is not allocation-free and must
/// not be driven from the audio thread. Patterns and mappings are held in
/// fixed-capacity storage, so installing and dispatching a binding never grows
/// the heap.
///
/// Value scaling: OSC arguments are unbounded floats, so a mapping carries an
/// `OscMapScale` describing both the incoming range the surface sends
/// (`in_min`..`in_max`) and the normalized [0, 1] window it drives
/// (`out_min`..`out_max`). Input outside the incoming range is clamped, so a
/// misconfigured sender can never push a parameter past its bounds, and
/// `out_min > out_max` inverts the response. The default is the identity map for
/// the conventional 0..1 fader: 0 → 0, 1 → 1.
///
/// Usage:
///   osc_map_.set_mapping("/track/1/fader", kGain);
///   osc_map_.set_mapping("/track/*/mix", kMix, {0.0f, 1.0f, 0.25f, 0.75f});
///   osc_map_.arm_learn(kCutoff);           // next message binds to kCutoff
/// and on the dispatching thread:
///   osc_map_.pump();
///   osc_map_.handle_message(state(), msg);

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <variant>

#include <pulp/osc/bundle.hpp>
#include <pulp/osc/osc.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/state/store.hpp>

namespace pulp::osc {

/// The incoming value range an OSC surface sends, and the normalized [0, 1]
/// window it drives. Defaults to the identity map for a 0..1 fader.
/// `out_min > out_max` inverts; a degenerate incoming range pins to `out_min`.
struct OscMapScale {
    float in_min = 0.0f;
    float in_max = 1.0f;
    float out_min = 0.0f;
    float out_max = 1.0f;

    /// Map a raw OSC argument onto this window. Input is clamped to the
    /// incoming range first, so the result always lies within the output
    /// window — and therefore within [0, 1], because both output endpoints are
    /// clamped there on insertion.
    float apply(float value) const {
        if (!std::isfinite(value))
            return out_min;
        const float span = in_max - in_min;
        if (span == 0.0f)
            return out_min;
        const float t = std::clamp((value - in_min) / span, 0.0f, 1.0f);
        return out_min + t * (out_max - out_min);
    }
};

/// Longest OSC address pattern a mapping can hold, including the terminator.
/// Longer patterns are rejected rather than truncated, so a binding can never
/// silently match the wrong address.
inline constexpr std::size_t kOscPatternCapacity = 96;

class OscParameterMap {
  public:
    /// Largest number of simultaneous bindings.
    static constexpr std::size_t kMaxMappings = 64;

    /// True if `pattern` can be held verbatim in a mapping's fixed-capacity,
    /// NUL-terminated storage. `set_mapping` rejects anything else, and an armed
    /// learn ignores an incoming address that does not fit rather than binding a
    /// truncated one — a shortened pattern would silently match the wrong
    /// addresses. An embedded NUL is rejected for the same reason; a wire-format
    /// OSC address cannot contain one.
    static bool pattern_fits(std::string_view pattern) {
        return pattern.size() + 1 <= kOscPatternCapacity
               && pattern.find('\0') == std::string_view::npos;
    }

    /// True if `pattern` uses OSC wildcard syntax and must therefore be matched
    /// with `address_matches` rather than a byte compare.
    static bool is_wildcard(std::string_view pattern) {
        return pattern.find_first_of("*?[{") != std::string_view::npos;
    }

    // ── UI thread ──

    /// Bind `pattern` → parameter over the full normalized range, treating the
    /// surface as sending 0..1. Returns false if the pattern does not fit.
    bool set_mapping(std::string_view pattern, pulp::state::ParamID id) {
        return set_mapping(pattern, id, OscMapScale{});
    }

    /// Bind `pattern` → parameter through `scale`. Returns false if the pattern
    /// does not fit (nothing is queued) or if the command queue is full, so a
    /// caller can tell a rejected binding from an accepted one.
    bool set_mapping(std::string_view pattern, pulp::state::ParamID id, OscMapScale scale) {
        Command cmd{Command::Set, id, clamp_scale(scale), {}};
        if (!store_pattern(cmd.pattern, pattern))
            return false;
        return commands_.try_push(cmd);
    }

    /// Arm learn: the next incoming address binds to `id` over the full range.
    void arm_learn(pulp::state::ParamID id) {
        arm_learn(id, OscMapScale{});
    }

    /// Arm learn: the next incoming address binds to `id` through `scale`.
    void arm_learn(pulp::state::ParamID id, OscMapScale scale) {
        commands_.try_push(Command{Command::Arm, id, clamp_scale(scale), {}});
    }

    /// Remove every mapping that targets `id`.
    void clear(pulp::state::ParamID id) {
        commands_.try_push(Command{Command::Clear, id, OscMapScale{}, {}});
    }

    // ── dispatching thread ──

    /// Drain queued UI commands. Call before dispatching a batch of messages.
    void pump() {
        Command cmd{};
        while (commands_.try_pop(cmd)) {
            switch (cmd.type) {
            case Command::Set:
                insert(std::string_view{cmd.pattern.data()}, cmd.id, cmd.scale);
                break;
            case Command::Arm:
                learn_armed_ = true;
                learn_target_ = cmd.id;
                learn_scale_ = cmd.scale;
                break;
            case Command::Clear:
                remove_target(cmd.id);
                break;
            }
        }
    }

    /// Route one address / value pair to a caller-supplied sink. Binds the
    /// address if learn is armed, then invokes `apply(ParamID, normalized)` once
    /// per matching mapping, with the value already through that mapping's
    /// scale. This is the seam for applying on the audio thread: a processor
    /// draining its own OSC queue inside `process()` passes a sink that calls
    /// `set_normalized_rt`. Routing itself allocates nothing for literal
    /// addresses; wildcard patterns go through `address_matches`, which does not
    /// carry that guarantee.
    template <typename Apply>
    void route(std::string_view address, float value, Apply&& apply) {
        if (learn_armed_ && pattern_fits(address)) {
            insert(address, learn_target_, learn_scale_);
            learn_armed_ = false;
        }
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& m = map_[i];
            if (matches(m, address))
                apply(m.id, m.scale.apply(value));
        }
    }

    /// Route one address / value pair into `store` from a control thread.
    void handle_value(pulp::state::StateStore& store, std::string_view address, float value) {
        route(address, value, [&store](pulp::state::ParamID id, float normalized) {
            store.set_normalized(id, normalized);
        });
    }

    /// Route one OSC message, taking its first numeric argument as the value.
    /// `int32` arguments are widened to float; a message whose first argument is
    /// missing or non-numeric carries no control value and is ignored entirely
    /// — it neither applies a mapping nor consumes an armed learn.
    void handle_message(pulp::state::StateStore& store, const Message& msg) {
        float value = 0.0f;
        if (!numeric_argument(msg, value))
            return;
        handle_value(store, msg.address, value);
    }

    /// True while an armed learn is still waiting for an address. Read on the
    /// dispatching thread — arming from the UI is queued, so a UI-thread reader
    /// would see the state before `pump()` applied its own command.
    bool learn_armed() const {
        return learn_armed_;
    }

    /// Number of installed mappings, up to `kMaxMappings`; further bindings are
    /// dropped. Visible on the dispatching thread after `pump()`.
    std::size_t mapping_count() const {
        return count_;
    }

  private:
    using Pattern = std::array<char, kOscPatternCapacity>;

    struct Command {
        enum Type : std::uint8_t { Set, Arm, Clear } type;
        pulp::state::ParamID id;
        OscMapScale scale;
        Pattern pattern;
    };
    struct Mapping {
        Pattern pattern;
        pulp::state::ParamID id;
        OscMapScale scale;
        bool wildcard;
    };

    /// Extract the first numeric argument of `msg`. Returns false when there is
    /// none, leaving `out` untouched.
    static bool numeric_argument(const Message& msg, float& out) {
        if (msg.args.empty())
            return false;
        if (const auto* f = std::get_if<float>(&msg.args[0])) {
            out = *f;
            return true;
        }
        if (const auto* i = std::get_if<std::int32_t>(&msg.args[0])) {
            out = static_cast<float>(*i);
            return true;
        }
        return false;
    }

    static OscMapScale clamp_scale(OscMapScale scale) {
        scale.in_min = std::isfinite(scale.in_min) ? scale.in_min : 0.0f;
        scale.in_max = std::isfinite(scale.in_max) ? scale.in_max : 1.0f;
        scale.out_min =
            std::isfinite(scale.out_min) ? std::clamp(scale.out_min, 0.0f, 1.0f) : 0.0f;
        scale.out_max =
            std::isfinite(scale.out_max) ? std::clamp(scale.out_max, 0.0f, 1.0f) : 1.0f;
        return scale;
    }

    static bool store_pattern(Pattern& dest, std::string_view pattern) {
        if (!pattern_fits(pattern))
            return false;
        dest.fill('\0');
        std::memcpy(dest.data(), pattern.data(), pattern.size());
        return true;
    }

    static bool matches(const Mapping& m, std::string_view address) {
        const std::string_view pattern{m.pattern.data()};
        // A literal address is a byte compare; only wildcard syntax needs the
        // pattern matcher, which is not allocation-free.
        return m.wildcard ? address_matches(pattern, address) : pattern == address;
    }

    void insert(std::string_view pattern, pulp::state::ParamID id, OscMapScale scale) {
        for (std::size_t i = 0; i < count_; ++i) {
            if (std::string_view{map_[i].pattern.data()} == pattern) {
                map_[i].id = id;
                map_[i].scale = scale;
                return;
            }
        }
        if (count_ >= kMaxMappings)
            return;
        Mapping& m = map_[count_];
        if (!store_pattern(m.pattern, pattern))
            return;
        m.id = id;
        m.scale = scale;
        m.wildcard = is_wildcard(pattern);
        ++count_;
    }

    void remove_target(pulp::state::ParamID id) {
        std::size_t w = 0;
        for (std::size_t i = 0; i < count_; ++i)
            if (map_[i].id != id)
                map_[w++] = map_[i];
        count_ = w;
    }

    std::array<Mapping, kMaxMappings> map_{};
    std::size_t count_ = 0;
    bool learn_armed_ = false;
    pulp::state::ParamID learn_target_ = 0;
    OscMapScale learn_scale_{};
    pulp::runtime::SpscQueue<Command, 64> commands_;
};

} // namespace pulp::osc
