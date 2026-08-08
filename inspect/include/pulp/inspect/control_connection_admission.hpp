#pragma once

#include <pulp/inspect/control_peer.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace pulp::inspect {

inline constexpr std::size_t kControlMaximumConnectionAdmissions = 1024;
inline constexpr auto kControlMaximumConnectionAdmissionTtl = std::chrono::seconds(30);

struct ControlClientConnectionPrincipal {
    ControlClientId client_id;
    friend bool operator==(const ControlClientConnectionPrincipal&,
                           const ControlClientConnectionPrincipal&) = default;
};

struct ControlHostConnectionPrincipal {
    ControlRegistrationId registration_id;
    friend bool operator==(const ControlHostConnectionPrincipal&,
                           const ControlHostConnectionPrincipal&) = default;
};

using ControlConnectionPrincipal =
    std::variant<ControlClientConnectionPrincipal, ControlHostConnectionPrincipal>;

struct ControlConnectionAdmission {
    std::string admission_id;
    ControlPeerExpectation expected_peer;
    ControlConnectionPrincipal principal;
    std::chrono::steady_clock::time_point expires_at;
};

enum class ControlConnectionAdmissionStatus : std::uint8_t {
    Issued,
    InvalidRequest,
    ResourceExhausted,
    EntropyUnavailable,
};

struct ControlConnectionAdmissionTicket {
    std::string admission_id;
    std::chrono::steady_clock::time_point expires_at;
};

struct ControlConnectionAdmissionResult {
    ControlConnectionAdmissionStatus status = ControlConnectionAdmissionStatus::InvalidRequest;
    std::optional<ControlConnectionAdmissionTicket> ticket;
};

struct ControlConnectionAdmissionStoreConfig {
    std::size_t maximum_admissions = 64;
    std::chrono::milliseconds admission_ttl = std::chrono::seconds(5);
};

/// Broker-owned, process-local admission registry for authenticated carriers.
///
/// Issuers supply only an expectation derived from trusted launcher or policy
/// state. consume() removes a matching entry atomically before returning it, so
/// rejected peer verification cannot make an admission reusable. The clock may
/// execute while the registry mutex is held and must be bounded, non-blocking,
/// and non-reentrant.
class ControlConnectionAdmissionStore {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit ControlConnectionAdmissionStore(
        ControlConnectionAdmissionStoreConfig config = {},
        Clock clock = [] { return std::chrono::steady_clock::now(); });
    ~ControlConnectionAdmissionStore();

    ControlConnectionAdmissionStore(const ControlConnectionAdmissionStore&) = delete;
    ControlConnectionAdmissionStore& operator=(const ControlConnectionAdmissionStore&) = delete;

    ControlConnectionAdmissionResult issue(ControlPeerExpectation expected_peer,
                                           ControlConnectionPrincipal principal);
    std::optional<ControlConnectionAdmission> consume(std::string_view admission_id);
    void sweep_expired();
    std::size_t size() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
