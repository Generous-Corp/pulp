// SignalGraph prepared-topology transactions.
//
// Unlike begin_swap_edit(), this path never mutates the owner before success.
// A private SignalGraph carries the complete candidate authoring state and is
// compiled off-side. Commit moves that state and publishes its snapshot while
// holding the owner's control lock, so audio observes exactly the old or new
// compiled graph and control readers cannot observe a mixed authoring graph.

#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <thread>
#include <utility>

namespace pulp::host {
namespace {

std::string prepared_custom_key(std::string_view type_id, int version) {
    std::string key(type_id);
    key.push_back('\x1f');
    key += std::to_string(version);
    return key;
}

bool valid_prepared_custom_type(const CustomNodeType& type) {
    return !type.type_id.empty() && type.version > 0 && type.num_input_ports >= 0 &&
           type.num_output_ports >= 0;
}

bool has_feedback(const std::vector<Connection>& connections) {
    return std::any_of(connections.begin(), connections.end(),
                       [](const Connection& c) { return c.feedback; });
}

} // namespace

std::unique_ptr<SignalGraph::PreparedTopologyEdit> SignalGraph::begin_prepared_topology_edit() {
    return std::unique_ptr<PreparedTopologyEdit>(new PreparedTopologyEdit(*this));
}

SignalGraph::PreparedTopologyEdit::PreparedTopologyEdit(SignalGraph& owner)
    : owner_(&owner), candidate_(std::make_unique<SignalGraph>()) {
    GraphMutationLock owner_lock(owner);

    candidate_->nodes_ = owner.nodes_;
    candidate_->connections_ = owner.connections_;
    candidate_->connection_identities_ = owner.connection_identities_;
    candidate_->next_connection_identity_ = owner.next_connection_identity_;
    candidate_->custom_node_types_ = owner.custom_node_types_;
    candidate_->custom_registry_generation_ = owner.custom_registry_generation_;
    candidate_->next_id_ = owner.next_id_;
    candidate_->limits_ = owner.limits_;
    candidate_->prepared_plugin_meta_ = owner.prepared_plugin_meta_;
    candidate_->canonical_executor_routing_enabled_.store(
        owner.canonical_executor_routing_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    candidate_->parallel_routing_enabled_.store(
        owner.parallel_routing_enabled_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    candidate_->anticipation_enabled_.store(
        owner.anticipation_enabled_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    candidate_->desired_live_dsp_telemetry_enabled_.store(
        owner.desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    candidate_->prepared_edit_origin_ = &owner;

    baseline_node_ids_.reserve(owner.nodes_.size());
    for (const auto& n : owner.nodes_)
        baseline_node_ids_.insert(n.id);
    baseline_registry_keys_.reserve(owner.custom_node_types_.size());
    for (const auto& [key, _] : owner.custom_node_types_) {
        baseline_registry_keys_.insert(key);
    }

    base_authoring_generation_ = owner.authoring_generation_;
    base_live_ = owner.live_slot_.live();
    base_canonical_routing_ =
        owner.canonical_executor_routing_enabled_.load(std::memory_order_relaxed);
    base_parallel_routing_ = owner.parallel_routing_enabled_.load(std::memory_order_relaxed);
    base_anticipation_ = owner.anticipation_enabled_.load(std::memory_order_relaxed);
}

SignalGraph::PreparedTopologyEdit::~PreparedTopologyEdit() {
    prepared_snapshot_.reset();
    if (!committed_) {
        GraphMutationLock owner_lock(*owner_);
        (void)rollback_quiesced_lifecycles_locked_();
        owner_lock.unlock();
        release_new_custom_instances_();
    }
}

bool SignalGraph::PreparedTopologyEdit::base_is_current_locked_() const {
    owner_->assert_graph_mutation_locked_();
    return owner_->authoring_generation_ == base_authoring_generation_ &&
           owner_->live_slot_.live() == base_live_ &&
           owner_->canonical_executor_routing_enabled_.load(std::memory_order_relaxed) ==
               base_canonical_routing_ &&
           owner_->parallel_routing_enabled_.load(std::memory_order_relaxed) ==
               base_parallel_routing_ &&
           owner_->anticipation_enabled_.load(std::memory_order_relaxed) == base_anticipation_;
}

bool SignalGraph::PreparedTopologyEdit::is_new_node_(NodeId id) const {
    return baseline_node_ids_.find(id) == baseline_node_ids_.end();
}

std::optional<SignalGraph::PreparedTopologyEdit::Result>
SignalGraph::PreparedTopologyEdit::baseline_removal_rejection_locked_() const {
    owner_->assert_graph_mutation_locked_();
    for (const auto& baseline : owner_->nodes_) {
        const bool retained = std::any_of(
            candidate_->nodes_.begin(), candidate_->nodes_.end(),
            [&](const GraphNode& node) { return node.id == baseline.id; });
        if (retained)
            continue;
        if (baseline.type == NodeType::Plugin)
            return Result::BaselinePluginRemovalRequiresRelease;
        if (baseline.type == NodeType::Custom) {
            const auto type = owner_->custom_node_types_.find(
                prepared_custom_key(baseline.custom_type_id, baseline.custom_type_version));
            if (type != owner_->custom_node_types_.end() && type->second.release)
                return Result::BaselineCustomRemovalRequiresRelease;
        }
    }
    return std::nullopt;
}

bool SignalGraph::PreparedTopologyEdit::rollback_quiesced_lifecycles_locked_() noexcept {
    owner_->assert_graph_mutation_locked_();
    if (!quiesced_lifecycles_dirty_)
        return true;

    bool restored = true;
    if (base_live_ != nullptr) {
        for (const auto& retained : quiesced_plugins_) {
            if (!retained.touched)
                continue;
            try {
                if (!retained.plugin->prepare(base_live_->sample_rate,
                                              base_live_->max_block_size)) {
                    restored = false;
                }
            } catch (...) {
                restored = false;
            }
        }
        for (const auto& retained : quiesced_customs_) {
            if (!retained.touched)
                continue;
            if (!retained.prepare)
                continue;
            try {
                retained.prepare(retained.instance.get(), base_live_->sample_rate,
                                 base_live_->max_block_size);
            } catch (...) {
                restored = false;
            }
        }
    } else {
        // The base graph had no prepared lifecycle. Undo only candidate prepare
        // callbacks that were entered; untouched retained objects were never
        // acquired and must not receive a release callback.
        for (const auto& retained : quiesced_plugins_) {
            if (!retained.touched)
                continue;
            try {
                retained.plugin->release();
            } catch (...) {
                restored = false;
            }
        }
        for (const auto& retained : quiesced_customs_) {
            if (!retained.touched)
                continue;
            if (!retained.release) {
                restored = false;
                continue;
            }
            try {
                retained.release(retained.instance.get());
            } catch (...) {
                restored = false;
            }
        }
    }

    quiesced_lifecycles_dirty_ = false;
    prepared_snapshot_.reset();

    // A different live generation cannot safely keep running after its shared
    // objects were restored to this edit's older base dimensions.
    if (owner_->live_slot_.live() != base_live_)
        restored = false;
    if (!restored) {
        owner_->live_slot_.unpublish();
        owner_->total_latency_samples_.store(0, std::memory_order_relaxed);
        owner_->clear_prepared_stats_locked_();
    }
    return restored;
}

void SignalGraph::PreparedTopologyEdit::release_new_custom_instances_() noexcept {
    if (!candidate_)
        return;
    GraphMutationLock candidate_lock(*candidate_);
    for (NodeId id : prepared_new_custom_ids_) {
        auto* n = candidate_->node_mut_locked_(id);
        if (n == nullptr || n->type != NodeType::Custom || !n->custom_instance) {
            continue;
        }
        const auto* type = candidate_->custom_node_type(n->custom_type_id, n->custom_type_version);
        if (type != nullptr && type->release) {
            type->release(n->custom_instance.get());
        }
    }
    prepared_new_custom_ids_.clear();
}

bool SignalGraph::PreparedTopologyEdit::register_custom_node_type(CustomNodeType type) {
    if (mutation_failed_ || committed_ || prepare_attempted_ || !valid_prepared_custom_type(type)) {
        mutation_failed_ = true;
        return false;
    }
    const auto key = prepared_custom_key(type.type_id, type.version);
    if (baseline_registry_keys_.count(key) != 0) {
        replaced_registry_keys_.insert(key);
    }
    const bool ok = candidate_->register_custom_node_type(std::move(type));
    if (!ok)
        mutation_failed_ = true;
    return ok;
}

bool SignalGraph::PreparedTopologyEdit::unregister_custom_node_type(std::string_view type_id,
                                                                    int version) {
    return mutate_([&] {
        GraphMutationLock candidate_lock(*candidate_);
        const auto key = prepared_custom_key(type_id, version);
        const auto found = candidate_->custom_node_types_.find(key);
        if (found == candidate_->custom_node_types_.end())
            return false;
        const bool in_use = std::any_of(
            candidate_->nodes_.begin(), candidate_->nodes_.end(), [&](const GraphNode& node) {
                return node.type == NodeType::Custom && node.custom_type_id == type_id &&
                       node.custom_type_version == version;
            });
        if (in_use)
            return false;
        candidate_->custom_node_types_.erase(found);
        ++candidate_->custom_registry_generation_;
        candidate_->invalidate_live_locked_();
        return true;
    });
}

std::size_t SignalGraph::PreparedTopologyEdit::prune_unused_custom_node_types() {
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        mutation_failed_ = true;
        return 0;
    }
    GraphMutationLock candidate_lock(*candidate_);
    std::unordered_set<std::string> used;
    used.reserve(candidate_->nodes_.size());
    for (const auto& n : candidate_->nodes_) {
        if (n.type == NodeType::Custom) {
            used.insert(prepared_custom_key(n.custom_type_id, n.custom_type_version));
        }
    }
    std::size_t removed = 0;
    for (auto it = candidate_->custom_node_types_.begin();
         it != candidate_->custom_node_types_.end();) {
        if (used.count(it->first) == 0) {
            it = candidate_->custom_node_types_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed != 0) {
        ++candidate_->custom_registry_generation_;
        candidate_->invalidate_live_locked_();
    }
    return removed;
}

std::size_t SignalGraph::PreparedTopologyEdit::custom_node_type_count() const {
    GraphMutationLock candidate_lock(*candidate_);
    return candidate_->custom_node_types_.size();
}

NodeId SignalGraph::PreparedTopologyEdit::add_input_node(int channels, const std::string& name) {
    return add_node_([&] { return candidate_->add_input_node(channels, name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_output_node(int channels, const std::string& name) {
    return add_node_([&] { return candidate_->add_output_node(channels, name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_gain_node(const std::string& name) {
    return add_node_([&] { return candidate_->add_gain_node(name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_midi_input_node(const std::string& name) {
    return add_node_([&] { return candidate_->add_midi_input_node(name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_midi_output_node(const std::string& name) {
    return add_node_([&] { return candidate_->add_midi_output_node(name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_custom_node(std::string_view type_id,
                                                          const std::string& name) {
    return add_node_([&] { return candidate_->add_custom_node(type_id, name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_custom_node(std::string_view type_id, int version,
                                                          const std::string& name) {
    return add_node_([&] { return candidate_->add_custom_node(type_id, version, name); });
}

NodeId SignalGraph::PreparedTopologyEdit::add_unresolved_custom_node(std::string_view type_id,
                                                                     int version, int num_inputs,
                                                                     int num_outputs,
                                                                     const std::string& name) {
    return add_node_([&] {
        return candidate_->add_unresolved_custom_node(type_id, version, num_inputs, num_outputs,
                                                      name);
    });
}

bool SignalGraph::PreparedTopologyEdit::remove_node(NodeId id) {
    return mutate_([&] { return candidate_->remove_node(id); });
}

bool SignalGraph::PreparedTopologyEdit::connect(NodeId source, PortIndex source_port, NodeId dest,
                                                PortIndex dest_port) {
    return mutate_([&] { return candidate_->connect(source, source_port, dest, dest_port); });
}

bool SignalGraph::PreparedTopologyEdit::connect_feedback(NodeId source, PortIndex source_port,
                                                         NodeId dest, PortIndex dest_port) {
    return mutate_(
        [&] { return candidate_->connect_feedback(source, source_port, dest, dest_port); });
}

bool SignalGraph::PreparedTopologyEdit::connect_midi(NodeId source, NodeId dest) {
    return mutate_([&] { return candidate_->connect_midi(source, dest); });
}

bool SignalGraph::PreparedTopologyEdit::disconnect(NodeId source, PortIndex source_port,
                                                   NodeId dest, PortIndex dest_port) {
    return mutate_([&] { return candidate_->disconnect(source, source_port, dest, dest_port); });
}

bool SignalGraph::PreparedTopologyEdit::set_node_gain(NodeId id, float linear_gain) {
    return mutate_([&] { return candidate_->set_node_gain(id, linear_gain); });
}

void SignalGraph::PreparedTopologyEdit::set_canonical_executor_routing_enabled(
    bool enabled) noexcept {
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        mutation_failed_ = true;
        return;
    }
    candidate_->set_canonical_executor_routing_enabled(enabled);
}

void SignalGraph::PreparedTopologyEdit::set_parallel_routing_enabled(bool enabled) noexcept {
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        mutation_failed_ = true;
        return;
    }
    candidate_->set_parallel_routing_enabled(enabled);
}

void SignalGraph::PreparedTopologyEdit::set_anticipation_enabled(bool enabled) noexcept {
    if (mutation_failed_ || committed_ || prepare_attempted_) {
        mutation_failed_ = true;
        return;
    }
    candidate_->set_anticipation_enabled(enabled);
}

const GraphNode* SignalGraph::PreparedTopologyEdit::node(NodeId id) const {
    return candidate_->node(id);
}

const std::vector<GraphNode>& SignalGraph::PreparedTopologyEdit::nodes() const {
    return candidate_->nodes_;
}

const std::vector<Connection>& SignalGraph::PreparedTopologyEdit::connections() const {
    return candidate_->connections_;
}

bool SignalGraph::PreparedTopologyEdit::routed_execution_ready(int block_size) const noexcept {
    if (!prepared_snapshot_ || block_size <= 0)
        return false;
    const auto frames = static_cast<std::uint32_t>(block_size);
    const auto serial_ready = [&] {
        return prepared_snapshot_->routed.serial.valid &&
               prepared_snapshot_->routed.serial.pool.fits(
                   prepared_snapshot_->routed.serial.snapshot, frames);
    };
    const auto parallel_ready = [&] {
        return prepared_snapshot_->routed.parallel.valid &&
               prepared_snapshot_->routed.parallel.pool.fits(
                   prepared_snapshot_->routed.parallel.snapshot, frames);
    };
    switch (prepared_snapshot_->pdc_execution_domain) {
    case PdcExecutionDomain::Legacy:
        return false;
    case PdcExecutionDomain::RoutedSerial:
        return serial_ready();
    case PdcExecutionDomain::RoutedParallel:
        return parallel_ready();
    case PdcExecutionDomain::Dynamic:
        return (candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed) &&
                parallel_ready()) ||
               (candidate_->canonical_executor_routing_enabled_.load(std::memory_order_relaxed) &&
                serial_ready());
    }
    return false;
}

SignalGraph::PreparedTopologyEdit::Result
SignalGraph::PreparedTopologyEdit::prepare(double sample_rate, int max_block_size) {
    if (committed_)
        return last_result_ = Result::AlreadyCommitted;
    if (mutation_failed_)
        return last_result_ = Result::InvalidMutation;
    if (prepared_snapshot_)
        return last_result_ = Result::Prepared;
    if (prepare_attempted_)
        return last_result_;
    prepare_attempted_ = true;

    GraphMutationLock owner_lock(*owner_);
    if (!base_is_current_locked_() || owner_->in_swap_edit_) {
        return last_result_ = Result::StaleBase;
    }

    GraphMutationLock candidate_lock(*candidate_);
    const auto old_keepalive = owner_->live_slot_.live();
    const auto* old = old_keepalive.get();
    const bool candidate_has_midi_output =
        std::any_of(candidate_->nodes_.begin(), candidate_->nodes_.end(),
                    [](const GraphNode& node) { return node.type == NodeType::MidiOutput; });
    const bool live_has_midi_output =
        old != nullptr &&
        std::any_of(old->shapes.begin(), old->shapes.end(),
                    [](const auto& entry) { return entry.second.type == NodeType::MidiOutput; });
    if (candidate_has_midi_output || live_has_midi_output) {
        return last_result_ = Result::MidiOutputSnapshotLocalRequired;
    }

    if (const auto rejection = baseline_removal_rejection_locked_())
        return last_result_ = *rejection;
    if (!candidate_->preflight_locked_(max_block_size) ||
        candidate_->processing_order().size() != candidate_->nodes_.size()) {
        return last_result_ = Result::PreflightFailed;
    }
    const bool dimensions_changed = old != nullptr && (old->sample_rate != sample_rate ||
                                                       old->max_block_size != max_block_size);
    if (old != nullptr) {
        if (old->anticipation.valid ||
            owner_->anticipation_enabled_.load(std::memory_order_relaxed) ||
            candidate_->anticipation_enabled_.load(std::memory_order_relaxed)) {
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }
    }

    // Existing external instances may be shared with an off-side compile, but
    // they must never be prepared or replaced by this transaction.
    for (const auto& n : candidate_->nodes_) {
        if (n.type != NodeType::Plugin || !n.plugin)
            continue;
        if (old == nullptr) {
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }
        if (dimensions_changed) {
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }
        const auto live_plugin = old->plugins.find(n.id);
        if (live_plugin == old->plugins.end() || live_plugin->second.get() != n.plugin.get() ||
            candidate_->prepared_plugin_meta_.count(n.id) == 0) {
            return last_result_ = Result::ExternalPluginReprepareRequired;
        }
    }

    if (!replaced_registry_keys_.empty()) {
        return last_result_ = Result::CustomRegistryConflict;
    }

    // Existing custom instances are immutable identities for this operation.
    // Only nodes minted inside this edit may run create/load/prepare callbacks.
    for (auto& n : candidate_->nodes_) {
        if (n.type != NodeType::Custom || is_new_node_(n.id))
            continue;
        if (n.custom_state_pending) {
            return last_result_ = Result::ExistingCustomReprepareRequired;
        }
        const auto* type = candidate_->custom_node_type(n.custom_type_id, n.custom_type_version);
        if (dimensions_changed && type != nullptr && (type->prepare || type->release)) {
            return last_result_ = Result::ExistingCustomReprepareRequired;
        }
        if (old == nullptr) {
            if (type != nullptr && type->create) {
                return last_result_ = Result::ExistingCustomReprepareRequired;
            }
            continue;
        }
        const auto existing = old->custom_instances.find(n.id);
        if (existing == old->custom_instances.end() ||
            existing->second != n.custom_instance.get()) {
            return last_result_ = Result::ExistingCustomReprepareRequired;
        }
    }

    for (auto& n : candidate_->nodes_) {
        if (n.type != NodeType::Custom || !is_new_node_(n.id))
            continue;
        const auto* type = candidate_->custom_node_type(n.custom_type_id, n.custom_type_version);
        if (type == nullptr || type->num_input_ports != n.num_input_ports ||
            type->num_output_ports != n.num_output_ports || !type->create) {
            continue;
        }
        void* raw = type->create();
        if (raw == nullptr) {
            return last_result_ = Result::CustomInstanceCreateFailed;
        }
        auto destroy = type->destroy;
        n.custom_instance = std::shared_ptr<void>(raw, [destroy](void* p) {
            if (destroy && p)
                destroy(p);
        });
        prepared_new_custom_ids_.push_back(n.id);
        if (n.custom_state_pending && type->load_state) {
            if (!type->load_state(n.custom_instance.get(), n.custom_state_blob)) {
                return last_result_ = Result::CustomStateRestoreFailed;
            }
            n.custom_state_pending = false;
        }
        if (type->prepare) {
            type->prepare(n.custom_instance.get(), sample_rate, max_block_size);
        }
    }

    auto next = candidate_->compile_(sample_rate, max_block_size);
    if (!next)
        return last_result_ = Result::CompileFailed;

    if (old != nullptr) {
        // Adopt stable runtime ingress/egress identities before publication. A
        // control-thread MIDI/parameter batch published during compilation is
        // therefore visible to exactly one audio block across the boundary.
        for (auto& [id, rt] : next->runtime) {
            const auto live_rt = old->runtime.find(id);
            const auto live_shape = old->shapes.find(id);
            const auto next_shape = next->shapes.find(id);
            if (live_rt == old->runtime.end() || live_shape == old->shapes.end() ||
                next_shape == next->shapes.end() ||
                live_shape->second.type != next_shape->second.type) {
                continue;
            }
            if (rt.parameter_input_mailbox && live_rt->second.parameter_input_mailbox) {
                rt.parameter_input_mailbox = live_rt->second.parameter_input_mailbox;
            }
            if (rt.midi_input_mailbox && live_rt->second.midi_input_mailbox) {
                rt.midi_input_mailbox = live_rt->second.midi_input_mailbox;
            }
        }
        const auto rebind_parameter_sequence = [&](CompiledGraph::RoutedPath& path) {
            for (auto& ctx : path.plugin_ctx) {
                const auto rt = next->runtime.find(ctx.node_id);
                ctx.parameter_events_sequence_seen =
                    rt != next->runtime.end() && rt->second.parameter_input_mailbox
                        ? &rt->second.parameter_input_mailbox->sequence_seen
                        : nullptr;
            }
        };
        rebind_parameter_sequence(next->routed.serial);
        rebind_parameter_sequence(next->routed.parallel);

        // A sample-rate/block-size transition intentionally starts fresh DSP
        // history. Stable mailbox identities above still preserve any pending
        // MIDI/parameter publication, but differently-sized PDC/feedback rings
        // are never copied or shared across the boundary.
        if (dimensions_changed) {
            prepared_snapshot_ = std::move(next);
            return last_result_ = Result::Prepared;
        }

        // Feedback owns previous-block samples per snapshot and is not safely
        // adoptable. Feed-forward PDC carries only the unchanged identity+shape
        // intersection: removed rings retire with the old snapshot, newly
        // delayed or reshaped rings start fresh, and no stale history can attach
        // to a disconnect+reconnect edge that minted a new identity.
        if (has_feedback(old->connections) || has_feedback(next->connections) ||
            old->connection_identities.size() != old->connection_delays.size() ||
            next->connection_identities.size() != next->connection_delays.size()) {
            return last_result_ = Result::RuntimeAdoptionFailed;
        }

        std::unordered_map<std::uint64_t, std::size_t> old_delayed;
        for (std::size_t i = 0; i < old->connection_delays.size(); ++i) {
            if (old->connection_delays[i].delay_samples > 0) {
                old_delayed.emplace(old->connection_identities[i], i);
            }
        }
        std::vector<std::pair<std::size_t, std::size_t>> matched;
        for (std::size_t i = 0; i < next->connection_delays.size(); ++i) {
            const auto& delay = next->connection_delays[i];
            if (delay.delay_samples <= 0)
                continue;
            const auto found = old_delayed.find(next->connection_identities[i]);
            if (found == old_delayed.end())
                continue;
            const auto& live_delay = old->connection_delays[found->second];
            if (delay.delay_samples != live_delay.delay_samples || !delay.state ||
                !live_delay.state || delay.state->ring.size() != live_delay.state->ring.size()) {
                continue;
            }
            matched.emplace_back(i, found->second);
        }
        for (const auto [candidate_index, live_index] : matched) {
            bool routed_state_compatible = next->pdc_execution_domain == old->pdc_execution_domain;
            if (routed_state_compatible &&
                next->pdc_execution_domain == PdcExecutionDomain::RoutedSerial) {
                routed_state_compatible =
                    next->routed.serial.valid && old->routed.serial.valid &&
                    next->routed.serial.pool.can_adopt_delay_ring_state(
                        static_cast<std::uint32_t>(candidate_index), old->routed.serial.pool,
                        static_cast<std::uint32_t>(live_index));
            } else if (routed_state_compatible &&
                       next->pdc_execution_domain == PdcExecutionDomain::RoutedParallel) {
                routed_state_compatible =
                    next->routed.parallel.valid && old->routed.parallel.valid &&
                    next->routed.parallel.pool.can_adopt_delay_ring_state(
                        static_cast<std::uint32_t>(candidate_index), old->routed.parallel.pool,
                        static_cast<std::uint32_t>(live_index));
            }
            if (!routed_state_compatible)
                continue;
            next->connection_delays[candidate_index].state =
                old->connection_delays[live_index].state;
            if (next->pdc_execution_domain == PdcExecutionDomain::RoutedSerial) {
                (void)next->routed.serial.pool.adopt_delay_ring_state(
                    static_cast<std::uint32_t>(candidate_index), old->routed.serial.pool,
                    static_cast<std::uint32_t>(live_index));
            }
            if (next->pdc_execution_domain == PdcExecutionDomain::RoutedParallel) {
                (void)next->routed.parallel.pool.adopt_delay_ring_state(
                    static_cast<std::uint32_t>(candidate_index), old->routed.parallel.pool,
                    static_cast<std::uint32_t>(live_index));
            }
        }
    }

    prepared_snapshot_ = std::move(next);
    return last_result_ = Result::Prepared;
}

SignalGraph::PreparedTopologyEdit::Result
SignalGraph::PreparedTopologyEdit::prepare_quiesced(double sample_rate,
                                                     int max_block_size) {
    if (committed_)
        return last_result_ = Result::AlreadyCommitted;
    if (mutation_failed_)
        return last_result_ = Result::InvalidMutation;
    if (prepared_snapshot_)
        return last_result_ = Result::Prepared;
    if (prepare_attempted_)
        return last_result_;
    prepare_attempted_ = true;

    // This path is deliberately separate from prepare(): the caller guarantees
    // that audio processing and anticipation are quiesced, so shared external
    // plugin/custom instances may be re-prepared for new dimensions on the
    // private candidate. The owner's authoring graph and live publication remain
    // unchanged unless the candidate succeeds and commit() is called.
    GraphMutationLock owner_lock(*owner_);
    if (!base_is_current_locked_() || owner_->in_swap_edit_)
        return last_result_ = Result::StaleBase;
    if (const auto rejection = baseline_removal_rejection_locked_())
        return last_result_ = *rejection;

    // Capture the exact retained objects and the owner's original custom
    // lifecycle callbacks before candidate preparation mutates shared state.
    quiesced_plugins_.clear();
    quiesced_customs_.clear();
    for (const auto& node : candidate_->nodes_) {
        if (is_new_node_(node.id))
            continue;
        if (node.plugin) {
            quiesced_plugins_.push_back({node.plugin, false});
            continue;
        }
        if (node.type != NodeType::Custom || !node.custom_instance)
            continue;
        const auto owner_node = std::find_if(
            owner_->nodes_.begin(), owner_->nodes_.end(),
            [&](const GraphNode& current) {
                return current.id == node.id && current.custom_instance == node.custom_instance;
            });
        if (owner_node == owner_->nodes_.end())
            continue;
        if (replaced_registry_keys_.count(
                prepared_custom_key(node.custom_type_id, node.custom_type_version)) != 0) {
            return last_result_ = Result::ExistingCustomReprepareRequired;
        }
        const auto* candidate_type = candidate_->custom_node_type(
            node.custom_type_id, node.custom_type_version);
        const auto* owner_type = owner_->custom_node_type(
            owner_node->custom_type_id, owner_node->custom_type_version);
        if (candidate_type != nullptr && candidate_type->prepare && owner_type != nullptr) {
            quiesced_customs_.push_back(
                {node.custom_instance, owner_type->prepare, owner_type->release,
                 false});
        }
    }
    quiesced_lifecycles_dirty_ =
        !quiesced_plugins_.empty() || !quiesced_customs_.empty();

    bool candidate_prepared = false;
    const SignalGraph::PrepareLifecycleObserver lifecycle_observer{
        this,
        [](void* context, PluginSlot* plugin) noexcept {
            auto& edit = *static_cast<PreparedTopologyEdit*>(context);
            for (auto& retained : edit.quiesced_plugins_) {
                if (retained.plugin.get() == plugin && !retained.touched) {
                    retained.touched = true;
                    return;
                }
            }
        },
        [](void* context, void* instance) noexcept {
            auto& edit = *static_cast<PreparedTopologyEdit*>(context);
            for (auto& retained : edit.quiesced_customs_) {
                if (retained.instance.get() == instance && !retained.touched) {
                    retained.touched = true;
                    return;
                }
            }
        },
    };
    try {
        candidate_prepared = candidate_->prepare_impl_(
            sample_rate, max_block_size, &lifecycle_observer);
    } catch (...) {
        candidate_prepared = false;
    }

    // candidate_->prepare() may create some new custom instances before a
    // later plugin/custom prepare fails. Remember them even on failure so an
    // abandoned edit still balances every successful create/prepare with the
    // control-thread release callback.
    prepared_new_custom_ids_.clear();
    for (const auto& node : candidate_->nodes_) {
        if (node.type != NodeType::Custom || !node.custom_instance)
            continue;
        const auto owner_node = std::find_if(
            owner_->nodes_.begin(), owner_->nodes_.end(),
            [&](const GraphNode& current) { return current.id == node.id; });
        if (owner_node == owner_->nodes_.end() ||
            owner_node->custom_instance != node.custom_instance) {
            prepared_new_custom_ids_.push_back(node.id);
        }
    }
    if (!candidate_prepared) {
        if (!rollback_quiesced_lifecycles_locked_())
            return last_result_ = Result::QuiescedRollbackFailed;
        return last_result_ = Result::ExternalPluginReprepareRequired;
    }

    prepared_snapshot_ = candidate_->live_slot_.live();
    if (!prepared_snapshot_) {
        if (!rollback_quiesced_lifecycles_locked_())
            return last_result_ = Result::QuiescedRollbackFailed;
        return last_result_ = Result::CompileFailed;
    }

    return last_result_ = Result::Prepared;
}

SignalGraph::PreparedTopologyEdit::Result SignalGraph::PreparedTopologyEdit::commit() {
    if (committed_)
        return last_result_ = Result::AlreadyCommitted;
    if (!prepared_snapshot_)
        return last_result_ = Result::NotPrepared;

    GraphMutationLock owner_lock(*owner_);
    const auto fail_before_commit = [&](Result result) {
        if (!rollback_quiesced_lifecycles_locked_())
            return last_result_ = Result::QuiescedRollbackFailed;
        return last_result_ = result;
    };
    if (!base_is_current_locked_() || owner_->in_swap_edit_) {
        return fail_before_commit(Result::StaleBase);
    }

    // Everything above the irreversible authoring move is an exception
    // boundary. Reserve both generic publication retirement and destination
    // load-map buckets now. After this point publication is noexcept and load
    // measurers move as allocation-free C++17 node handles.
    try {
        {
            std::scoped_lock load_locks(owner_->node_load_mu_, candidate_->node_load_mu_);
            std::size_t incoming = 0;
            for (const auto& [id, measurer] : candidate_->node_load_) {
                if (measurer && owner_->node_load_.find(id) == owner_->node_load_.end())
                    ++incoming;
            }
            owner_->node_load_.reserve(owner_->node_load_.size() + incoming);
        }
        owner_->live_slot_.prepare_publish();

        if (candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed) &&
            prepared_snapshot_->routed.parallel.valid &&
            owner_->worker_pool_.worker_count() == 0) {
            const unsigned hw = std::thread::hardware_concurrency();
            const std::uint32_t workers =
                std::clamp<std::uint32_t>(hw == 0 ? 2 : hw, 2, 16);
            if (!owner_->worker_pool_.start(workers))
                return fail_before_commit(Result::ParallelWorkerStartFailed);
        }
        if (candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed) &&
            prepared_snapshot_->routed.parallel.valid && !owner_->worker_pool_.running()) {
            return fail_before_commit(Result::ParallelWorkerStartFailed);
        }
    } catch (...) {
        if (!rollback_quiesced_lifecycles_locked_())
            return last_result_ = Result::QuiescedRollbackFailed;
        throw;
    }

    // Transfer only load measurers allocated for genuinely new/reused IDs.
    // std::unordered_map<NodeId, unique_ptr<...>> uses the same default
    // allocator, integral hash, and equality on both sides; destination buckets
    // were reserved above. Extract/insert therefore transfers ownership and
    // preserves the addresses captured by the compiled snapshot without an
    // allocation after the exception boundary.
    {
        std::scoped_lock load_locks(owner_->node_load_mu_, candidate_->node_load_mu_);
        for (auto it = candidate_->node_load_.begin(); it != candidate_->node_load_.end();) {
            if (!it->second || owner_->node_load_.find(it->first) != owner_->node_load_.end()) {
                ++it;
                continue;
            }
            auto current = it++;
            auto node = candidate_->node_load_.extract(current);
            [[maybe_unused]] const auto inserted =
                owner_->node_load_.insert(std::move(node));
            assert(inserted.inserted);
        }
    }

    owner_->cancel_swap_edit_locked_();
    owner_->nodes_ = std::move(candidate_->nodes_);
    owner_->connections_ = std::move(candidate_->connections_);
    owner_->connection_identities_ = std::move(candidate_->connection_identities_);
    owner_->next_connection_identity_ = candidate_->next_connection_identity_;
    owner_->custom_node_types_ = std::move(candidate_->custom_node_types_);
    owner_->custom_registry_generation_ = candidate_->custom_registry_generation_;
    owner_->next_id_ = candidate_->next_id_;
    owner_->limits_ = candidate_->limits_;
    owner_->prepared_plugin_meta_ = std::move(candidate_->prepared_plugin_meta_);
    owner_->canonical_executor_routing_enabled_.store(
        candidate_->canonical_executor_routing_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    owner_->parallel_routing_enabled_.store(
        candidate_->parallel_routing_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    owner_->anticipation_enabled_.store(
        candidate_->anticipation_enabled_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    owner_->transport_suppressed_for_anticipation_.store(
        candidate_->transport_suppressed_for_anticipation_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

    // The telemetry toggle is an independent control setting, so it does not
    // stale a topology edit. Re-seed from the owner's authoritative
    // desired value at the publication boundary instead of restoring the value
    // captured when the candidate was created.
    prepared_snapshot_->live_dsp_telemetry.set_enabled(
        owner_->desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed));
    owner_->live_slot_.publish_prepared(prepared_snapshot_);
    owner_->total_latency_samples_.store(prepared_snapshot_->total_latency_samples,
                                         std::memory_order_relaxed);
    owner_->publish_prepared_stats_locked_(*prepared_snapshot_);
    ++owner_->authoring_generation_;

    committed_snapshot_ = prepared_snapshot_;
    prepared_snapshot_.reset();
    prepared_new_custom_ids_.clear();
    quiesced_lifecycles_dirty_ = false;
    quiesced_plugins_.clear();
    quiesced_customs_.clear();
    committed_ = true;
    return last_result_ = Result::Committed;
}

} // namespace pulp::host
