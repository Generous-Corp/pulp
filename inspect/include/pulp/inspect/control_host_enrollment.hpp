#pragma once

#include <pulp/inspect/control_peer.hpp>
#include <pulp/inspect/control_trusted_host_inventory.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect {

inline constexpr std::size_t kControlMaximumHostEnrollments = 1024;
inline constexpr auto kControlMaximumHostEnrollmentTtl = std::chrono::seconds(30);

enum class ControlHostEnrollmentStatus : std::uint8_t {
    Created,
    InvalidRequest,
    ResourceExhausted,
    EntropyUnavailable,
};

struct ControlHostEnrollmentTicket {
    std::string enrollment_id;
    std::chrono::steady_clock::time_point expires_at;
};

struct ControlHostEnrollmentResult {
    ControlHostEnrollmentStatus status = ControlHostEnrollmentStatus::InvalidRequest;
    std::optional<ControlHostEnrollmentTicket> ticket;
};

/// Move-only broker plan binding one consumed immutable launch snapshot to the
/// exact preobserved process the broker launched. Only
/// ControlHostEnrollmentStore can mint this type; request payloads supply only
/// its opaque, one-use identifier.
class ControlHostEnrollmentPlan {
  public:
    ControlHostEnrollmentPlan(ControlHostEnrollmentPlan&&) noexcept;
    ControlHostEnrollmentPlan& operator=(ControlHostEnrollmentPlan&&) noexcept;
    ~ControlHostEnrollmentPlan();
    ControlHostEnrollmentPlan(const ControlHostEnrollmentPlan&) = delete;
    ControlHostEnrollmentPlan& operator=(const ControlHostEnrollmentPlan&) = delete;

    std::int64_t expected_process_id() const;
    const ControlPeerExpectation& expected_peer() const;
    std::uint64_t broker_generation() const;
    std::chrono::steady_clock::time_point expires_at() const;
    const ControlTrustedHostSnapshot& snapshot() const;

  private:
    struct Impl;
    explicit ControlHostEnrollmentPlan(std::unique_ptr<Impl> impl);
    friend class ControlHostEnrollmentStore;
    std::unique_ptr<Impl> impl_;
};

struct ControlHostEnrollmentStoreConfig {
    std::size_t maximum_enrollments = 64;
};

/// Bounded, process-local enrollment registry. create() accepts only a
/// previously consumed trusted inventory snapshot, and consume() burns the
/// claim atomically before any live peer validation occurs. The endpoint never
/// holds this store's mutex while observing a peer or entering broker,
/// admission-store, or router coordination.
class ControlHostEnrollmentStore {
  public:
    /// Invoked under the store mutex; it must be bounded, non-blocking, and
    /// non-reentrant.
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit ControlHostEnrollmentStore(
        ControlHostEnrollmentStoreConfig config = {},
        Clock clock = [] { return std::chrono::steady_clock::now(); });
    ~ControlHostEnrollmentStore();
    ControlHostEnrollmentStore(const ControlHostEnrollmentStore&) = delete;
    ControlHostEnrollmentStore& operator=(const ControlHostEnrollmentStore&) = delete;

    /// preobserved_child must come from kernel evidence on an endpoint-owned
    /// private rendezvous. A PID-only post-spawn callback cannot satisfy this
    /// contract because macOS audit pidversion is available only from the
    /// connected peer's audit token.
    ControlHostEnrollmentResult create(ControlTrustedHostSnapshot snapshot,
                                       const VerifiedControlPeerIdentity& preobserved_child,
                                       std::uint64_t current_broker_generation,
                                       std::chrono::steady_clock::time_point expires_at);
    std::optional<ControlHostEnrollmentPlan> consume(std::string_view enrollment_id);
    std::size_t sweep();
    std::size_t size() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
