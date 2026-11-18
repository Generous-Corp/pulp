// SPDX-License-Identifier: MIT
//
// Admission gates for PreparedTopologyEdit::prepare().
// See signal_graph_prepared_edit_admission.hpp for the contract.

#include "signal_graph_prepared_edit_admission.hpp"

#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>

namespace pulp::host {

// Every refusal returns the Result prepare() records; std::nullopt admits the
// candidate. The candidate lock is acquired here, in the same order as the
// rest of the transaction, and stays held by the caller after return.
std::optional<SignalGraph::PreparedTopologyEdit::Result>
SignalGraph::PreparedTopologyEdit::admission_result_(
    double sample_rate, int max_block_size, std::optional<GraphMutationLock>& candidate_lock,
    std::shared_ptr<CompiledGraph>& old_keepalive, bool& dimensions_changed) {
    if (!base_is_current_locked_() || owner_->in_swap_edit_) {
        return Result::StaleBase;
    }
    if (owner_->execution_selection_generation_.load(std::memory_order_relaxed) ==
            std::numeric_limits<std::uint64_t>::max() ||
        candidate_->execution_selection_generation_.load(std::memory_order_relaxed) ==
            std::numeric_limits<std::uint64_t>::max()) {
        return Result::PreflightFailed;
    }

    if (owner_->spectral_mode_.load(std::memory_order_acquire)) {
        const auto unsupported = [](const auto& nodes) {
            return std::any_of(nodes.begin(), nodes.end(), [](const auto& n) {
                return n.type == NodeType::Plugin || n.type == NodeType::Custom;
            });
        };
        if (unsupported(owner_->nodes_) || unsupported(candidate_->nodes_)) {
            owner_->spectral_error_.store(SpectralError::UnsupportedTransactionalReplacement,
                                          std::memory_order_relaxed);
            return Result::PreflightFailed;
        }
    }
    candidate_lock.emplace(*candidate_);
    if (const auto rejection = sample_region_preparation_result_(sample_rate, max_block_size))
        return *rejection;
    old_keepalive = owner_->spectral_mode_.load(std::memory_order_acquire)
                        ? std::shared_ptr<CompiledGraph>{}
                        : owner_->live_slot_.live();
    const auto* old = old_keepalive.get();
    const bool candidate_has_midi_output =
        std::any_of(candidate_->nodes_.begin(), candidate_->nodes_.end(),
                    [](const GraphNode& node) { return node.type == NodeType::MidiOutput; });
    const bool live_has_midi_output =
        old != nullptr &&
        std::any_of(old->shapes.begin(), old->shapes.end(),
                    [](const auto& entry) { return entry.second.type == NodeType::MidiOutput; });
    if (!owner_->spectral_mode_.load(std::memory_order_acquire) &&
        (candidate_has_midi_output || live_has_midi_output)) {
        return Result::MidiOutputSnapshotLocalRequired;
    }

    if (const auto rejection = baseline_removal_rejection_locked_())
        return *rejection;
    if (!candidate_->preflight_locked_(max_block_size) ||
        (candidate_->sample_region_definitions_.empty() &&
         candidate_->processing_order().size() != candidate_->nodes_.size())) {
        return Result::PreflightFailed;
    }
    dimensions_changed = old != nullptr &&
                         (old->sample_rate != sample_rate || old->max_block_size != max_block_size);
    if (old != nullptr) {
        if (old->anticipation.valid ||
            owner_->anticipation_enabled_.load(std::memory_order_relaxed) ||
            candidate_->anticipation_enabled_.load(std::memory_order_relaxed)) {
            return Result::ExternalPluginReprepareRequired;
        }
    }

    for (const auto& [id, processor] : candidate_->processor_nodes_) {
        if (!processor)
            return Result::PreflightFailed;
        if (!is_new_node_(id)) {
            if (old == nullptr || dimensions_changed) {
                return Result::ExternalPluginReprepareRequired;
            }
            const auto live = old->processors.find(id);
            if (live == old->processors.end() || live->second.get() != processor.get()) {
                return Result::ExternalPluginReprepareRequired;
            }
            continue;
        }
        const auto* node = candidate_->node(id);
        if (node == nullptr)
            return Result::PreflightFailed;
        format::PrepareContext context;
        context.sample_rate = sample_rate;
        context.max_buffer_size = max_block_size;
        context.input_channels = node->num_input_ports;
        context.output_channels = node->num_output_ports;
        if (!processor->instance->prepare(context))
            return Result::ExternalPluginReprepareRequired;
    }

    // Existing external instances may be shared with an off-side compile, but
    // they must never be prepared or replaced by this transaction.
    for (const auto& n : candidate_->nodes_) {
        if (n.type != NodeType::Plugin || !n.plugin)
            continue;
        if (is_new_node_(n.id) && detail::is_prepared_owned_built_in_slot(n.plugin.get())) {
            try {
                if (!n.plugin->prepare(sample_rate, max_block_size))
                    return Result::ExternalPluginReprepareRequired;
            } catch (...) {
                return Result::ExternalPluginReprepareRequired;
            }
            candidate_->prepared_plugin_meta_[n.id] = SignalGraph::PreparedPluginMetadata{
                n.plugin->parameters(), std::max(0, n.plugin->latency_samples()),
                n.plugin->wants_transport(), n.plugin->restart_request_count()};
            continue;
        }
        if (old == nullptr) {
            return Result::ExternalPluginReprepareRequired;
        }
        if (dimensions_changed) {
            return Result::ExternalPluginReprepareRequired;
        }
        const auto live_plugin = old->plugins.find(n.id);
        if (live_plugin == old->plugins.end() || live_plugin->second.get() != n.plugin.get() ||
            candidate_->prepared_plugin_meta_.count(n.id) == 0) {
            return Result::ExternalPluginReprepareRequired;
        }
    }

    if (!replaced_registry_keys_.empty()) {
        return Result::CustomRegistryConflict;
    }

    // Existing custom instances are immutable identities for this operation.
    // Only nodes minted inside this edit may run create/load/prepare callbacks.
    for (auto& n : candidate_->nodes_) {
        if (n.type != NodeType::Custom || is_new_node_(n.id))
            continue;
        if (n.custom_state_pending) {
            return Result::ExistingCustomReprepareRequired;
        }
        const auto* type = candidate_->custom_node_type(n.custom_type_id, n.custom_type_version);
        if (dimensions_changed && type != nullptr && (type->prepare || type->release)) {
            return Result::ExistingCustomReprepareRequired;
        }
        if (old == nullptr) {
            if (type != nullptr && type->create) {
                return Result::ExistingCustomReprepareRequired;
            }
            continue;
        }
        const auto existing = old->custom_instances.find(n.id);
        if (existing == old->custom_instances.end() ||
            existing->second != n.custom_instance.get()) {
            return Result::ExistingCustomReprepareRequired;
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
            return Result::CustomInstanceCreateFailed;
        }
        auto destroy = type->destroy;
        n.custom_instance = std::shared_ptr<void>(raw, [destroy](void* p) {
            if (destroy && p)
                destroy(p);
        });
        prepared_new_custom_ids_.push_back(n.id);
        if (n.custom_state_pending && type->load_state) {
            if (!type->load_state(n.custom_instance.get(), n.custom_state_blob)) {
                return Result::CustomStateRestoreFailed;
            }
            n.custom_state_pending = false;
        }
        if (type->prepare) {
            type->prepare(n.custom_instance.get(), sample_rate, max_block_size);
        }
    }
    return std::nullopt;
}

} // namespace pulp::host
