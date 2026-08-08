#pragma once

#include <pulp/inspect/control_peer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

/// Transport primitives for a record already minted by broker-owned enrollment.
/// They do not authenticate, register, admit, or launch a host.
/// The record format is not authenticated and its fields are never a trust root.
/// The production broker daemon does not issue these records and remains denied.

inline constexpr std::uint32_t kControlHostBootstrapVersion = 1;
inline constexpr std::string_view kControlHostBootstrapSchema = "dev.pulp.control/host-bootstrap@1";
inline constexpr std::size_t kControlHostBootstrapMaximumBytes = 16u * 1024u;

enum class ControlHostBootstrapStatus : std::uint8_t {
    Accepted,
    Absent,
    Truncated,
    Oversize,
    InvalidRecord,
    UnsupportedVersion,
    Expired,
    ReadFailed,
};

struct ControlHostBootstrapDiagnostics {
    ControlHostBootstrapStatus status = ControlHostBootstrapStatus::InvalidRecord;
    std::string explanation;
};

/// One-shot inherited authority material. This is a pre-carrier record,
/// not a broker RPC frame: the authenticated host connection continues to use
/// the canonical ControlEnvelope/InterprocessConnection protocol exclusively.
class ControlHostBootstrapRecord {
  public:
    std::string schema = std::string(kControlHostBootstrapSchema);
    std::uint32_t version = kControlHostBootstrapVersion;
    std::filesystem::path endpoint_path;
    ControlPeerExpectation expected_broker;
    /// Exactly one credential form is valid: admission plus registration, or
    /// enrollment alone. An absent enrollment field decodes as the legacy
    /// admission form; new encodings always include it.
    std::string admission_id;
    ControlRegistrationId registration_id;
    std::string enrollment_id;
    std::int64_t expires_at_unix_ms = 0;

    ControlHostBootstrapRecord() = default;
    ~ControlHostBootstrapRecord();
    ControlHostBootstrapRecord(const ControlHostBootstrapRecord&) = delete;
    ControlHostBootstrapRecord& operator=(const ControlHostBootstrapRecord&) = delete;
    ControlHostBootstrapRecord(ControlHostBootstrapRecord&&) noexcept;
    ControlHostBootstrapRecord& operator=(ControlHostBootstrapRecord&&) noexcept;

    void clear() noexcept;
};

class ControlHostBootstrapBytes {
  public:
    ControlHostBootstrapBytes() = default;
    explicit ControlHostBootstrapBytes(std::vector<std::uint8_t> bytes);
    ~ControlHostBootstrapBytes();
    ControlHostBootstrapBytes(const ControlHostBootstrapBytes&) = delete;
    ControlHostBootstrapBytes& operator=(const ControlHostBootstrapBytes&) = delete;
    ControlHostBootstrapBytes(ControlHostBootstrapBytes&&) noexcept;
    ControlHostBootstrapBytes& operator=(ControlHostBootstrapBytes&&) noexcept;

    bool empty() const noexcept { return bytes_.empty(); }
    std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
    void clear() noexcept;

  private:
    std::vector<std::uint8_t> bytes_;
};

ControlHostBootstrapBytes encode_control_host_bootstrap(const ControlHostBootstrapRecord& record);

std::optional<ControlHostBootstrapRecord> decode_control_host_bootstrap(
    std::span<const std::uint8_t> bytes,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
    ControlHostBootstrapDiagnostics* diagnostics = nullptr);

#ifdef _WIN32
using ControlHostBootstrapHandle = void*;
#else
using ControlHostBootstrapHandle = int;
#endif

/// Reads one bounded document to EOF, closes the inherited handle, and wipes
/// the temporary encoded bytes on every path.
std::optional<ControlHostBootstrapRecord> read_control_host_bootstrap(
    ControlHostBootstrapHandle handle, std::chrono::milliseconds timeout = std::chrono::seconds(3),
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
    ControlHostBootstrapDiagnostics* diagnostics = nullptr);

/// Claims the private inherited standard-input handle.
/// Later calls in the same process return an invalid handle.
ControlHostBootstrapHandle inherited_control_host_bootstrap_handle() noexcept;

} // namespace pulp::inspect
