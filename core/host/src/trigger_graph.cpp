#include "signal_graph_internal.hpp"

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/trigger_graph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace pulp::host {
SignalGraph::MidiBlockSnapshot::MidiBlockSnapshot(bool reserve_storage) {
    if (reserve_storage)
        prepare_midi_block_storage(events, ump);
}
namespace detail {
namespace {
constexpr std::size_t record_capacity = 4096 * 8;
// Reservations include object storage, alignment and shared-owner bookkeeping.
// This fixed overhead is conservative; unused reserved bytes remain charged.
constexpr std::size_t owner_overhead = 256;
constexpr std::size_t authored_descriptor_reservation = 4096;
void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
struct TriggerArena {
    std::array<TriggerRecord, record_capacity> records;
};
struct Gate {
    std::atomic_flag* flag = nullptr;
    explicit Gate(std::atomic_flag& value) noexcept {
        if (!value.test_and_set(std::memory_order_acquire))
            flag = &value;
    }
    ~Gate() {
        if (flag)
            flag->clear(std::memory_order_release);
    }
    explicit operator bool() const noexcept {
        return flag != nullptr;
    }
};
} // namespace

struct TriggerController {
    std::atomic_flag gate = ATOMIC_FLAG_INIT;
    std::atomic<const void*> active{nullptr};
    std::atomic<std::size_t> generations{0};
    std::unique_ptr<TriggerArena> arena;
    std::uint64_t epoch = 0;
    bool reserve(std::size_t bytes) noexcept {
        auto used = generations.load(std::memory_order_relaxed);
        const auto fixed = sizeof(TriggerController) + sizeof(TriggerArena) + 2 * owner_overhead +
                           authored_descriptor_reservation;
        if (fixed > TriggerResourceReport::limit || bytes > TriggerResourceReport::limit - fixed)
            return false;
        do {
            if (used > TriggerResourceReport::limit - fixed - bytes)
                return false;
        } while (!generations.compare_exchange_weak(used, used + bytes, std::memory_order_acq_rel,
                                                    std::memory_order_relaxed));
        return true;
    }
};
struct TriggerState {
    std::shared_ptr<TriggerController> controller;
    TriggerProducerConfig producer_config;
    TriggerEnvelopeConfig envelope_config;
    TriggerEdge edge;
    TriggerCounters counters;
    std::array<bool, 16> above{};
    float envelope = 0;
    std::uint64_t current_sample = 0;
    std::uint64_t consumed_frontier = 0;
    std::size_t trace_size = 0;
    std::size_t sample_begin = 0;
    std::uint32_t max_frames = 0;
    std::uint8_t phase = 0; // 0 idle/sealed, 1 producer, 2 consumer
    bool attempt_closed = false;
    bool rendered = false;
    bool consumer_has_input = false;
    struct AudioState {
        NodeId id;
        TriggerAudioConfig config;
        float memory = 0;
    };
    std::vector<AudioState> audio;
    std::size_t reserved_bytes = 0;
    static constexpr std::size_t reservation() noexcept {
        // One charged page includes state, the candidate's two one-entry
        // factory maps, edge descriptor, snapshot ownership and alignment.
        static_assert(sizeof(TriggerState) + owner_overhead < 4096);
        return 4096;
    }
    ~TriggerState() {
        controller->generations.fetch_sub(reserved_bytes, std::memory_order_acq_rel);
    }
    void reset(std::uint64_t epoch) noexcept {
        counters = {};
        counters.epoch = epoch;
        above.fill(false);
        envelope = 0;
        for (auto& node : audio)
            node.memory = 0;
        current_sample = consumed_frontier = 0;
        trace_size = sample_begin = 0;
        phase = 0;
        attempt_closed = rendered = false;
    }
    TriggerEmitResult emit_at(TriggerPayload payload, std::uint64_t epoch, std::uint8_t port,
                              std::uint64_t target, bool out_of_range = false) noexcept {
        if (attempt_closed)
            return TriggerEmitResult::AttemptLimit;
        if (phase != 1) {
            increment(counters.diagnostic_refusals);
            return TriggerEmitResult::InvalidPhase;
        }
        if (counters.attempts == 16) {
            attempt_closed = true;
            increment(counters.diagnostic_refusals);
            return TriggerEmitResult::AttemptLimit;
        }
        ++counters.attempts;
        const auto sequence = counters.sequence++;
        auto refuse = [&](TriggerEmitResult result) {
            increment(counters.diagnostic_refusals);
            return result;
        };
        if (epoch != counters.epoch)
            return refuse(TriggerEmitResult::StaleEpoch);
        if (port != 0 || edge.producer == 0 || edge.consumer == 0)
            return refuse(TriggerEmitResult::Unbound);
        if (out_of_range)
            return refuse(TriggerEmitResult::OutOfRange);
        if (target > current_sample)
            return refuse(TriggerEmitResult::Future);
        if (target < current_sample || (rendered && target < consumed_frontier))
            return refuse(TriggerEmitResult::Late);
        if (counters.accepted == 8) {
            increment(counters.overflow);
            return TriggerEmitResult::Overflow;
        }
        controller->arena->records[trace_size++] = {counters.epoch,      current_sample, sequence,
                                                    edge.producer,       payload.token,  0,
                                                    TriggerKind::Trigger};
        ++counters.accepted;
        return TriggerEmitResult::Accepted;
    }
    void produce_sample(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                        int, const CustomNodeEventBlock& events) noexcept {
        for (std::uint8_t channel = 0; channel < producer_config.input_channels; ++channel)
            out.channel_ptr(channel)[0] = in.channel_ptr(channel)[0];
        for (std::uint8_t lane = 0; lane < producer_config.lane_count; ++lane) {
            const auto& detector = producer_config.lanes[lane];
            const bool high = in.channel_ptr(detector.input_channel)[0] > detector.threshold;
            if (high && !above[lane])
                (void)events.triggers->emit(detector.payload);
            above[lane] = high;
        }
    }
    void consume_sample(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                        int, const CustomNodeEventBlock& events) noexcept {
        for (const auto& trigger : events.triggers->received()) {
            (void)trigger;
            envelope = 1.0f;
        }
        const auto channel = producer_config.input_channels;
        out.channel_ptr(channel)[0] =
            envelope * (consumer_has_input ? in.channel_ptr(channel)[0] : 1.0f);
        envelope *= envelope_config.decay;
    }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 int frames) noexcept {
        TriggerContext context(*this);
        for (int s = 0; s < frames; ++s) {
            current_sample = counters.cursor + static_cast<std::uint64_t>(s);
            sample_begin = trace_size;
            counters.attempts = counters.accepted = 0;
            attempt_closed = false;
            phase = 1;
            const CustomNodeEventBlock events{nullptr, nullptr, &context};
            auto sample_out = out.slice(s, 1);
            const auto sample_in = in.slice(s, 1);
            produce_sample(sample_out, sample_in, 1, events);
            phase = 2;
            consume_sample(sample_out, sample_in, 1, events);
            phase = 0;
            consumed_frontier = current_sample + 1;
            rendered = true;
        }
    }
};
} // namespace detail

TriggerEmitResult TriggerContext::emit(TriggerPayload payload) noexcept {
    return state_->emit_at(payload, state_->counters.epoch, 0, state_->current_sample);
}
std::span<const TriggerRecord> TriggerContext::received() const noexcept {
    if (state_->phase != 2)
        return {};
    return {state_->controller->arena->records.data() + state_->sample_begin,
            state_->trace_size - state_->sample_begin};
}

struct PreparedTriggerPlan::Impl {
    SignalGraph* owner = nullptr;
    std::uint64_t authoring_generation = 0;
    std::shared_ptr<SignalGraph> prepared_owner;
    std::shared_ptr<SignalGraph::CompiledGraph> graph;
};
PreparedTriggerPlan::PreparedTriggerPlan(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PreparedTriggerPlan::~PreparedTriggerPlan() = default;

bool SignalGraph::ensure_trigger_controller_() {
    if (trigger_controller_)
        return true;
    static_assert(sizeof(detail::TriggerController) + sizeof(detail::TriggerArena) +
                      2 * detail::owner_overhead + detail::authored_descriptor_reservation +
                      detail::TriggerState::reservation() <=
                  TriggerResourceReport::limit);
    try {
        auto controller = std::make_shared<detail::TriggerController>();
        controller->arena = std::make_unique<detail::TriggerArena>();
        trigger_controller_ = std::move(controller);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

NodeId SignalGraph::add_trigger_producer(const TriggerProducerConfig& config) {
    if (config.lane_count == 0 || config.lane_count > 16 || config.input_channels == 0 ||
        config.input_channels > 16)
        return 0;
    for (std::uint8_t i = 0; i < config.lane_count; ++i)
        if (!std::isfinite(config.lanes[i].threshold) ||
            config.lanes[i].input_channel >= config.input_channels)
            return 0;
    GraphMutationLock lock(*this);
    if (!trigger_producers_.empty() || !ensure_trigger_controller_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Custom;
    node.name = "Trigger onset";
    node.custom_type_id = "pulp.trigger.onset";
    node.num_input_ports = config.input_channels;
    node.num_output_ports = config.input_channels;
    const auto id = node.id;
    nodes_.push_back(std::move(node));
    trigger_producers_.emplace(id, config);
    typed_trigger_authoring_.store(true, std::memory_order_release);
    invalidate_live_locked_();
    return id;
}
NodeId SignalGraph::add_trigger_envelope(const TriggerEnvelopeConfig& config) {
    if (!std::isfinite(config.decay) || config.decay < 0 || config.decay > 1)
        return 0;
    GraphMutationLock lock(*this);
    if (!trigger_envelopes_.empty() || !ensure_trigger_controller_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Custom;
    node.name = "Trigger envelope";
    node.custom_type_id = "pulp.trigger.envelope";
    node.num_input_ports = 1;
    node.num_output_ports = 1;
    const auto id = node.id;
    nodes_.push_back(std::move(node));
    trigger_envelopes_.emplace(id, config);
    typed_trigger_authoring_.store(true, std::memory_order_release);
    invalidate_live_locked_();
    return id;
}
NodeId SignalGraph::add_trigger_modulation() {
    return add_trigger_audio_({TriggerAudioKind::Modulation, 0.5f});
}
NodeId SignalGraph::add_trigger_lowpass(float coefficient) {
    if (!std::isfinite(coefficient) || coefficient < 0 || coefficient > 1)
        return 0;
    return add_trigger_audio_({TriggerAudioKind::Lowpass, coefficient});
}
NodeId SignalGraph::add_trigger_audio_(TriggerAudioConfig config) {
    GraphMutationLock lock(*this);
    if (!ensure_trigger_controller_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Custom;
    node.name =
        config.kind == TriggerAudioKind::Modulation ? "Trigger modulation" : "Trigger lowpass";
    node.custom_type_id = config.kind == TriggerAudioKind::Modulation ? "pulp.trigger.modulation"
                                                                      : "pulp.trigger.lowpass";
    node.num_input_ports = config.kind == TriggerAudioKind::Modulation ? 2 : 1;
    node.num_output_ports = 1;
    const auto id = node.id;
    nodes_.push_back(std::move(node));
    trigger_audio_.emplace(id, config);
    typed_trigger_authoring_.store(true, std::memory_order_release);
    invalidate_live_locked_();
    return id;
}
TriggerGraphResult SignalGraph::connect_trigger(TriggerEdge edge) {
    GraphMutationLock lock(*this);
    if (edge.kind != TriggerKind::Trigger || edge.source_port || edge.destination_port)
        return TriggerGraphResult::Payload;
    if (edge.producer == edge.consumer)
        return TriggerGraphResult::Feedback;
    if (!trigger_producers_.contains(edge.producer) || !trigger_envelopes_.contains(edge.consumer))
        return TriggerGraphResult::UnsupportedProducer;
    if (!trigger_edges_.empty())
        return TriggerGraphResult::Cardinality;
    trigger_edges_.push_back(edge);
    invalidate_live_locked_();
    return TriggerGraphResult::Accepted;
}
std::vector<TriggerEdge> SignalGraph::trigger_edges() const {
    GraphMutationLock lock(*this);
    return trigger_edges_;
}
void SignalGraph::invalidate_trigger_binding_() noexcept {
    if (trigger_controller_)
        trigger_controller_->active.store(nullptr, std::memory_order_release);
}

bool SignalGraph::compile_trigger_pair_(CompiledGraph& graph) {
    if (!compiling_trigger_state_)
        return true;
    auto state = compiling_trigger_state_;
    const auto producer = state->edge.producer;
    const auto consumer = state->edge.consumer;
    auto p = std::find_if(graph.executable_nodes.begin(), graph.executable_nodes.end(),
                          [&](const auto& n) { return n.id == producer; });
    auto c = std::find_if(graph.executable_nodes.begin(), graph.executable_nodes.end(),
                          [&](const auto& n) { return n.id == consumer; });
    if (p == graph.executable_nodes.end() || c == graph.executable_nodes.end())
        return false;
    p->num_input_ports = state->producer_config.input_channels + 1;
    p->num_output_ports = state->producer_config.input_channels + 1;
    p->transport_sensitive = true;
    graph.executable_nodes.erase(c);
    for (auto& edge : graph.connections) {
        if ((edge.source_node == producer || edge.source_node == consumer) &&
            (edge.dest_node == producer || edge.dest_node == consumer))
            return false;
        if (edge.source_node == consumer) {
            edge.source_node = producer;
            edge.source_port += state->producer_config.input_channels;
        }
        if (edge.dest_node == consumer) {
            state->consumer_has_input = true;
            edge.dest_node = producer;
            edge.dest_port += state->producer_config.input_channels;
        }
    }
    graph.trigger_state = std::move(state);
    return true;
}
void SignalGraph::bind_trigger_pair_(CompiledGraph& graph) {
    if (!graph.trigger_state)
        return;
    auto state = graph.trigger_state;
    graph.custom_event_processors[state->edge.producer] =
        [state](audio::BufferView<float>& out, const audio::BufferView<const float>& in, int frames,
                const CustomNodeEventBlock&) noexcept { state->process(out, in, frames); };
    graph.custom_processors.erase(state->edge.producer);
    graph.custom_latency_samples[state->edge.producer] = 0;
    for (std::size_t index = 0; index < state->audio.size(); ++index) {
        const auto id = state->audio[index].id;
        graph.custom_processors[id] = [state, index](audio::BufferView<float>& out,
                                                     const audio::BufferView<const float>& in,
                                                     int frames) noexcept {
            auto& node = state->audio[index];
            for (int sample = 0; sample < frames; ++sample) {
                if (node.config.kind == TriggerAudioKind::Modulation)
                    out.channel_ptr(0)[sample] =
                        in.channel_ptr(0)[sample] * in.channel_ptr(1)[sample];
                else {
                    node.memory +=
                        node.config.coefficient * (in.channel_ptr(0)[sample] - node.memory);
                    out.channel_ptr(0)[sample] = node.memory;
                }
            }
        };
        graph.custom_latency_samples[id] = 0;
    }
}

TriggerPreparation SignalGraph::prepare_trigger_plan(double sample_rate, int max_frames) {
    GraphMutationLock lock(*this);
    auto fail = [](TriggerGraphResult code) { return TriggerPreparation{code, {}}; };
    if (!canonical_executor_routing_enabled() || parallel_routing_enabled() ||
        anticipation_enabled() || !sample_region_definitions_.empty() || in_swap_edit_)
        return fail(TriggerGraphResult::Mode);
    if (max_frames < 1 || max_frames > 4096 || !std::isfinite(sample_rate) || sample_rate <= 0)
        return fail(TriggerGraphResult::Budget);
    if (trigger_producers_.size() != 1 || trigger_envelopes_.size() != 1 ||
        trigger_edges_.size() != 1)
        return fail(TriggerGraphResult::Cardinality);
    const auto edge = trigger_edges_.front();
    if (edge.kind != TriggerKind::Trigger || edge.source_port || edge.destination_port)
        return fail(TriggerGraphResult::Payload);
    if (!trigger_producers_.contains(edge.producer) || !trigger_envelopes_.contains(edge.consumer))
        return fail(TriggerGraphResult::Binding);
    if (edge.producer == edge.consumer)
        return fail(TriggerGraphResult::Feedback);
    for (const auto& [key, type] : custom_node_types_) {
        (void)key;
        if (type.type_id.starts_with("pulp.trigger."))
            return fail(TriggerGraphResult::UnsupportedProducer);
    }
    bool has_producer = false, has_consumer = false;
    for (const auto& node : nodes_) {
        if (node.id == edge.producer) {
            has_producer =
                node.type == NodeType::Custom && node.custom_type_id == "pulp.trigger.onset";
        } else if (node.id == edge.consumer) {
            has_consumer =
                node.type == NodeType::Custom && node.custom_type_id == "pulp.trigger.envelope";
        }
        if (node.id != edge.producer && node.id != edge.consumer &&
            (node.custom_type_id == "pulp.trigger.onset" ||
             node.custom_type_id == "pulp.trigger.envelope"))
            return fail(TriggerGraphResult::UnsupportedProducer);
        if (node.type == NodeType::Custom && node.id != edge.producer && node.id != edge.consumer &&
            !trigger_audio_.contains(node.id))
            return fail(TriggerGraphResult::UnsupportedProducer);
        // First typed publication owns fresh bounded builtin/Gain/audio state. Hosted
        // Processor and MIDI ingress lifecycles need their own admission.
        if (node.type == NodeType::Plugin || node.type == NodeType::MidiInput ||
            node.type == NodeType::MidiOutput)
            return fail(TriggerGraphResult::Mode);
    }
    if (!has_producer || !has_consumer)
        return fail(TriggerGraphResult::UnsupportedProducer);
    for (const auto& connection : connections_) {
        if (connection.feedback &&
            (connection.midi || connection.automation || connection.audio_rate_modulation))
            return fail(TriggerGraphResult::Feedback);
        const bool from_pair =
            connection.source_node == edge.producer || connection.source_node == edge.consumer;
        const bool to_pair =
            connection.dest_node == edge.producer || connection.dest_node == edge.consumer;
        if (from_pair && to_pair)
            return fail(TriggerGraphResult::DependencyCycle);
        if (connection.midi || connection.automation || connection.audio_rate_modulation ||
            connection.sidechain)
            return fail(TriggerGraphResult::Payload);
    }
    // Charge the whole candidate, including duplicate compiler/runtime scratch,
    // before constructing it. Audio storage is bounded by the reuse-free port
    // sum (serial reuse can only reduce it); both legacy and routed copies are
    // charged. Only zero-latency framework factories are admitted, so there are
    // no PDC rings. Every feedback edge owns two max-frame copies.
    // The fixed/container allowances include telemetry's 256-block windows,
    // hash-table nodes/buckets, callback captures, allocator alignment, and
    // temporary compiler plans. Allocation-probe controls check the reservation
    // against all bytes allocated during real maximum-frame preparations.
    std::size_t reservation =
        sizeof(SignalGraph) + sizeof(CompiledGraph) + detail::TriggerState::reservation() + 65536;
    const auto charge = [&](std::size_t count, std::size_t bytes) {
        if (bytes && count > TriggerResourceReport::limit / bytes)
            return false;
        const auto amount = count * bytes;
        if (reservation > TriggerResourceReport::limit - amount)
            return false;
        reservation += amount;
        return true;
    };
    std::size_t ports = 0, feedback = 0;
    for (const auto& node : nodes_) {
        if (node.id == edge.consumer)
            continue;
        const auto inputs = node.id == edge.producer
                                ? trigger_producers_.at(edge.producer).input_channels + 1
                                : node.num_input_ports;
        const auto outputs = node.id == edge.producer ? inputs : node.num_output_ports;
        if (inputs < 0 || outputs < 0 || inputs > 64 || outputs > 64)
            return fail(TriggerGraphResult::Budget);
        ports += static_cast<std::size_t>(inputs + outputs);
        if (!charge(1, sizeof(NodeRuntime) + 32768) || !charge(node.name.size(), 8) ||
            !charge(node.custom_type_id.size(), 8))
            return fail(TriggerGraphResult::Budget);
    }
    for (const auto& connection : connections_)
        feedback += connection.feedback ? 1 : 0;
    if (!charge(trigger_audio_.size(), sizeof(detail::TriggerState::AudioState)) ||
        !charge(connections_.size(), 4096) ||
        !charge(ports + feedback, static_cast<std::size_t>(max_frames) * sizeof(float) * 2))
        return fail(TriggerGraphResult::Budget);
    try {
        if (!trigger_controller_)
            return fail(TriggerGraphResult::Binding);
        if (!trigger_controller_->reserve(reservation))
            return fail(TriggerGraphResult::Budget);
        std::shared_ptr<detail::TriggerState> state;
        try {
            state = std::make_shared<detail::TriggerState>();
        } catch (...) {
            trigger_controller_->generations.fetch_sub(reservation, std::memory_order_acq_rel);
            throw;
        }
        state->controller = trigger_controller_;
        state->reserved_bytes = reservation;
        state->edge = edge;
        state->producer_config = trigger_producers_.at(edge.producer);
        state->envelope_config = trigger_envelopes_.at(edge.consumer);
        state->max_frames = static_cast<std::uint32_t>(max_frames);
        state->audio.reserve(trigger_audio_.size());
        for (const auto& [id, config] : trigger_audio_)
            state->audio.push_back({id, config, 0});
        auto candidate = std::make_shared<SignalGraph>();
        candidate->nodes_ = nodes_;
        for (auto& node : candidate->nodes_)
            node.custom_instance.reset();
        candidate->connections_ = connections_;
        candidate->connection_identities_ = connection_identities_;
        // Framework callbacks are bound after compilation; opaque registries are not copied.
        candidate->custom_registry_generation_ = custom_registry_generation_;
        candidate->limits_ = limits_;
        candidate->compiling_trigger_state_ = state;
        // The private candidate never owns the public controller: its ordinary
        // prepare/unpublish lifecycle cannot invalidate the currently live plan.
        if (!candidate->prepare(sample_rate, max_frames))
            return fail(TriggerGraphResult::DependencyCycle);
        auto graph = candidate->live_slot_.live();
        if (!graph || !graph->routed.serial.valid || !graph->trigger_state)
            return fail(TriggerGraphResult::Binding);
        if (graph->routed.serial.snapshot.buffer_assignment().has_delay ||
            graph->routed.serial.pool.slot_count() > ports + feedback)
            return fail(TriggerGraphResult::Budget);
        candidate->live_slot_.unpublish();
        candidate->live_slot_.reclaim_if_quiescent();
        candidate->compiling_trigger_state_.reset();
        graph->trigger_preparation_owner = candidate;
        auto impl = std::make_unique<PreparedTriggerPlan::Impl>();
        impl->owner = this;
        impl->authoring_generation = authoring_generation_;
        impl->prepared_owner = std::move(candidate);
        impl->graph = std::move(graph);
        return {TriggerGraphResult::Accepted,
                std::unique_ptr<PreparedTriggerPlan>(new PreparedTriggerPlan(std::move(impl)))};
    } catch (const std::bad_alloc&) {
        return fail(TriggerGraphResult::Budget);
    }
}

TriggerGraphResult
SignalGraph::try_install_prepared_trigger_plan(std::unique_ptr<PreparedTriggerPlan>& candidate) {
    if (!trigger_controller_)
        return TriggerGraphResult::Binding;
    detail::Gate gate(trigger_controller_->gate);
    if (!gate)
        return TriggerGraphResult::Busy;
    GraphMutationLock lock(*this, std::try_to_lock);
    if (!lock.owns_lock())
        return TriggerGraphResult::Busy;
    if (!candidate || !candidate->impl_ || candidate->impl_->owner != this ||
        !candidate->impl_->graph || !candidate->impl_->graph->trigger_state)
        return TriggerGraphResult::Binding;
    auto& prepared = *candidate->impl_;
    if (prepared.authoring_generation != authoring_generation_)
        return TriggerGraphResult::StaleGeneration;
    if (!canonical_executor_routing_enabled() || parallel_routing_enabled() ||
        anticipation_enabled() || in_swap_edit_)
        return TriggerGraphResult::Mode;
    if (trigger_controller_->epoch == std::numeric_limits<std::uint64_t>::max())
        return TriggerGraphResult::ClockExhausted;
    try {
        live_slot_.prepare_publish();
    } catch (const std::bad_alloc&) {
        return TriggerGraphResult::Budget;
    }
    auto graph = std::move(prepared.graph);
    graph->trigger_state->reset(++trigger_controller_->epoch);
    reset_trigger_audio_(*graph);
    trigger_controller_->active.store(graph.get(), std::memory_order_release);
    publish_prepared_stats_locked_(*graph);
    total_latency_samples_.store(graph->total_latency_samples, std::memory_order_relaxed);
    live_slot_.publish_prepared(std::move(graph));
    candidate.reset();
    return TriggerGraphResult::Accepted;
}

void SignalGraph::reset_trigger_audio_(CompiledGraph& graph) noexcept {
    auto& pool = graph.routed.serial.pool;
    for (std::uint32_t slot = 0; slot < pool.slot_count(); ++slot)
        std::fill_n(pool.slot_data(slot), pool.max_frames(), 0.0f);
    for (std::uint32_t index = 0; index < graph.routed.serial.snapshot.plan().connections.size();
         ++index) {
        auto ring = pool.delay_ring(index);
        if (ring.data)
            std::fill_n(ring.data, ring.size, 0.0f);
        if (ring.write_pos)
            *ring.write_pos = 0;
    }
    for (auto& [id, runtime] : graph.runtime) {
        (void)id;
        std::fill(runtime.output_data.begin(), runtime.output_data.end(), 0.0f);
        std::fill(runtime.input_data.begin(), runtime.input_data.end(), 0.0f);
    }
    for (auto& edge : graph.connection_delays) {
        std::fill(edge.feedback_prev.begin(), edge.feedback_prev.end(), 0.0f);
        if (edge.state) {
            std::fill(edge.state->ring.begin(), edge.state->ring.end(), 0.0f);
            edge.state->write_pos = 0;
        }
        edge.slew_last_value = 0;
        edge.slew_primed = false;
    }
}
TriggerGraphResult SignalGraph::try_reset_events() noexcept {
    if (!trigger_controller_)
        return TriggerGraphResult::Binding;
    detail::Gate gate(trigger_controller_->gate);
    if (!gate)
        return TriggerGraphResult::Busy;
    auto pin = live_slot_.read();
    if (!pin || !pin->trigger_state ||
        trigger_controller_->active.load(std::memory_order_acquire) != pin.get())
        return TriggerGraphResult::StaleGeneration;
    if (trigger_controller_->epoch == std::numeric_limits<std::uint64_t>::max())
        return TriggerGraphResult::ClockExhausted;
    pin->trigger_state->reset(++trigger_controller_->epoch);
    reset_trigger_audio_(*pin);
    return TriggerGraphResult::Accepted;
}
TriggerGraphResult SignalGraph::process_checked(audio::BufferView<float>& output,
                                                const audio::BufferView<const float>& input,
                                                int frames) {
    if (!typed_trigger_authoring_.load(std::memory_order_acquire))
        return TriggerGraphResult::Binding;
    auto pin = live_slot_.read();
    return process_trigger_snapshot_(output, input, frames, nullptr, pin.get());
}
TriggerGraphResult SignalGraph::process_trigger_snapshot_(
    audio::BufferView<float>& output, const audio::BufferView<const float>& input, int frames,
    const format::ProcessContext* transport, CompiledGraph* graph) {
    if (!trigger_controller_ || !graph || !graph->trigger_state)
        return TriggerGraphResult::Binding;
    detail::Gate gate(trigger_controller_->gate);
    if (!gate)
        return TriggerGraphResult::Busy;
    auto current = live_slot_.read();
    if (current.get() != graph ||
        trigger_controller_->active.load(std::memory_order_acquire) != graph)
        return TriggerGraphResult::StaleGeneration;
    auto& state = *graph->trigger_state;
    if (!canonical_executor_routing_enabled() || parallel_routing_enabled() ||
        anticipation_enabled() || (transport && transport->reset_requested))
        return TriggerGraphResult::Mode;
    if (frames < 1 || frames > 4096 || frames > graph->max_block_size ||
        static_cast<std::uint32_t>(frames) > state.max_frames)
        return TriggerGraphResult::Frames;
    const auto count = static_cast<std::uint64_t>(frames);
    if (state.counters.cursor > std::numeric_limits<std::uint64_t>::max() - count ||
        state.counters.sequence > std::numeric_limits<std::uint64_t>::max() - count * 16)
        return TriggerGraphResult::ClockExhausted;
    if (!graph->routed.serial.valid || !graph->routed.serial.snapshot.valid() ||
        !graph->routed.serial.pool.fits(graph->routed.serial.snapshot, frames) ||
        !state.controller->arena)
        return TriggerGraphResult::Binding;
    if (input.num_samples() < count || output.num_samples() < count ||
        input.num_channels() < state.producer_config.input_channels || input.num_channels() > 64 ||
        output.num_channels() == 0 || output.num_channels() > 64 || !input.has_valid_channels() ||
        !output.has_valid_channels())
        return TriggerGraphResult::InvalidBuffers;
    auto exact_output = output.slice(0, count);
    const auto exact_input = input.slice(0, count);
    format::BusBufferSet validation_buses;
    if (!validation_buses.add_input("main", exact_input, format::BusRole::Main) ||
        !validation_buses.add_output("main", exact_output, format::BusRole::Main))
        return TriggerGraphResult::InvalidBuffers;
    format::ProcessBlock validation_block;
    validation_block.sample_rate = graph->sample_rate;
    validation_block.frame_count = static_cast<std::uint32_t>(frames);
    validation_block.buses = &validation_buses;
    if (!validation_block.validate())
        return TriggerGraphResult::InvalidBuffers;
    // All refusals above precede command draining, telemetry and output writes.
    state.trace_size = state.sample_begin = 0;
    const auto failures = routed_only_execution_failures_.load(std::memory_order_relaxed);
    process_snapshot_impl(exact_output, exact_input, frames, transport, graph, true);
    state.counters.cursor += count;
    if (routed_only_execution_failures_.load(std::memory_order_relaxed) != failures)
        return TriggerGraphResult::ProcessingFailure;
    return TriggerGraphResult::Accepted;
}
TriggerGraphResult SignalGraph::trigger_counters(TriggerCounters& counters) const noexcept {
    if (!trigger_controller_)
        return TriggerGraphResult::Binding;
    detail::Gate gate(trigger_controller_->gate);
    if (!gate)
        return TriggerGraphResult::Busy;
    auto pin = live_slot_.read();
    if (!pin || !pin->trigger_state ||
        trigger_controller_->active.load(std::memory_order_acquire) != pin.get())
        return TriggerGraphResult::StaleGeneration;
    counters = pin->trigger_state->counters;
    return TriggerGraphResult::Accepted;
}
TriggerResourceReport SignalGraph::trigger_resources() const {
    GraphMutationLock lock(*this);
    if (!trigger_controller_)
        return {};
    return {sizeof(detail::TriggerArena) + detail::owner_overhead,
            trigger_controller_->generations.load(std::memory_order_acquire),
            sizeof(detail::TriggerController) + detail::owner_overhead,
            detail::authored_descriptor_reservation};
}
std::span<const TriggerRecord> SignalGraph::trigger_trace() const noexcept {
    auto pin = live_slot_.read();
    if (!pin || !pin->trigger_state || !trigger_controller_ ||
        trigger_controller_->active.load(std::memory_order_acquire) != pin.get())
        return {};
    return {trigger_controller_->arena->records.data(), pin->trigger_state->trace_size};
}

void SignalGraph::trigger_test_clock_(std::uint64_t cursor, std::uint64_t sequence,
                                      std::uint64_t epoch, std::uint8_t phase, bool saturate) {
    auto pin = live_slot_.read();
    auto& state = *pin->trigger_state;
    state.reset(epoch);
    state.counters.cursor = cursor;
    state.current_sample = cursor;
    state.counters.sequence = sequence;
    state.phase = phase;
    if (saturate) {
        state.counters.overflow = std::numeric_limits<std::uint64_t>::max();
        state.counters.diagnostic_refusals = std::numeric_limits<std::uint64_t>::max();
    }
    trigger_controller_->epoch = epoch;
}
TriggerEmitResult SignalGraph::trigger_test_emit_(std::int64_t target, std::uint64_t epoch,
                                                  std::uint8_t port) {
    auto pin = live_slot_.read();
    return pin->trigger_state->emit_at({42}, epoch, port, static_cast<std::uint64_t>(target),
                                       target < 0);
}
void SignalGraph::trigger_test_gate_(bool held) {
    if (held)
        (void)trigger_controller_->gate.test_and_set(std::memory_order_acquire);
    else
        trigger_controller_->gate.clear(std::memory_order_release);
}
bool SignalGraph::trigger_test_budget_(std::size_t total) {
    const auto report = trigger_resources();
    if (total < report.total())
        return false;
    const auto amount = total - report.total();
    if (!trigger_controller_->reserve(amount))
        return false;
    trigger_controller_->generations.fetch_sub(amount, std::memory_order_acq_rel);
    return true;
}
} // namespace pulp::host
