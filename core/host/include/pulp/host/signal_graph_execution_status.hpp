#pragma once

// Execution readiness reported by a prepared SignalGraph snapshot.
//
// This contract is intentionally independent of SignalGraph's authoring and
// runtime declarations so hosts that only inspect routed-path readiness can
// include a small, stable surface. SignalGraph keeps its historical nested
// name as an alias to SignalGraphExecutionStatus.

namespace pulp::host {

struct SignalGraphExecutionStatus {
    bool prepared = false;
    bool serial_selected = false;
    bool serial_snapshot_valid = false;
    bool serial_pool_fits = false;
    bool parallel_selected = false;
    bool parallel_snapshot_valid = false;
    bool parallel_pool_fits = false;
    bool worker_pool_running = false;
    bool reference_walk_permitted = true;

    constexpr bool routed_path_ready() const noexcept {
        const bool serial_ready = serial_selected && serial_snapshot_valid && serial_pool_fits;
        const bool parallel_ready = parallel_selected && parallel_snapshot_valid &&
                                    parallel_pool_fits && worker_pool_running;
        return prepared && (serial_ready || parallel_ready);
    }

    constexpr bool strict_routed_ready() const noexcept {
        return routed_path_ready() && !reference_walk_permitted;
    }
};

} // namespace pulp::host
