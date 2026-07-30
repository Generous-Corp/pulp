#pragma once

#include <pulp/tools/timeline/agent.hpp>

#include <pulp/timeline/transaction.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp_mcp {

std::string timeline_dirty_set_json(const pulp::timeline::DirtySet& dirty);

struct TimelineSessionStoreLimits {
    std::size_t max_sessions = 32;
    // Deterministic admission-charge ceiling, not a direct heap measurement.
    // Each session is charged twice its canonical JSON size plus its fixed
    // history reservation.
    std::size_t max_admission_charge_bytes = 64 * 1024 * 1024;
    std::size_t max_output_bytes = 64 * 1024 * 1024;
    // Zero derives a reservation equal to half the aggregate admission-charge
    // ceiling divided across max_sessions. It is split between journal and undo.
    std::size_t max_history_bytes_per_session = 0;
};

class TimelineSessionStore {
  public:
    explicit TimelineSessionStore(TimelineSessionStoreLimits limits = {});
    ~TimelineSessionStore();
    TimelineSessionStore(const TimelineSessionStore&) = delete;
    TimelineSessionStore& operator=(const TimelineSessionStore&) = delete;

    std::optional<std::string> open(std::string_view canonical_project, std::string& error);
    pulp::tools::timeline::OperationResult apply(std::string_view session_id,
                                                 std::string_view commands);
    pulp::tools::timeline::OperationResult diff(std::string_view session_id);
    pulp::tools::timeline::OperationResult undo(std::string_view session_id);
    pulp::tools::timeline::OperationResult redo(std::string_view session_id);

    std::size_t admission_charge_for_testing() const;
    void set_max_output_bytes_for_testing(std::size_t maximum);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::optional<std::string> open_timeline_session(std::string_view canonical_project,
                                                 std::string& error);
pulp::tools::timeline::OperationResult apply_timeline_session(std::string_view session_id,
                                                              std::string_view commands);
pulp::tools::timeline::OperationResult diff_timeline_session(std::string_view session_id);
pulp::tools::timeline::OperationResult undo_timeline_session(std::string_view session_id);
pulp::tools::timeline::OperationResult redo_timeline_session(std::string_view session_id);

} // namespace pulp_mcp
