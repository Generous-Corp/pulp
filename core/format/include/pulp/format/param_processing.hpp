#pragma once

// Processor-facing helpers for sample-accurate parameter work.

#include <pulp/audio/buffer.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/state/param_cursor.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>
#include <cstddef>
#include <span>
#include <utility>

namespace pulp::format {

using ParamCursor = state::ParamCursor;
using ParamSnapshotEntry = state::ParamSnapshotEntry;

template <typename Fn>
void for_each_subblock(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input,
                       const state::ParameterEventQueue* events,
                       state::ParamCursor& params,
                       Fn&& fn)
{
    const auto total = output.num_samples();
    params.advance_to(0);
    if (total == 0) return;

    std::size_t start = 0;
    const auto event_span = events ? events->events() : std::span<const state::ParameterEvent>{};

    auto emit = [&](std::size_t block_start, std::size_t block_end) {
        if (block_end <= block_start) return;
        auto out_slice = output.slice(block_start, block_end - block_start);
        auto in_slice = input.slice(block_start, block_end - block_start);
        fn(out_slice, in_slice, params);
    };

    for (const auto& event : event_span) {
        if (event.sample_offset <= 0) continue;
        if (static_cast<std::size_t>(event.sample_offset) >= total) break;

        const auto split = static_cast<std::size_t>(event.sample_offset);
        if (split > start) {
            emit(start, split);
        }
        params.advance_to(event.sample_offset);
        start = split;
    }

    if (start < total) {
        emit(start, total);
    }
}

template <typename Fn>
void for_each_subblock(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input,
                       const state::StateStore& store,
                       const state::ParameterEventQueue* events,
                       std::span<const state::ParamSnapshotEntry> initial_values,
                       Fn&& fn)
{
    state::ParamCursor params(store, events, initial_values);
    for_each_subblock(output, input, events, params, std::forward<Fn>(fn));
}

template <typename Fn>
void for_each_subblock(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input,
                       const state::StateStore& store,
                       const state::ParameterEventQueue* events,
                       Fn&& fn)
{
    for_each_subblock(output, input, store, events,
                      std::span<const state::ParamSnapshotEntry>{},
                      std::forward<Fn>(fn));
}

class ControlRateParamSmoother {
public:
    void prepare(const state::ParamInfo& info,
                 double sample_rate,
                 float initial_value) {
        ramp_seconds_ = info.smoothing_ramp_seconds > 0.0f
            ? info.smoothing_ramp_seconds
            : 0.0f;
        smoothing_enabled_ = ramp_seconds_ > 0.0f && sample_rate > 0.0;
        value_.set_immediate(initial_value);
        if (smoothing_enabled_) {
            value_.set_ramp_time(ramp_seconds_, static_cast<float>(sample_rate));
        }
    }

    void reset(float value) { value_.set_immediate(value); }

    void set_target(float value) {
        if (smoothing_enabled_) {
            value_.set_target(value);
        } else {
            value_.set_immediate(value);
        }
    }

    float next() { return value_.next(); }
    void skip(int samples) { value_.skip(samples); }
    float current() const { return value_.current(); }
    float target() const { return value_.target(); }
    bool is_smoothing() const { return value_.is_smoothing(); }
    bool smoothing_enabled() const { return smoothing_enabled_; }
    float ramp_seconds() const { return ramp_seconds_; }

private:
    signal::SmoothedValue<float> value_;
    float ramp_seconds_ = 0.0f;
    bool smoothing_enabled_ = false;
};

} // namespace pulp::format
