#pragma once

#include <pulp/inspect/protocol.hpp>

#include <optional>
#include <string>

namespace pulp::inspect {

/// Exact selector for one canonical control registration generation.
struct InspectorClientTarget {
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    friend bool operator==(const InspectorClientTarget&, const InspectorClientTarget&) = default;
};

/// Typed outcome for a one-shot canonical control request.
struct InspectorClientResult {
    std::optional<InspectorClientTarget> target;
    InspectorMessage response;

    bool succeeded() const {
        return target.has_value() && !response.is_error;
    }
};

} // namespace pulp::inspect
