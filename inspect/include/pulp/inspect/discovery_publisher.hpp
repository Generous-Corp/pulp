#pragma once

#include <pulp/inspect/discovery.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulp::inspect {

/// Publishing authority held only by a live inspector runtime. Publication
/// holds an OS-released exclusive lease and uses owner-only atomic data files.
/// Destruction removes record and credential; the inert lock sentinel remains
/// available for race-free reuse.
class InspectorDiscoveryPublisher {
public:
    explicit InspectorDiscoveryPublisher(
        std::filesystem::path runtime_directory =
            default_inspector_runtime_directory());
    ~InspectorDiscoveryPublisher();

    InspectorDiscoveryPublisher(const InspectorDiscoveryPublisher&) = delete;
    InspectorDiscoveryPublisher& operator=(const InspectorDiscoveryPublisher&) =
        delete;

    bool publish(InspectorDiscoveryRecord record,
                 std::span<const std::uint8_t> credential,
                 std::chrono::milliseconds ttl = std::chrono::seconds(30));
    bool refresh(std::chrono::milliseconds ttl = std::chrono::seconds(30));
    void remove();

    const std::optional<InspectorDiscoveryRecord>& record() const {
        return record_;
    }

private:
    struct OwnershipLease;
    std::filesystem::path runtime_directory_;
    std::optional<InspectorDiscoveryRecord> record_;
    std::vector<std::uint8_t> credential_;
    std::filesystem::path ownership_path_;
    std::string ownership_marker_;
    std::unique_ptr<OwnershipLease> ownership_;
};

} // namespace pulp::inspect
