#pragma once

#include <pulp/host/signal_graph_execution_snapshot.hpp>

#include <optional>

namespace pulp::host {

// Isolated, fail-closed authoring transaction for topology-producing clients.
// This exposes only nodes that SignalGraph owns and can prepare off-side.
class SignalGraph::PreparedTopologyEdit {
  public:
    enum class Result : std::uint8_t {
        Prepared,
        Committed,
        InvalidMutation,
        StaleBase,
        PreflightFailed,
        MidiOutputSnapshotLocalRequired,
        ExternalPluginReprepareRequired,
        BaselinePluginRemovalRequiresRelease,
        ExistingCustomReprepareRequired,
        BaselineCustomRemovalRequiresRelease,
        CustomRegistryConflict,
        CustomInstanceCreateFailed,
        CustomStateRestoreFailed,
        CompileFailed,
        RuntimeAdoptionFailed,
        ParallelWorkerStartFailed,
        QuiescedRollbackFailed,
        NotPrepared,
        AlreadyCommitted,
    };

    ~PreparedTopologyEdit();
    PreparedTopologyEdit(const PreparedTopologyEdit&) = delete;
    PreparedTopologyEdit& operator=(const PreparedTopologyEdit&) = delete;

    bool register_custom_node_type(CustomNodeType type);
    bool unregister_custom_node_type(std::string_view type_id, int version);
    std::size_t prune_unused_custom_node_types();
    std::size_t custom_node_type_count() const;

    NodeId add_input_node(int channels, const std::string& name = "Input");
    NodeId add_output_node(int channels, const std::string& name = "Output");
    NodeId add_gain_node(const std::string& name = "Gain");
    NodeId add_midi_input_node(const std::string& name = "MIDI In");
    NodeId add_midi_output_node(const std::string& name = "MIDI Out");
    NodeId add_custom_node(std::string_view type_id, const std::string& name = {});
    NodeId add_custom_node(std::string_view type_id, int version,
                           const std::string& name = {});
    NodeId add_unresolved_custom_node(std::string_view type_id, int version,
                                      int num_inputs, int num_outputs,
                                      const std::string& name);
    bool remove_node(NodeId id);
    bool connect(NodeId source, PortIndex source_port, NodeId dest, PortIndex dest_port);
    bool connect_feedback(NodeId source, PortIndex source_port, NodeId dest,
                          PortIndex dest_port);
    bool connect_midi(NodeId source, NodeId dest);
    bool disconnect(NodeId source, PortIndex source_port, NodeId dest, PortIndex dest_port);
    bool set_node_gain(NodeId id, float linear_gain);

    void set_canonical_executor_routing_enabled(bool enabled) noexcept;
    void set_parallel_routing_enabled(bool enabled) noexcept;
    void set_anticipation_enabled(bool enabled) noexcept;

    const GraphNode* node(NodeId id) const;
    const std::vector<GraphNode>& nodes() const;
    const std::vector<Connection>& connections() const;
    bool routed_execution_ready(int block_size) const noexcept;

    Result prepare(double sample_rate, int max_block_size);
    /// Requires process, MIDI injection, and anticipation to be stopped.
    Result prepare_quiesced(double sample_rate, int max_block_size);
    Result commit();
    ExecutionSnapshot committed_execution_snapshot() const noexcept {
        return ExecutionSnapshot(*owner_, committed_snapshot_);
    }
    Result last_result() const noexcept { return last_result_; }

  private:
    friend class SignalGraph;
    friend class TimelineGraphPlaybackBinding;
    bool set_exact_parameter_event_nodes(
        const std::shared_ptr<detail::ExactParameterIngressOwner>& owner,
        std::span<const NodeId> nodes);
    explicit PreparedTopologyEdit(SignalGraph& owner);
    bool base_is_current_locked_() const;
    bool is_new_node_(NodeId id) const;
    std::optional<Result> baseline_removal_rejection_locked_() const;
    bool rollback_quiesced_lifecycles_locked_() noexcept;
    void release_new_custom_instances_() noexcept;

    struct QuiescedPluginLifecycle {
        std::shared_ptr<PluginSlot> plugin;
        bool touched = false;
    };
    struct QuiescedCustomLifecycle {
        std::shared_ptr<void> instance;
        std::function<void(void*, double, int)> prepare;
        std::function<void(void*)> release;
        bool touched = false;
    };

    template <typename Fn> NodeId add_node_(Fn&& fn) {
        if (mutation_failed_ || committed_ || prepare_attempted_) {
            mutation_failed_ = true;
            return 0;
        }
        const NodeId id = fn();
        if (id == 0)
            mutation_failed_ = true;
        return id;
    }
    template <typename Fn> bool mutate_(Fn&& fn) {
        if (mutation_failed_ || committed_ || prepare_attempted_) {
            mutation_failed_ = true;
            return false;
        }
        const bool ok = fn();
        if (!ok)
            mutation_failed_ = true;
        return ok;
    }

    SignalGraph* owner_ = nullptr;
    std::unique_ptr<SignalGraph> candidate_;
    std::shared_ptr<SignalGraph::CompiledGraph> prepared_snapshot_;
    std::shared_ptr<SignalGraph::CompiledGraph> committed_snapshot_;
    std::unordered_set<NodeId> baseline_node_ids_;
    std::unordered_set<std::string> baseline_registry_keys_;
    std::unordered_set<std::string> replaced_registry_keys_;
    std::vector<NodeId> prepared_new_custom_ids_;
    std::vector<QuiescedPluginLifecycle> quiesced_plugins_;
    std::vector<QuiescedCustomLifecycle> quiesced_customs_;
    std::uint64_t base_authoring_generation_ = 0;
    std::shared_ptr<SignalGraph::CompiledGraph> base_live_;
    bool base_canonical_routing_ = false;
    bool base_parallel_routing_ = false;
    bool base_anticipation_ = false;
    bool mutation_failed_ = false;
    bool prepare_attempted_ = false;
    bool quiesced_lifecycles_dirty_ = false;
    bool committed_ = false;
    Result last_result_ = Result::NotPrepared;
};

} // namespace pulp::host
