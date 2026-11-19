// SignalGraph private compile seam: executable-node binding.
// For each executable node of one compiled snapshot, builds its NodeRuntime
// (scratch buffers, load measurer, MIDI/MPE storage and mailboxes), records its
// shape, plugin and processor, resolves its transport sensitivity, and binds
// custom-node and sample-region callbacks with their latency metadata.
// compile_ calls it once, after the processing order is known.
#include "signal_graph_internal.hpp"
#include "signal_graph_port_equaliser.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <pulp/host/control_index_nodes.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::host {
namespace {
bool custom_type_matches_node_shape(const CustomNodeType& type, const GraphNode& node) {
    return type.matches_ports(node);
}
} // namespace

// Returns false when the snapshot must not be published: a custom node with a
// control port whose registered type is missing or does not match its shape.
bool SignalGraph::bind_executable_nodes_(
    CompiledGraph& compiled, double sample_rate, int max_block_size,
    const std::unordered_map<NodeId, std::shared_ptr<PreparedSampleRegion>>& region_by_anchor) {
    CompiledGraph* const cg = &compiled;
    for (auto& n : cg->executable_nodes) {
        NodeRuntime rt;
        // Resolve (or lazily create) this node's persistent load measurer.
        // node_load_ only grows here, so the raw measurer pointer handed to the
        // audio thread via NodeRuntime::load stays valid across snapshot swaps.
        // Locked against a concurrent node_loads() poll on the UI thread.
        if (prepared_edit_origin_ != nullptr) {
            std::lock_guard<std::mutex> origin_load_lock(prepared_edit_origin_->node_load_mu_);
            const auto existing = prepared_edit_origin_->node_load_.find(n.id);
            if (existing != prepared_edit_origin_->node_load_.end()) {
                rt.load = existing->second.get();
            }
        }
        if (rt.load == nullptr) {
            std::lock_guard<std::mutex> node_load_lock(node_load_mu_);
            auto& load_slot = node_load_[n.id];
            if (!load_slot) {
                load_slot = std::make_unique<audio::AudioProcessLoadMeasurer>();
            }
            rt.load = load_slot.get();
        }
        const int out_ch = std::max(0, n.num_output_ports);
        const int in_ch = std::max(0, n.num_input_ports);
        rt.output_data.assign(static_cast<size_t>(out_ch) * max_block_size, 0.f);
        rt.input_data.assign(static_cast<size_t>(in_ch) * max_block_size, 0.f);
        rt.output_ptrs.resize(out_ch);
        rt.input_ptrs.resize(in_ch);
        rt.input_const_ptrs.resize(in_ch);
        for (int c = 0; c < out_ch; ++c)
            rt.output_ptrs[c] = rt.output_data.data() + static_cast<size_t>(c) * max_block_size;
        for (int c = 0; c < in_ch; ++c) {
            rt.input_ptrs[c] = rt.input_data.data() + static_cast<size_t>(c) * max_block_size;
            rt.input_const_ptrs[c] = rt.input_ptrs[c];
        }
        rt.gain = std::make_unique<std::atomic<float>>(n.gain);
        if (n.plugin) {
            // 2.2b (H2): read cached parameter bounds, not the live slot.
            auto mit = prepared_plugin_meta_.find(n.id);
            if (mit != prepared_plugin_meta_.end()) {
                for (const auto& p : mit->second.parameters) {
                    rt.param_bounds.push_back({
                        p.id,
                        p.min_value,
                        p.max_value,
                    });
                }
            }
        }
        auto [runtime_it, inserted] = cg->runtime.emplace(n.id, std::move(rt));
        (void)inserted;
        // Every admitted routed generation owns fixed MIDI/UMP/MPE scratch.
        // Trigger plans now admit the serial host-ingress lane, so leaving
        // these arenas unprepared would make the graph silently drop the
        // projected payload before a Processor or MidiOutput can consume it.
        if (!compiling_trigger_state_ || n.type == NodeType::MidiInput ||
            n.type == NodeType::MidiOutput || n.type == NodeType::Plugin) {
            prepare_midi_block_storage(runtime_it->second.midi_in, runtime_it->second.midi_in_ump);
            prepare_midi_block_storage(runtime_it->second.midi_out,
                                       runtime_it->second.midi_out_ump);
            prepare_mpe_block_storage(runtime_it->second.mpe_in);
            prepare_mpe_block_storage(runtime_it->second.mpe_out);
        }
        if (n.type == NodeType::MidiInput) {
            // Keep mailbox identity across a live snapshot recompile so an
            // injection published between its build and publication cannot fall
            // between the old and new snapshots. Normal eager prepare has no live
            // snapshot here and allocates a fresh mailbox.
            if (const auto live = live_slot_.live()) {
                auto old_it = live->runtime.find(n.id);
                if (old_it != live->runtime.end()) {
                    runtime_it->second.midi_input_mailbox = old_it->second.midi_input_mailbox;
                }
            }
            if (!runtime_it->second.midi_input_mailbox) {
                runtime_it->second.midi_input_mailbox = std::make_shared<MidiInputMailbox>();
            }
        }
        if (n.type == NodeType::MidiOutput) {
            runtime_it->second.midi_output_mailbox = std::make_unique<MidiOutputMailbox>();
        }
        if (n.type == NodeType::Plugin &&
            (n.plugin != nullptr || processor_nodes_.contains(n.id))) {
            const auto exact_claim = exact_parameter_event_claims_.find(n.id);
            const bool exact_claimed = exact_claim != exact_parameter_event_claims_.end() &&
                                       !exact_claim->second.expired();
            const auto exact_owner = exact_claimed
                                         ? exact_claim->second.lock()
                                         : std::shared_ptr<detail::ExactParameterIngressOwner>{};
            // Keep mailbox identity across a live snapshot recompile so a
            // batch published while prepare_swap() is building cannot fall
            // between the old and new snapshots. Normal eager prepare has no
            // live snapshot here and allocates a fresh mailbox.
            if (const auto live = live_slot_.live()) {
                auto old_it = live->runtime.find(n.id);
                const auto old_exact_owner =
                    old_it != live->runtime.end()
                        ? old_it->second.exact_parameter_event_owner.lock()
                        : std::shared_ptr<detail::ExactParameterIngressOwner>{};
                if (old_it != live->runtime.end() && old_exact_owner == exact_owner) {
                    runtime_it->second.parameter_input_mailbox =
                        old_it->second.parameter_input_mailbox;
                }
            }
            if (!runtime_it->second.parameter_input_mailbox) {
                runtime_it->second.parameter_input_mailbox =
                    std::make_shared<ParameterInputMailbox>();
            }
            if (exact_claimed) {
                runtime_it->second.exact_parameter_input_mailbox =
                    std::make_unique<ParameterInputMailbox>();
                runtime_it->second.exact_parameter_event_owner = exact_claim->second;
            }
        }

        CompiledGraph::NodeShape shape{n.type, n.num_input_ports, n.num_output_ports};
        cg->shapes[n.id] = shape;

        if (n.plugin)
            cg->plugins[n.id] = n.plugin;
        if (const auto processor = processor_nodes_.find(n.id);
            processor != processor_nodes_.end() && processor->second) {
            cg->processors[n.id] = processor->second;
        }
        // Resolve the node's transport-sensitivity ONCE here (before the
        // anticipation eligibility analysis and the routed-snapshot build, both
        // later in compile_). The SAME GraphNode::transport_sensitive value feeds
        // the anticipation partition (seeds AnticipationExclusion::TransportSensitive)
        // and the routed binding (PluginBindingContext::wants_transport /
        // CustomBindingContext::process_transport), so the two can never disagree.
        // Prepare-stable: a slot/type whose capability changes later needs a
        // re-prepare to be observed.
        n.transport_sensitive = region_by_anchor.contains(n.id);
        if (n.type == NodeType::Plugin) {
            // 2.2b (H2): read cached transport-sensitivity, not the live slot.
            auto mit = prepared_plugin_meta_.find(n.id);
            n.transport_sensitive =
                (mit != prepared_plugin_meta_.end()) && mit->second.wants_transport;
        }
        if (n.type == NodeType::Custom) {
            const bool has_control_port =
                std::any_of(n.input_ports.begin(), n.input_ports.end(),
                            [](const auto& port) { return port.kind == PortKind::Control; }) ||
                std::any_of(n.output_ports.begin(), n.output_ports.end(),
                            [](const auto& port) { return port.kind == PortKind::Control; });
            if (has_control_port) {
                const auto* registered = custom_node_type(n.custom_type_id, n.custom_type_version);
                if (!registered || !custom_type_matches_node_shape(*registered, n))
                    return false;
            }
            if (const auto* type = custom_node_type(n.custom_type_id, n.custom_type_version);
                type && custom_type_matches_node_shape(*type, n)) {
                if (n.custom_instance && type->process_instance) {
                    // Bind the stateful processor. The lambda captures the
                    // instance shared_ptr BY VALUE, so this snapshot keeps the
                    // instance alive for its whole audio-thread lifetime (same
                    // guarantee as cg->plugins[...] for plugin nodes). No raw
                    // pointer into GraphNode is stored.
                    auto inst = n.custom_instance;
                    auto fn = type->process_instance;
                    cg->custom_processors[n.id] =
                        [inst, fn](audio::BufferView<float>& out,
                                   const audio::BufferView<const float>& in,
                                   int num_samples) { fn(inst.get(), out, in, num_samples); };
                } else if (type->process) {
                    cg->custom_processors[n.id] = type->process;
                }
                if (has_control_port && n.custom_instance &&
                    custom_node_reset_runs_on_audio_thread(*type)) {
                    auto inst = n.custom_instance;
                    auto fn = type->reset;
                    cg->control_stream_resets.push_back([inst, fn] { fn(inst.get()); });
                }
                if (n.custom_type_id == kControlIndexInputTypeId &&
                    n.custom_type_version == kControlIndexInputVersion) {
                    cg->control_index_state = std::make_shared<detail::ControlIndexInputState>();
                    auto state = cg->control_index_state;
                    cg->custom_processors[n.id] = [state](audio::BufferView<float>& out,
                                                          const audio::BufferView<const float>&,
                                                          int frames) {
                        if (out.num_channels() == 0 || frames <= 0 ||
                            static_cast<std::size_t>(frames) > state->frames)
                            return;
                        std::copy_n(state->indices.data(), frames, out.channel_ptr(0));
                    };
                }
                // Event-aware resolution, with the same instance-lifetime
                // guarantee: a stateful callback is wrapped in a lambda holding
                // the instance shared_ptr by value, so the map always stores the
                // stateless shape and both execution paths read one map.
                if (n.custom_instance && type->process_instance_events) {
                    auto inst = n.custom_instance;
                    auto fn = type->process_instance_events;
                    cg->custom_event_processors[n.id] =
                        [inst, fn](audio::BufferView<float>& out,
                                   const audio::BufferView<const float>& in, int num_samples,
                                   const CustomNodeEventBlock& events) {
                            fn(inst.get(), out, in, num_samples, events);
                        };
                } else if (type->process_events) {
                    cg->custom_event_processors[n.id] = type->process_events;
                }
                // Bake-layer param injection: if the type declared baked_params
                // and a param-aware process, bind a closure that captures the
                // instance shared_ptr by value (keepalive, same guarantee as the
                // plain closure above). bake() copies this so a baked graph can
                // deliver injected ParameterEvents to the node. The live routed
                // path never calls it; it runs process_instance as usual.
                if (n.custom_instance && type->process_instance_baked_param &&
                    !type->baked_params.empty()) {
                    auto inst = n.custom_instance;
                    auto pfn = type->process_instance_baked_param;
                    cg->custom_param_processors[n.id] =
                        [inst, pfn](audio::BufferView<float>& out,
                                    const audio::BufferView<const float>& in, int num_samples,
                                    const BakedParamView& params) {
                            pfn(inst.get(), out, in, num_samples, params);
                        };
                }
                // Transport-aware resolution mirrors the plain one: prefer the
                // stateful variant when an instance + stateful callback exist,
                // else the stateless variant. Any non-empty result marks the node
                // transport-sensitive (the presence of the map entry mirrors the
                // flag, both resolved from this one condition).
                if (n.custom_instance && type->process_instance_transport) {
                    auto inst = n.custom_instance;
                    auto fn = type->process_instance_transport;
                    cg->custom_transport_processors[n.id] =
                        [inst, fn](audio::BufferView<float>& out,
                                   const audio::BufferView<const float>& in, int num_samples,
                                   const format::ProcessContext& transport) {
                            fn(inst.get(), out, in, num_samples, transport);
                        };
                    n.transport_sensitive = true;
                } else if (type->process_transport) {
                    cg->custom_transport_processors[n.id] = type->process_transport;
                    n.transport_sensitive = true;
                }
                // A registered type with no live callback is transparent on the
                // live graph, so it must not add latency there. Baked-only
                // callbacks capture their latency separately during lowering.
                // Evaluated once here, off the audio thread, at the graph's own
                // rate, and clamped into the declared range; the value is not
                // knowable at registration, so this is where it gets checked.
                // Transport binding is resolved above so transport-only nodes
                // participate in the same prepared latency metadata as the
                // plain and event callback variants.
                const bool has_live_callback = cg->custom_processors.contains(n.id) ||
                                               cg->custom_event_processors.contains(n.id) ||
                                               cg->custom_transport_processors.contains(n.id);
                if (has_live_callback &&
                    (type->latency_samples_for_block || type->latency_samples)) {
                    const int latency =
                        type->latency_samples_for_block
                            ? type->latency_samples_for_block(sample_rate, max_block_size)
                            : type->latency_samples(sample_rate);
                    cg->custom_latency_samples[n.id] =
                        std::clamp(latency, 0, CustomNodeType::kMaxLatencySamples);
                }
                // Intentional delay is reported, never compensated: it does not
                // touch custom_latency_samples, so neither PDC pass sees it.
                if (has_live_callback && type->intentional_delay_samples) {
                    cg->custom_intentional_delay_samples[n.id] =
                        std::clamp(type->intentional_delay_samples(sample_rate), 0,
                                   CustomNodeType::kMaxLatencySamples);
                }
                // Per-port artifact latency: the node reports the maximum, and
                // the binding equalises the other ports up to it.
                if (has_live_callback && type->output_port_latency_samples) {
                    std::vector<int> ports(static_cast<std::size_t>(n.num_output_ports), 0);
                    for (int port = 0; port < n.num_output_ports; ++port)
                        ports[static_cast<std::size_t>(port)] =
                            std::clamp(type->output_port_latency_samples(sample_rate, port), 0,
                                       CustomNodeType::kMaxLatencySamples);
                    const int node_latency =
                        ports.empty() ? 0 : *std::max_element(ports.begin(), ports.end());
                    cg->custom_latency_samples[n.id] = node_latency;
                    cg->custom_output_port_latency_samples[n.id] = std::move(ports);
                }
                // Equalise per-port artifact latency inside the binding, after
                // every live callback variant has been resolved. One equaliser
                // serves all variants (only one runs per block). A live snapshot
                // whose equaliser declares the same port latencies hands its
                // state over, so a gap-free swap keeps the delayed history.
                if (const auto ports = cg->custom_output_port_latency_samples.find(n.id);
                    ports != cg->custom_output_port_latency_samples.end()) {
                    std::shared_ptr<detail::OutputPortLatencyEqualiser> equaliser;
                    if (const auto live = live_slot_.live()) {
                        if (const auto old = live->custom_output_port_equalisers.find(n.id);
                            old != live->custom_output_port_equalisers.end() && old->second &&
                            old->second->port_latencies() == ports->second)
                            equaliser = old->second;
                    }
                    if (!equaliser) {
                        auto fresh = std::make_shared<detail::OutputPortLatencyEqualiser>();
                        if (fresh->prepare(ports->second))
                            equaliser = std::move(fresh);
                    }
                    if (equaliser) {
                        cg->custom_output_port_equalisers[n.id] = equaliser;
                        if (auto it = cg->custom_processors.find(n.id);
                            it != cg->custom_processors.end()) {
                            it->second = [equaliser, fn = std::move(it->second)](
                                             audio::BufferView<float>& out,
                                             const audio::BufferView<const float>& in,
                                             int num_samples) {
                                fn(out, in, num_samples);
                                equaliser->process(out, num_samples);
                            };
                        }
                        if (auto it = cg->custom_event_processors.find(n.id);
                            it != cg->custom_event_processors.end()) {
                            it->second = [equaliser, fn = std::move(it->second)](
                                             audio::BufferView<float>& out,
                                             const audio::BufferView<const float>& in,
                                             int num_samples, const CustomNodeEventBlock& events) {
                                fn(out, in, num_samples, events);
                                equaliser->process(out, num_samples);
                            };
                        }
                        if (auto it = cg->custom_transport_processors.find(n.id);
                            it != cg->custom_transport_processors.end()) {
                            it->second = [equaliser, fn = std::move(it->second)](
                                             audio::BufferView<float>& out,
                                             const audio::BufferView<const float>& in,
                                             int num_samples,
                                             const format::ProcessContext& transport) {
                                fn(out, in, num_samples, transport);
                                equaliser->process(out, num_samples);
                            };
                        }
                    }
                }
            }
        }
        if (const auto region = region_by_anchor.find(n.id); region != region_by_anchor.end()) {
            auto prepared = region->second;
            cg->custom_processors[n.id] =
                [prepared](audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                           int frames) noexcept { prepared->process(out, in, frames); };
            cg->custom_latency_samples[n.id] = 0;
            cg->custom_intentional_delay_samples.erase(n.id);
            cg->custom_output_port_latency_samples.erase(n.id);
            cg->custom_output_port_equalisers.erase(n.id);
            // A region anchor's processor IS the prepared region, which has no
            // event plane and no transport plane. Drop both bindings its
            // registered type may have resolved above, so the region cannot be
            // entered through either lane. The transport entry matters because
            // the routed binding tries transport BEFORE the plain callback while
            // the reference walk has no transport branch at all, so leaving one
            // here would run the transport callback on one path and the region on
            // the other.
            cg->custom_event_processors.erase(n.id);
            cg->custom_transport_processors.erase(n.id);
            n.transport_sensitive = true;
        }
        // Before sample-region quotienting, compile_ resolved this flag directly
        // on nodes_. Keep that public authoring readback stable now that the
        // executable topology is a private copy with region members removed.
        if (auto* authored = node_mut_locked_(n.id))
            authored->transport_sensitive = n.transport_sensitive;
    }
    return true;
}

} // namespace pulp::host
