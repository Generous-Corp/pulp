#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/clip_launch.hpp>

#include <cstdint>
#include <limits>
#include <optional>

// Real-time clip-launch engine: it resolves an armed, quantized launch to a
// sample-accurate offset within the current audio block and drives a per-slot
// launch state machine. All resolution happens on the transport's MONOTONIC
// clock (see pulp/timebase/tick.hpp), which never wraps, so "launch at the next
// bar" stays well-defined even when the rendered block straddles a loop wrap.
// Every function here is allocation-free, lock-free, and total; process() is
// audio-callback safe.
namespace pulp::playback {

using timeline::LaunchQuantize;

// The first monotonic boundary at or after `from` for the given quantization.
// Immediate quantization returns `from` unchanged. The boundary set is
// { phase + k*grid : k in Z }; this returns the boundary for the smallest k with
// phase + k*grid >= from (ceil). Overflow saturates into the int64 tick domain,
// per operation: the product rails first, then the sum. A wide intermediate
// would instead clamp the exact value once, which differs only when a railed
// product is pulled back into range by phase — a regime that needs ~1.3e13
// quarter notes to reach, so both readings agree everywhere reachable. Stated
// because it is a behavioural choice, not an accident of the arithmetic.
constexpr timebase::MonotonicBeat next_launch_boundary(timebase::MonotonicBeat from,
                                                       LaunchQuantize quantize) noexcept {
    const std::int64_t grid = quantize.grid.value;
    if (grid <= 0)
        return from; // immediate: no quantization
    const std::int64_t phase = quantize.phase.value;
    const std::int64_t rel = (from.position - quantize.phase).value; // saturating subtract
    // Ceil division toward +infinity for a positive grid.
    std::int64_t k = rel / grid;
    if (rel % grid != 0 && rel > 0)
        ++k;
    return {{timebase::detail::saturating_add(
        phase, timebase::detail::saturating_multiply(k, grid))}};
}

// Resolves the block-relative sample offset at which the monotonic clock reaches
// `target`, scanning the transport ranges (which may split at a loop wrap).
//
//   - target lands inside a range [monotonic_start, monotonic_end): returns the
//     exact offset. The mapping monotonic->timeline within a range is affine
//     (equal tick deltas by construction), and timeline->sample uses the exact
//     tempo map, so the result is sample-accurate even in the post-wrap range.
//   - target is already behind the block's first range: returns 0 (fire now,
//     late) — e.g. a seek jumped the clock past the boundary.
//   - target is still ahead of every range: returns nullopt (stay armed).
//
// Range monotonic spans are contiguous across a wrap (range[i].monotonic_start ==
// range[i-1].monotonic_end), so a boundary exactly on a wrap belongs to the later
// range under the half-open convention — the wrap-safe answer.
inline std::optional<std::uint32_t>
resolve_launch_sample(const TransportSnapshot& snapshot, const timebase::CompiledTempoMap& tempo_map,
                      timebase::MonotonicBeat target) noexcept {
    const std::int64_t target_pos = target.position.value;
    for (std::uint8_t index = 0; index < snapshot.range_count; ++index) {
        const auto& range = snapshot.ranges[index];
        const std::int64_t start = range.monotonic_start.position.value;
        const std::int64_t end = range.monotonic_end.position.value;
        if (target_pos < start)
            return range.sample_offset; // boundary already behind us: fire now
        if (target_pos < end) {
            const timebase::TickDuration delta{target.position.value - start};
            const timebase::TickPosition target_tick = range.timeline_tick_start + delta;
            const timebase::SamplePosition target_sample = tempo_map.ticks_to_samples(target_tick);
            const std::int64_t offset_in_range =
                target_sample.value - range.timeline_sample_start.value;
            const std::int64_t block_offset =
                static_cast<std::int64_t>(range.sample_offset) + offset_in_range;
            // Tick->sample rounding can push a boundary in the final range's last
            // tick onto the block's end sample (== the next block's first sample).
            // Defer it so the offset stays in [0, frame_count); the next block's
            // range starts at/after this target and fires it at offset 0.
            if (block_offset >= static_cast<std::int64_t>(snapshot.frame_count))
                return std::nullopt;
            return static_cast<std::uint32_t>(block_offset);
        }
    }
    return std::nullopt; // boundary is beyond this block: stay armed
}

enum class LaunchState : std::uint8_t {
    Stopped,  // idle, not sounding
    Armed,    // triggered, waiting for the quantized launch boundary
    Playing,  // sounding
    Stopping, // sounding, waiting for the quantized stop boundary
};

enum class LaunchEventKind : std::uint8_t { None, Start, Stop };

// The outcome of processing one block. When `kind` is Start/Stop the transition
// fires at `sample_offset` within the block (0 <= offset < frame_count).
struct LaunchEvent {
    LaunchEventKind kind = LaunchEventKind::None;
    std::uint32_t sample_offset = 0;
};

// Per-slot launch state machine. Control-thread callers arm()/stop(); the audio
// thread calls process() once per block. The launch target is resolved lazily on
// the first block after a trigger, anchored to that block's starting monotonic
// position, so the resolution is deterministic given the block sequence.
class LaunchHandle {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    constexpr LaunchState state() const noexcept {
        return state_;
    }
    constexpr bool has_target() const noexcept {
        return has_target_;
    }
    constexpr timebase::MonotonicBeat target() const noexcept {
        return target_;
    }

    // Arm a (re)launch quantized by `quantize`. Legal from any state — an armed
    // slot re-arms, a playing slot retriggers at the next boundary. The target
    // resolves on the next process() block.
    constexpr void arm(LaunchQuantize quantize) noexcept {
        quantize_ = quantize;
        state_ = LaunchState::Armed;
        has_target_ = false;
    }

    // Arm a stop quantized by `quantize`. Only meaningful while sounding; a no-op
    // when already stopped/armed-from-stop.
    constexpr void stop(LaunchQuantize quantize) noexcept {
        if (state_ == LaunchState::Playing || state_ == LaunchState::Stopping) {
            quantize_ = quantize;
            state_ = LaunchState::Stopping;
            has_target_ = false;
        }
    }

    // Cancel any pending arm/stop and hard-stop immediately (no boundary).
    constexpr void reset() noexcept {
        state_ = LaunchState::Stopped;
        has_target_ = false;
        has_last_start_ = false;
    }

    // Advance one block. Emits at most one Start/Stop transition per block.
    LaunchEvent process(const TransportSnapshot& snapshot,
                        const timebase::CompiledTempoMap& tempo_map) noexcept {
        if (state_ != LaunchState::Armed && state_ != LaunchState::Stopping)
            return {};
        if (snapshot.range_count == 0)
            return {};

        if (!has_target_) {
            target_ = next_launch_boundary(snapshot.ranges[0].monotonic_start, quantize_);
            has_target_ = true;
        }

        const auto offset = resolve_launch_sample(snapshot, tempo_map, target_);
        if (!offset)
            return {}; // boundary still ahead: stay armed

        const bool starting = state_ == LaunchState::Armed;
        state_ = starting ? LaunchState::Playing : LaunchState::Stopped;
        has_target_ = false;
        if (starting) {
            last_start_ = target_;
            has_last_start_ = true;
        }
        return {starting ? LaunchEventKind::Start : LaunchEventKind::Stop, *offset};
    }

    // The monotonic beat the most recent Start resolved to — the exact tick the
    // clip began on, not an approximation recovered from a sample offset. A
    // follow-action grid anchors here so it inherits the launch's accuracy.
    constexpr bool has_last_start() const noexcept {
        return has_last_start_;
    }
    constexpr timebase::MonotonicBeat last_start() const noexcept {
        return last_start_;
    }

  private:
    LaunchQuantize quantize_{};
    timebase::MonotonicBeat target_{};
    timebase::MonotonicBeat last_start_{};
    LaunchState state_ = LaunchState::Stopped;
    bool has_target_ = false;
    bool has_last_start_ = false;
};

// Counts off a slot's follow-action grid from the beat its launch resolved to.
// The grid is anchored to that beat (phase == launch beat), so the boundaries
// are exactly { launch + k*grid } and each one resolves through the same
// resolve_launch_sample() path a launch does — a follow action is therefore
// sample-accurate under the same block splits and loop wraps a launch is.
// `repetitions` grid periods elapse before the action fires, so a two-bar grid
// with three repetitions fires six bars after the clip started.
//
// The timer copies the grid and repetition count out of the set it is armed
// with and holds no pointer into it, so the authored model may change under a
// running timer without a dangling read on the audio thread.
struct FollowActionEvent {
    bool fired = false;
    std::uint32_t sample_offset = 0;
};

class FollowActionTimer {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    // Start counting from `launch_beat`. A set that cannot fire (no candidates,
    // an immediate grid, zero repetitions) leaves the timer disarmed.
    constexpr void arm(timebase::MonotonicBeat launch_beat,
                       const timeline::FollowActionSet& set) noexcept {
        if (!set.enabled()) {
            disarm();
            return;
        }
        grid_ = set.grid;
        remaining_ = set.repetitions;
        target_ = advance(launch_beat);
        armed_ = true;
    }

    constexpr void disarm() noexcept {
        armed_ = false;
        remaining_ = 0;
    }

    constexpr bool armed() const noexcept {
        return armed_;
    }
    constexpr timebase::MonotonicBeat target() const noexcept {
        return target_;
    }
    // How many times this timer has fired. Used as the follow-action draw index,
    // which is what makes a repeated random choice reproducible.
    constexpr std::uint64_t fire_count() const noexcept {
        return fire_count_;
    }

    // Advance one block. Emits at most one fire per block; intermediate
    // repetition boundaries falling in the same block are consumed silently.
    FollowActionEvent process(const TransportSnapshot& snapshot,
                              const timebase::CompiledTempoMap& tempo_map) noexcept {
        if (!armed_ || snapshot.range_count == 0)
            return {};
        // Bounded by remaining_: every iteration consumes one repetition.
        while (true) {
            const auto offset = resolve_launch_sample(snapshot, tempo_map, target_);
            if (!offset)
                return {}; // boundary still ahead: keep counting
            --remaining_;
            if (remaining_ == 0) {
                armed_ = false;
                ++fire_count_;
                return {true, *offset};
            }
            target_ = advance(target_);
        }
    }

  private:
    // The next boundary strictly after `from` on a grid anchored at `from`.
    constexpr timebase::MonotonicBeat advance(timebase::MonotonicBeat from) const noexcept {
        const LaunchQuantize anchored{grid_, from.position};
        return next_launch_boundary({from.position + timebase::TickDuration{1}}, anchored);
    }

    timebase::TickDuration grid_{0};
    timebase::MonotonicBeat target_{};
    std::uint32_t remaining_ = 0;
    std::uint64_t fire_count_ = 0;
    bool armed_ = false;
};

} // namespace pulp::playback
