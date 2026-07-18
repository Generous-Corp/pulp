// signal_graph_reference_walk.cpp — the legacy SERIAL REFERENCE WALK.
//
// Holds the hand-maintained, bit-exact inter-node DSP oracle and fallback for
// SignalGraph::process_impl(): clear the output bus, walk the nodes in
// topological order (gather MIDI + audio with PDC/feedback delay lines, run
// each node's work, accumulate AudioOutput), capture feedback sources for the
// next block, and drain MidiInput scratch. process_impl() enters it via
// run_reference_walk_() whenever no routed path (anticipation / parallel /
// canonical executor) takes a block.
//
// This walk is INTENTIONALLY INDEPENDENT of GraphRuntimeExecutor
// (signal_graph_executor_routing.{hpp,cpp}). Do NOT share or merge its
// gather / PDC / feedback / MIDI / automation EXECUTION code with the executor:
// the walk's value as a parity oracle comes from being a separately maintained
// reference implementation. Its bit-exactness against the routed backends is
// guarded by test_graph_routing_differential_parity,
// test_signal_graph_executor_parity, and test_signal_graph_offline_parity. Any
// edit here that changes audio output MUST be mirrored in the executor (and
// vice versa) or those parity guards will fail.

#include <pulp/host/signal_graph.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/parameter_event_queue.hpp>
#include "signal_graph_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <utility>

namespace pulp::host {
namespace {

float map_modulation_sample(const Connection& c, float sample) {
    const float normalized = std::clamp(sample, 0.0f, 1.0f);
    return c.automation_range_lo
        + normalized * (c.automation_range_hi - c.automation_range_lo);
}

bool push_parameter_event(ParameterEventQueue& queue,
                          uint32_t param_id,
                          int sample_offset,
                          float value) {
    return queue.push({
        param_id,
        sample_offset,
        value,
        0,
    });
}

}  // namespace

void SignalGraph::run_reference_walk_(
    audio::BufferView<float>& output,
    const audio::BufferView<const float>& input,
    int num_samples,
    CompiledGraph* cg) {
    // Clear the final destination; AudioOutput nodes accumulate into it.
    for (std::size_t c = 0; c < output.num_channels(); ++c) {
        std::memset(output.channel_ptr(c), 0, sizeof(float) * static_cast<size_t>(num_samples));
    }

    auto pass_through_or_zero = [num_samples](NodeRuntime& rt) {
        const int in_ch  = static_cast<int>(rt.input_ptrs.size());
        const int out_ch = static_cast<int>(rt.output_ptrs.size());
        const int chs = std::min(in_ch, out_ch);
        for (int c = 0; c < chs; ++c) {
            std::memcpy(rt.output_ptrs[c], rt.input_ptrs[c],
                        sizeof(float) * static_cast<size_t>(num_samples));
        }
        for (int c = chs; c < out_ch; ++c) {
            std::memset(rt.output_ptrs[c], 0,
                        sizeof(float) * static_cast<size_t>(num_samples));
        }
    };

    for (const auto& ordered : cg->ordered_runtime) {
        const NodeId id = ordered.id;
        const auto& shape = ordered.shape;
        auto& rt = *ordered.runtime;

        // 1. Zero input scratch.
        if (!rt.input_data.empty()) {
            std::memset(rt.input_data.data(), 0, rt.input_data.size() * sizeof(float));
        }
        rt.midi_in_incomplete = false;
        clear_midi_block(rt.midi_in);
        rt.midi_out_incomplete = false;
        clear_midi_block(rt.midi_out);
        if (shape.type == NodeType::MidiInput && rt.midi_input_mailbox) {
            const auto& injected = rt.midi_input_mailbox->published.read();
            const auto sequence_seen =
                rt.midi_input_mailbox->sequence_seen.load(std::memory_order_relaxed);
            if (injected.sequence != 0
                && injected.sequence != sequence_seen) {
                rt.midi_input_mailbox->sequence_seen.store(
                    injected.sequence, std::memory_order_relaxed);
                if (!injected.copy_to_midi(rt.midi_out)) {
                    rt.midi_out_incomplete = true;
                }
            }
        }

        // 1b. Gather MIDI from MIDI-flagged inbound connections.
        for (const auto& edge : rt.inbound_midi_edges) {
            if (!edge.source_runtime) continue;
            if (!copy_midi_block(edge.source_runtime->midi_out, rt.midi_in)
                || edge.source_runtime->midi_out_incomplete) {
                rt.midi_in_incomplete = true;
            }
        }

        // 2. Gather audio inbound with PDC/feedback delay lines.
        for (const auto& edge : rt.inbound_audio_edges) {
            const size_t ci = edge.connection_index;
            const auto& c = cg->connections[ci];
            const int dport = static_cast<int>(c.dest_port);
            if (dport < 0 || dport >= static_cast<int>(rt.input_ptrs.size())) continue;
            if (!edge.source_runtime) continue;
            const auto& src_rt = *edge.source_runtime;
            const int sport = static_cast<int>(c.source_port);
            if (sport < 0 || sport >= static_cast<int>(src_rt.output_ptrs.size())) continue;

            float* dst = rt.input_ptrs[dport];
            const float* src = src_rt.output_ptrs[sport];
            auto& dl = cg->connection_delays[ci];
            if (c.feedback) {
                if (!dl.feedback_prev.empty()) {
                    for (int i = 0; i < num_samples; ++i) dst[i] += dl.feedback_prev[(size_t)i];
                }
                continue;
            }
            if (dl.delay_samples <= 0 || !dl.state || dl.state->ring.empty()) {
                for (int i = 0; i < num_samples; ++i) dst[i] += src[i];
            } else {
                auto& state = *dl.state;
                const int ring_size = (int)state.ring.size();
                const int D = dl.delay_samples;
                int wp = state.write_pos;
                int rp = wp - D;
                if (rp < 0) rp += ring_size;
                for (int i = 0; i < num_samples; ++i) {
                    state.ring[(size_t)wp] = src[i];
                    dst[i] += state.ring[(size_t)rp];
                    if (++wp == ring_size) wp = 0;
                    if (++rp == ring_size) rp = 0;
                }
                state.write_pos = wp;
            }
        }

        // 3. Produce output based on node type. Wrap the node's work in its
        // persistent load measurer (relaxed-atomic begin()/end(); RT-safe) so
        // per-node CPU load is attributable via node_loads(). Live-DSP telemetry
        // reads these same per-node measurers after the block (see process_impl),
        // so it needs no separate hook here.
        if (rt.load) rt.load->begin(num_samples, static_cast<float>(cg->sample_rate));
        switch (shape.type) {
            case NodeType::AudioInput: {
                const int chs = std::min(
                    static_cast<int>(rt.output_ptrs.size()),
                    static_cast<int>(input.num_channels()));
                for (int c = 0; c < chs; ++c) {
                    std::memcpy(rt.output_ptrs[c], input.channel_ptr(c),
                                sizeof(float) * static_cast<size_t>(num_samples));
                }
                for (int c = chs; c < static_cast<int>(rt.output_ptrs.size()); ++c) {
                    std::memset(rt.output_ptrs[c], 0,
                                sizeof(float) * static_cast<size_t>(num_samples));
                }
                break;
            }
            case NodeType::Plugin: {
                auto pit = cg->plugins.find(id);
                if (pit == cg->plugins.end() || !pit->second) {
                    // GraphSerializer rehydration creates a placeholder Plugin
                    // node when the saved plugin can't be resolved (missing,
                    // moved, or wrong format). Without an explicit branch
                    // here, output scratch keeps whatever stale data it
                    // carried, causing audible artifacts on the next
                    // AudioOutput gather.
                    // Deterministic fallback: pass input → output when
                    // channel counts match, zero-fill when they don't.
                    pass_through_or_zero(rt);
                    break;
                }
                audio::BufferView<float> out_view(
                    rt.output_ptrs.data(), rt.output_ptrs.size(),
                    static_cast<std::size_t>(num_samples));
                audio::BufferView<const float> in_c(
                    rt.input_const_ptrs.data(), rt.input_const_ptrs.size(),
                    static_cast<std::size_t>(num_samples));
                clear_midi_block(rt.midi_out);
                rt.midi_out_incomplete = false;

                // Build per-block automation event queue. For each
                // (param_id) target, collect values from every automation
                // edge and mix per edges' MixMode. Two control points per
                // block (sample 0 and N-1) so the plugin can interpolate.
                // Audio-rate modulation edges append one event per sample
                // after applying the same PDC delay-line alignment used by
                // normal audio connections.
                ParameterEventQueue param_events;
                {
                    auto bounds_for_param = [&rt](uint32_t param_id,
                                                  float fallback_lo,
                                                  float fallback_hi) {
                        for (const auto& bounds : rt.param_bounds) {
                            if (bounds.id != param_id) continue;
                            return std::pair<float, float>{
                                std::min(bounds.min_value, bounds.max_value),
                                std::max(bounds.min_value, bounds.max_value),
                            };
                        }
                        return std::pair<float, float>{
                            std::min(fallback_lo, fallback_hi),
                            std::max(fallback_lo, fallback_hi),
                        };
                    };

                    for (auto& a : rt.sparse_automation_accum) {
                        a = NodeRuntime::SparseAutomationAccum{};
                    }
                    const int last = num_samples - 1;
                    for (const auto& edge : rt.sparse_automation_edges) {
                        const size_t ci = edge.connection_index;
                        const auto& c = cg->connections[ci];
                        if (!edge.source_runtime) continue;
                        const int sport = static_cast<int>(c.source_port);
                        if (sport < 0
                            || sport >= (int)edge.source_runtime->output_ptrs.size()) {
                            continue;
                        }
                        const float* src = edge.source_runtime->output_ptrs[sport];
                        const float s0 = std::clamp(src[0], 0.0f, 1.0f);
                        const float sN = std::clamp(src[last < 0 ? 0 : last], 0.0f, 1.0f);
                        float m0 = c.automation_range_lo
                            + s0 * (c.automation_range_hi - c.automation_range_lo);
                        float mN = c.automation_range_lo
                            + sN * (c.automation_range_hi - c.automation_range_lo);

                        // Per-source linear slew. The user declares
                        // automation_smoothing_ms; we limit how far the
                        // delivered value can move per sample at that ramp
                        // speed. State (last_value/primed) lives on the
                        // parallel ConnectionDelay so it survives the next
                        // block.
                        if (c.automation_smoothing_ms > 0.0f
                            && cg->sample_rate > 0.0
                            && ci < cg->connection_delays.size()) {
                            auto& dl = cg->connection_delays[ci];
                            const float range = std::abs(
                                c.automation_range_hi - c.automation_range_lo);
                            const double slew_samples =
                                (double)c.automation_smoothing_ms * 0.001
                                * cg->sample_rate;
                            // max move per sample, in the plugin's plain
                            // parameter domain. We use the connection's
                            // mapped range as the "full sweep" scale so
                            // smoothing_ms behaves like "ms to traverse
                            // the entire declared range".
                            const float max_step = slew_samples > 0.0
                                ? (range / (float)slew_samples)
                                : range;
                            if (!dl.slew_primed) {
                                // First block — snap so we don't glide
                                // up from 0.
                                dl.slew_last_value = m0;
                                dl.slew_primed = true;
                            }
                            auto ramp_to = [max_step](float from, float target) {
                                const float delta = target - from;
                                if (delta > max_step) return from + max_step;
                                if (delta < -max_step) return from - max_step;
                                return target;
                            };
                            // v0 lands at sample 0; vN lands at sample
                            // `last`. Slew from the previous block's
                            // post-slew value to m0 in one step, then
                            // from there to mN over `last` steps.
                            const float new_v0 = ramp_to(dl.slew_last_value, m0);
                            float new_vN = new_v0;
                            if (last > 0) {
                                const float max_block_step =
                                    max_step * (float)last;
                                const float delta = mN - new_v0;
                                if (delta > max_block_step) {
                                    new_vN = new_v0 + max_block_step;
                                } else if (delta < -max_block_step) {
                                    new_vN = new_v0 - max_block_step;
                                } else {
                                    new_vN = mN;
                                }
                            }
                            dl.slew_last_value = new_vN;
                            m0 = new_v0;
                            mN = new_vN;
                        }

                        auto param_it = std::find(rt.sparse_automation_param_ids.begin(),
                                                  rt.sparse_automation_param_ids.end(),
                                                  c.automation_param_id);
                        if (param_it == rt.sparse_automation_param_ids.end()) continue;
                        auto& a = rt.sparse_automation_accum[static_cast<size_t>(
                            std::distance(rt.sparse_automation_param_ids.begin(), param_it))];
                        const auto bounds = bounds_for_param(c.automation_param_id,
                                                             c.automation_range_lo,
                                                             c.automation_range_hi);
                        a.lo = bounds.first;
                        a.hi = bounds.second;
                        a.touched = true;
                        if (c.automation_mix == AutomationMix::Replace) {
                            a.v0 = m0;
                            a.vN = mN;
                        } else {
                            a.v0 += m0;
                            a.vN += mN;
                            a.has_add = true;
                        }
                    }
                    for (size_t pi = 0; pi < rt.sparse_automation_accum.size(); ++pi) {
                        auto& a = rt.sparse_automation_accum[pi];
                        if (!a.touched) continue;
                        const uint32_t pid = rt.sparse_automation_param_ids[pi];
                        float v0 = a.v0, vN = a.vN;
                        if (a.has_add) {
                            const float lo = std::min(a.lo, a.hi);
                            const float hi = std::max(a.lo, a.hi);
                            v0 = std::clamp(v0, lo, hi);
                            vN = std::clamp(vN, lo, hi);
                        }
                        if (!push_parameter_event(param_events, pid, 0, v0)) break;
                        if (last > 0
                            && !push_parameter_event(param_events, pid, last, vN)) {
                            break;
                        }
                    }

                    if (!rt.audio_rate_param_ids.empty()) {
                        const size_t block = static_cast<size_t>(cg->max_block_size);
                        std::fill(rt.audio_rate_param_data.begin(),
                                  rt.audio_rate_param_data.end(),
                                  0.0f);

                        for (auto& d : rt.audio_rate_accum) {
                            d = NodeRuntime::DenseAutomationAccum{};
                        }

                        for (const auto& edge : rt.audio_rate_modulation_edges) {
                            const size_t ci = edge.connection_index;
                            const auto& c = cg->connections[ci];
                            if (!edge.source_runtime) continue;
                            const int sport = static_cast<int>(c.source_port);
                            if (sport < 0
                                || sport >= (int)edge.source_runtime->output_ptrs.size()) {
                                continue;
                            }

                            auto param_it = std::find(rt.audio_rate_param_ids.begin(),
                                                      rt.audio_rate_param_ids.end(),
                                                      c.automation_param_id);
                            if (param_it == rt.audio_rate_param_ids.end()) continue;
                            const size_t param_index = static_cast<size_t>(
                                std::distance(rt.audio_rate_param_ids.begin(), param_it));
                            auto& dst = rt.audio_rate_accum[param_index];
                            float* dst_values =
                                rt.audio_rate_param_data.data() + param_index * block;
                            const auto bounds = bounds_for_param(c.automation_param_id,
                                                                 c.automation_range_lo,
                                                                 c.automation_range_hi);
                            dst.lo = bounds.first;
                            dst.hi = bounds.second;

                            const float* src = edge.source_runtime->output_ptrs[sport];
                            auto& dl = cg->connection_delays[ci];
                            if (dl.delay_samples <= 0 || !dl.state
                                || dl.state->ring.empty()) {
                                for (int i = 0; i < num_samples; ++i) {
                                    const float value = map_modulation_sample(c, src[i]);
                                    if (c.automation_mix == AutomationMix::Replace) {
                                        dst_values[static_cast<size_t>(i)] = value;
                                        dst.has_replace = true;
                                    } else {
                                        dst_values[static_cast<size_t>(i)] += value;
                                        dst.has_add = true;
                                    }
                                }
                            } else {
                                auto& state = *dl.state;
                                const int ring_size = (int)state.ring.size();
                                const int D = dl.delay_samples;
                                int wp = state.write_pos;
                                int rp = wp - D;
                                if (rp < 0) rp += ring_size;
                                for (int i = 0; i < num_samples; ++i) {
                                    state.ring[static_cast<size_t>(wp)] = src[i];
                                    const float value = map_modulation_sample(
                                        c, state.ring[static_cast<size_t>(rp)]);
                                    if (c.automation_mix == AutomationMix::Replace) {
                                        dst_values[static_cast<size_t>(i)] = value;
                                        dst.has_replace = true;
                                    } else {
                                        dst_values[static_cast<size_t>(i)] += value;
                                        dst.has_add = true;
                                    }
                                    if (++wp == ring_size) wp = 0;
                                    if (++rp == ring_size) rp = 0;
                                }
                                state.write_pos = wp;
                            }
                        }

                        for (size_t pi = 0; pi < rt.audio_rate_accum.size(); ++pi) {
                            const auto& d = rt.audio_rate_accum[pi];
                            if (!d.has_replace && !d.has_add) continue;
                            const uint32_t param_id = rt.audio_rate_param_ids[pi];
                            const float* values =
                                rt.audio_rate_param_data.data() + pi * block;
                            const float lo = std::min(d.lo, d.hi);
                            const float hi = std::max(d.lo, d.hi);
                            for (int i = 0; i < num_samples; ++i) {
                                float value = values[static_cast<size_t>(i)];
                                if (d.has_add) value = std::clamp(value, lo, hi);
                                if (!push_parameter_event(param_events, param_id, i, value)) {
                                    break;
                                }
                            }
                        }
                    }
                    const std::uint64_t pending_parameter_sequence =
                        append_parameter_mailbox_events_(&rt, param_events);
                    param_events.sort();

                    std::array<format::ProcessBusBufferView<const float>, 1> input_buses{{
                        {
                            .info = {
                                .name = "Plugin Node In",
                                .index = 0,
                                .direction = format::BusDirection::Input,
                                // bus label for a plugin node's main I/O inside a SignalGraph
                                .role = format::BusRole::Main,
                                .declared_channels =
                                    static_cast<int>(in_c.num_channels()),
                                .optional = in_c.num_channels() == 0,
                                .active = in_c.num_channels() > 0,
                            },
                            .buffer = in_c,
                        },
                    }};
                    std::array<format::ProcessBusBufferView<float>, 1> output_buses{{
                        {
                            .info = {
                                .name = "Plugin Node Out",
                                .index = 0,
                                .direction = format::BusDirection::Output,
                                .role = format::BusRole::Main,
                                .declared_channels =
                                    static_cast<int>(out_view.num_channels()),
                                .optional = false,
                                .active = out_view.num_channels() > 0,
                            },
                            .buffer = out_view,
                        },
                    }};
                    format::ProcessBuffers process_buffers{
                        format::ProcessBusBufferSet<const float>{std::span(input_buses)},
                        format::ProcessBusBufferSet<float>{std::span(output_buses)},
                    };

                    pit->second->process(process_buffers, rt.midi_in, rt.midi_out,
                                         param_events, num_samples);
                    if (pending_parameter_sequence != 0) {
                        rt.parameter_input_mailbox->sequence_seen.store(
                            pending_parameter_sequence, std::memory_order_relaxed);
                    }
                    rt.midi_out_incomplete =
                        rt.midi_in_incomplete || midi_block_has_drops(rt.midi_out);
                    break;
                }
            }
            case NodeType::Gain: {
                const float g = rt.gain
                    ? rt.gain->load(std::memory_order_relaxed)
                    : 1.0f;
                const int chs = std::min(
                    static_cast<int>(rt.input_ptrs.size()),
                    static_cast<int>(rt.output_ptrs.size()));
                for (int c = 0; c < chs; ++c) {
                    const float* in = rt.input_ptrs[c];
                    float* out = rt.output_ptrs[c];
                    for (int i = 0; i < num_samples; ++i) out[i] = in[i] * g;
                }
                break;
            }
            case NodeType::AudioOutput: {
                const int chs = std::min(
                    static_cast<int>(rt.input_ptrs.size()),
                    static_cast<int>(output.num_channels()));
                for (int c = 0; c < chs; ++c) {
                    float* dst = output.channel_ptr(c);
                    const float* src = rt.input_ptrs[c];
                    for (int i = 0; i < num_samples; ++i) dst[i] += src[i];
                }
                break;
            }
            case NodeType::MidiInput:
                break;
            case NodeType::MidiOutput:
                if (rt.midi_output_mailbox) {
                    cg->midi_publish_scratch.set_from_midi(
                        rt.midi_in,
                        0,
                        rt.midi_in_incomplete);
                    rt.midi_output_mailbox->write(cg->midi_publish_scratch);
                }
                break;
            case NodeType::Custom:
                if (auto custom_it = cg->custom_processors.find(id);
                    custom_it != cg->custom_processors.end()) {
                    audio::BufferView<float> out_view(
                        rt.output_ptrs.data(), rt.output_ptrs.size(),
                        static_cast<std::size_t>(num_samples));
                    audio::BufferView<const float> in_view(
                        rt.input_const_ptrs.data(), rt.input_const_ptrs.size(),
                        static_cast<std::size_t>(num_samples));
                    custom_it->second(out_view, in_view, num_samples);
                } else {
                    pass_through_or_zero(rt);
                }
                break;
        }
        if (rt.load) rt.load->end();
    }

    // Capture each feedback source's current block for the *next* block.
    for (const auto& edge : cg->feedback_edges) {
        const size_t ci = edge.connection_index;
        const auto& c = cg->connections[ci];
        if (!edge.source_runtime) continue;
        const auto& src_rt = *edge.source_runtime;
        const int sport = static_cast<int>(c.source_port);
        if (sport < 0 || sport >= static_cast<int>(src_rt.output_ptrs.size())) continue;
        auto& dl = cg->connection_delays[ci];
        if (dl.feedback_prev.empty()) continue;
        const float* src = src_rt.output_ptrs[sport];
        std::memcpy(dl.feedback_prev.data(), src,
                    sizeof(float) * static_cast<size_t>(num_samples));
    }

    // Drain MidiInput nodes' audio-thread scratch so events consumed for THIS
    // block never carry over. inject_midi() publishes into the mailbox; the
    // next process() call copies a new, unseen snapshot back into midi_out.
    for (const auto& ordered : cg->ordered_runtime) {
        if (ordered.shape.type == NodeType::MidiInput) {
            clear_midi_block(ordered.runtime->midi_out);
            ordered.runtime->midi_out_incomplete = false;
        }
    }
}

}  // namespace pulp::host
