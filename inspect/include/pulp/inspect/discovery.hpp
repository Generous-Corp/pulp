#pragma once

#include <pulp/inspect/capabilities.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

struct InspectorDiscoveryRecord {
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
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

/// Move-only inspector credential whose storage is wiped on every exit path.
class InspectorCredential {
public:
    explicit InspectorCredential(std::span<const std::uint8_t> bytes);
    ~InspectorCredential();

    InspectorCredential(const InspectorCredential&) = delete;
    InspectorCredential& operator=(const InspectorCredential&) = delete;
    InspectorCredential(InspectorCredential&& other) noexcept;
    InspectorCredential& operator=(InspectorCredential&& other) noexcept;

    std::span<const std::uint8_t> bytes() const {
        return bytes_;
    }

    friend bool operator==(const InspectorCredential& credential,
                           const std::vector<std::uint8_t>& bytes) {
        return credential.bytes_ == bytes;
    }

    friend bool operator==(const std::vector<std::uint8_t>& bytes,
                           const InspectorCredential& credential) {
        return credential == bytes;
    }

private:
    void clear() noexcept;
    std::vector<std::uint8_t> bytes_;
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
    std::optional<InspectorCredential> read_credential(
        const InspectorDiscoveryRecord& record) const;

private:
    std::filesystem::path runtime_directory_;
};

std::optional<InspectorDiscoveryRecord> select_inspector_session(
    std::span<const InspectorDiscoveryRecord> records,
    std::string_view session_id,
    std::string_view instance_id = {},
    std::string_view publication_id = {},
    std::string* error = nullptr);

} // namespace pulp::inspect
