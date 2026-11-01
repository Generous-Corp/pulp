// SignalGraph live-swap policy and scanned-plugin catalog ownership.
//
// These control-thread entry points share the transaction's private state but
// do not participate in staging or publication. Keeping them separate makes
// the catalog/policy boundary reviewable without changing the public API.

#include <pulp/host/signal_graph.hpp>

#include "signal_graph_live_swap_detail.hpp"

#include <utility>

namespace pulp::host {

using namespace live_swap_detail;

bool SignalGraph::set_node_live_swap_policy(NodeId id, NodeLiveSwapPolicy policy) {
    GraphMutationLock mutation_lock(*this);
    auto* n = node_mut_locked_(id);
    if (!n || n->type != NodeType::Plugin)
        return false;
    n->live_swap_policy = clamp_policy(std::move(policy));
    ++authoring_generation_;
    return true;
}

bool SignalGraph::set_node_hosted_editor_open(NodeId id, bool open) {
    GraphMutationLock mutation_lock(*this);
    auto* n = node_mut_locked_(id);
    if (!n || n->type != NodeType::Plugin)
        return false;
    n->hosted_editor_open = open;
    ++authoring_generation_;
    return true;
}

SignalGraph::PluginCatalogToken SignalGraph::register_scanned_plugin(const PluginInfo& info) {
    GraphMutationLock mutation_lock(*this);
    const auto key = plugin_identity_key(info);
    if (auto it = scanned_plugin_identity_to_token_.find(key);
        it != scanned_plugin_identity_to_token_.end()) {
        return PluginCatalogToken{it->second};
    }
    const std::uint64_t token = next_scanned_plugin_token_++;
    scanned_plugin_catalog_[token] = info;
    scanned_plugin_identity_to_token_[key] = token;
    return PluginCatalogToken{token};
}

void SignalGraph::clear_scanned_plugin_catalog() {
    GraphMutationLock mutation_lock(*this);
    scanned_plugin_catalog_.clear();
    scanned_plugin_identity_to_token_.clear();
}

LiveSwapDiagnostics SignalGraph::last_swap_diagnostics() const {
    GraphMutationLock mutation_lock(*this);
    return last_swap_diagnostics_;
}

void SignalGraph::set_live_swap_plugin_loader_for_test(PluginLoaderForTest loader) {
    GraphMutationLock mutation_lock(*this);
    live_swap_plugin_loader_for_test_ = std::move(loader);
}

} // namespace pulp::host
