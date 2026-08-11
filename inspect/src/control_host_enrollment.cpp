#include <pulp/inspect/control_host_enrollment.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace pulp::inspect {
namespace {

std::optional<std::string> random_enrollment_id() {
    const auto bytes = runtime::secure_random_bytes(16);
    if (!bytes)
        return std::nullopt;
    return "enrollment-" + runtime::hex_encode(*bytes);
}

bool valid_enrollment_id(std::string_view id) {
    constexpr std::string_view prefix = "enrollment-";
    constexpr std::size_t encoded_bytes = 32;
    return id.size() == prefix.size() + encoded_bytes && id.starts_with(prefix) &&
           std::all_of(id.begin() + static_cast<std::ptrdiff_t>(prefix.size()), id.end(),
                       [](char value) {
                           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                       });
}

bool supported_snapshot(const ControlTrustedHostSnapshot& snapshot) {
    const auto tier = snapshot.registration().host_tier;
    const auto& registration = snapshot.registration();
    const auto& expectation = snapshot.static_expectation();
    return (tier == ControlHostTier::OfflineJob || tier == ControlHostTier::Standalone) &&
           !snapshot.executable().empty() && !registration.session_id.empty() &&
           !registration.instance_id.empty() && !registration.publication_id.empty() &&
           !registration.artifact_digest.empty() && !expectation.executable_identity.empty() &&
           !expectation.publisher_id.empty();
}

bool matches_snapshot(const ControlTrustedHostSnapshot& snapshot,
                      const VerifiedControlPeerIdentity& peer) {
    const auto& evidence = peer.evidence();
    const auto expected_role = snapshot.registration().host_tier == ControlHostTier::OfflineJob
                                   ? ControlPeerRole::OfflineHost
                                   : ControlPeerRole::StandaloneHost;
    return evidence.role == expected_role && !evidence.user_id.empty() && evidence.process_id > 0 &&
           !evidence.process_start_id.empty() &&
           evidence.executable_identity == snapshot.static_expectation().executable_identity &&
           evidence.publisher_id == snapshot.static_expectation().publisher_id;
}

} // namespace

struct ControlHostEnrollmentPlan::Impl {
    ControlTrustedHostSnapshot snapshot;
    ControlPeerExpectation expected_peer;
    std::uint64_t broker_generation = 0;
    std::chrono::steady_clock::time_point expires_at;
};

ControlHostEnrollmentPlan::ControlHostEnrollmentPlan(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ControlHostEnrollmentPlan::ControlHostEnrollmentPlan(ControlHostEnrollmentPlan&&) noexcept =
    default;
ControlHostEnrollmentPlan&
ControlHostEnrollmentPlan::operator=(ControlHostEnrollmentPlan&&) noexcept = default;
ControlHostEnrollmentPlan::~ControlHostEnrollmentPlan() = default;

std::int64_t ControlHostEnrollmentPlan::expected_process_id() const {
    return impl_->expected_peer.evidence.process_id;
}
const ControlPeerExpectation& ControlHostEnrollmentPlan::expected_peer() const {
    return impl_->expected_peer;
}
std::uint64_t ControlHostEnrollmentPlan::broker_generation() const {
    return impl_->broker_generation;
}
std::chrono::steady_clock::time_point ControlHostEnrollmentPlan::expires_at() const {
    return impl_->expires_at;
}
const ControlTrustedHostSnapshot& ControlHostEnrollmentPlan::snapshot() const {
    return impl_->snapshot;
}

struct ControlHostEnrollmentStore::Impl {
    Impl(ControlHostEnrollmentStoreConfig config_in, Clock clock_in)
        : config(std::move(config_in)), clock(std::move(clock_in)) {}

    std::size_t sweep_locked(std::chrono::steady_clock::time_point now) {
        std::size_t removed = 0;
        for (auto it = enrollments.begin(); it != enrollments.end();) {
            if (it->second.expires_at() <= now) {
                it = enrollments.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    ControlHostEnrollmentStoreConfig config;
    Clock clock;
    mutable std::mutex mutex;
    std::unordered_map<std::string, ControlHostEnrollmentPlan> enrollments;
};

ControlHostEnrollmentStore::ControlHostEnrollmentStore(ControlHostEnrollmentStoreConfig config,
                                                       Clock clock)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(clock))) {}
ControlHostEnrollmentStore::~ControlHostEnrollmentStore() = default;

ControlHostEnrollmentResult ControlHostEnrollmentStore::create(
    ControlTrustedHostSnapshot snapshot, const VerifiedControlPeerIdentity& preobserved_child,
    std::uint64_t current_broker_generation, std::chrono::steady_clock::time_point expires_at) {
    if (!impl_->clock || impl_->config.maximum_enrollments == 0 ||
        impl_->config.maximum_enrollments > kControlMaximumHostEnrollments ||
        current_broker_generation == 0 ||
        snapshot.broker_generation() != current_broker_generation ||
        !supported_snapshot(snapshot) || !matches_snapshot(snapshot, preobserved_child))
        return {};

    std::lock_guard lock(impl_->mutex);
    const auto now = impl_->clock();
    impl_->sweep_locked(now);
    // Inventory expiry bounds when launch can consume the snapshot. Once the
    // exact child has passed preflight, its one-use enrollment gets an
    // independent bounded window to reach the broker endpoint.
    if (expires_at <= now || expires_at - now > kControlMaximumHostEnrollmentTtl)
        return {};
    if (impl_->enrollments.size() >= impl_->config.maximum_enrollments)
        return {.status = ControlHostEnrollmentStatus::ResourceExhausted};

    const auto enrollment_id = random_enrollment_id();
    if (!enrollment_id)
        return {.status = ControlHostEnrollmentStatus::EntropyUnavailable};
    auto plan = ControlHostEnrollmentPlan(std::make_unique<ControlHostEnrollmentPlan::Impl>(
        ControlHostEnrollmentPlan::Impl{.snapshot = std::move(snapshot),
                                        .expected_peer = {.evidence = preobserved_child.evidence()},
                                        .broker_generation = current_broker_generation,
                                        .expires_at = expires_at}));
    const auto [it, inserted] = impl_->enrollments.emplace(*enrollment_id, std::move(plan));
    if (!inserted)
        return {.status = ControlHostEnrollmentStatus::EntropyUnavailable};
    return {.status = ControlHostEnrollmentStatus::Created,
            .ticket = ControlHostEnrollmentTicket{it->first, expires_at}};
}

std::optional<ControlHostEnrollmentPlan>
ControlHostEnrollmentStore::consume(std::string_view enrollment_id) {
    if (!valid_enrollment_id(enrollment_id) || !impl_->clock)
        return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto now = impl_->clock();
    impl_->sweep_locked(now);
    const auto it = impl_->enrollments.find(std::string(enrollment_id));
    if (it == impl_->enrollments.end())
        return std::nullopt;
    auto plan = std::move(it->second);
    impl_->enrollments.erase(it);
    return plan;
}

std::size_t ControlHostEnrollmentStore::sweep() {
    if (!impl_->clock)
        return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->sweep_locked(impl_->clock());
}

std::size_t ControlHostEnrollmentStore::size() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->enrollments.size();
}

} // namespace pulp::inspect
