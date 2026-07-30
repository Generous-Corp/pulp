#pragma once

#include <pulp/inspect/capabilities.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

struct InspectorDiscoveryRecord {
    std::string session_id;
    std::string instance_id;
    std::string plugin_id;
    std::string endpoint;
    std::string protocol_version = "1";
    InspectorProfile profile = InspectorProfile::Off;
    std::int64_t process_id = 0;
    std::string process_start_id;
    std::int64_t expires_at_unix_ms = 0;
    std::filesystem::path record_path;
    std::filesystem::path credential_path;
};

/// Returns the owner-private directory used by standalone inspector sessions.
/// PULP_INSPECTOR_RUNTIME_DIR is honored for deterministic installed-tool tests.
std::filesystem::path default_inspector_runtime_directory();

/// Read-only discovery authority for CLI, MCP, and other clients. It rejects
/// records or credentials outside the configured owner-private directory,
/// insecure file modes, expired records, and dead publishers.
class InspectorDiscoveryReader {
public:
    explicit InspectorDiscoveryReader(
        std::filesystem::path runtime_directory =
            default_inspector_runtime_directory());

    const std::filesystem::path& runtime_directory() const {
        return runtime_directory_;
    }

    std::vector<InspectorDiscoveryRecord> list() const;
    std::optional<std::vector<std::uint8_t>> read_credential(
        const InspectorDiscoveryRecord& record) const;

private:
    std::filesystem::path runtime_directory_;
};

/// Publishing authority held by a live inspector owner. Publication uses
/// owner-only atomic files. Destruction removes both record and credential.
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
    std::filesystem::path runtime_directory_;
    std::optional<InspectorDiscoveryRecord> record_;
    std::vector<std::uint8_t> credential_;
};

std::optional<InspectorDiscoveryRecord> select_inspector_session(
    std::span<const InspectorDiscoveryRecord> records,
    std::string_view session_id,
    std::string* error = nullptr);

} // namespace pulp::inspect
