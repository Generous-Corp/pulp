#include <pulp/inspect/control_operations.hpp>

#include "control_operation_internal.hpp"
#include "control_private_store.hpp"
#include "control_protocol_internal.hpp"

#include <algorithm>
#include <mutex>
#include <ranges>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace pulp::inspect {
namespace {

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

} // namespace

class ControlOperationStore::Impl {
  public:
    Impl(ControlOperationStoreConfig config_in, WallClock clock_in)
        : config(std::move(config_in)), clock(std::move(clock_in)) {}

    std::filesystem::path path_for(const ControlReceiptId& id) const {
        return config.directory / (id.value + ".json");
    }

    void clear() {
        by_id.clear();
        by_idempotency_scope.clear();
        by_request_scope.clear();
    }

    bool persist(const ControlOperationReceipt& receipt) const {
        const auto encoded = detail::serialize_receipt(receipt);
        if (encoded.size() > config.max_receipt_bytes)
            return false;
        return detail::write_owner_private_file_atomic(path_for(receipt.receipt_id),
                                                       bytes_of(encoded));
    }

    bool prune_expired_terminal_receipts(std::int64_t now) {
        for (auto iterator = by_id.begin(); iterator != by_id.end();) {
            const auto& receipt = iterator->second;
            if (!control_receipt_state_is_terminal(receipt.state) ||
                !detail::elapsed_at_least(now, receipt.updated_at_unix_ms, config.retention)) {
                ++iterator;
                continue;
            }
            if (!detail::remove_owner_private_file_durable(path_for(receipt.receipt_id))) {
                return false;
            }
            by_idempotency_scope.erase(detail::idempotency_scope_hash(receipt.binding));
            by_request_scope.erase(detail::request_scope_hash(receipt.binding));
            iterator = by_id.erase(iterator);
        }
        return true;
    }

    ControlOperationStoreConfig config;
    WallClock clock;
    mutable std::mutex mutex;
    bool opened = false;
    std::unordered_map<std::string, ControlOperationReceipt> by_id;
    std::unordered_map<std::string, std::string> by_idempotency_scope;
    std::unordered_map<std::string, std::string> by_request_scope;
};

ControlOperationStore::ControlOperationStore(ControlOperationStoreConfig config, WallClock clock)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(clock))) {}

ControlOperationStore::~ControlOperationStore() = default;

ControlOperationStoreResult ControlOperationStore::open() {
    std::lock_guard lock(impl_->mutex);
    impl_->opened = false;
    impl_->clear();
    if (impl_->config.directory.empty() || impl_->config.max_receipts == 0 ||
        impl_->config.max_receipt_bytes == 0 || impl_->config.max_active_receipts_per_client == 0 ||
        impl_->config.max_active_receipts_per_registration_instance == 0 ||
        impl_->config.replay_window.count() <= 0 ||
        impl_->config.retention <= impl_->config.replay_window ||
        !detail::ensure_owner_private_directory(impl_->config.directory)) {
        return {ControlOperationStoreStatus::StoreUnavailable,
                {},
                "receipt directory is unavailable or not owner-private"};
    }

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(impl_->config.directory, error)) {
        if (error)
            break;
        const auto filename = entry.path().filename().string();
        if (filename.find(".tmp-") != std::string::npos)
            continue;
        if (!filename.ends_with(".json"))
            continue;
        const auto bytes =
            detail::read_owner_private_file(entry.path(), impl_->config.max_receipt_bytes);
        const auto parsed =
            bytes
                ? detail::parse_receipt(
                      std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()},
                      impl_->config.max_receipt_bytes)
                : std::nullopt;
        if (!parsed || filename != parsed->receipt_id.value + ".json" ||
            impl_->by_id.contains(parsed->receipt_id.value)) {
            impl_->clear();
            return {ControlOperationStoreStatus::MalformedStore,
                    {},
                    "committed receipt metadata is malformed"};
        }
        const auto idempotency_scope = detail::idempotency_scope_hash(parsed->binding);
        const auto request_scope = detail::request_scope_hash(parsed->binding);
        if (impl_->by_idempotency_scope.contains(idempotency_scope) ||
            impl_->by_request_scope.contains(request_scope) ||
            impl_->by_id.size() >= impl_->config.max_receipts) {
            impl_->clear();
            return {ControlOperationStoreStatus::MalformedStore,
                    {},
                    "receipt store contains duplicate scope or exceeds capacity"};
        }
        impl_->by_idempotency_scope.emplace(idempotency_scope, parsed->receipt_id.value);
        impl_->by_request_scope.emplace(request_scope, parsed->receipt_id.value);
        impl_->by_id.emplace(parsed->receipt_id.value, *parsed);
    }
    if (error) {
        impl_->clear();
        return {ControlOperationStoreStatus::StoreUnavailable,
                {},
                "receipt directory could not be enumerated"};
    }

    const auto now = detail::unix_milliseconds(impl_->clock());
    for (auto& [_, receipt] : impl_->by_id) {
        if (control_receipt_state_is_terminal(receipt.state))
            continue;

        auto recovered = receipt;
        recovered.result = {};
        if (receipt.state == ControlReceiptState::Admitted) {
            recovered.state = ControlReceiptState::Cancelled;
            recovered.result.result_code = ControlResultCode::Cancelled;
            recovered.result.explanation = "broker restarted before the operation began";
            recovered.result.cancellation_reason = receipt.cancellation_requested
                                                       ? receipt.cancellation_reason
                                                       : "broker-restart-before-execution";
        } else if (receipt.state == ControlReceiptState::Running) {
            recovered.state = ControlReceiptState::UnknownNeedsRefresh;
            recovered.result.result_code = ControlResultCode::UnknownNeedsRefresh;
            recovered.result.retry = ControlRetryClassification::AfterRefresh;
            recovered.result.explanation = "broker restarted while the operation was running";
        } else {
            impl_->clear();
            return {ControlOperationStoreStatus::MalformedStore,
                    {},
                    "receipt store contains an unknown nonterminal state"};
        }
        recovered.updated_at_unix_ms = std::max(recovered.created_at_unix_ms, now);
        if (!impl_->persist(recovered)) {
            impl_->clear();
            return {ControlOperationStoreStatus::PersistenceError,
                    {},
                    "nonterminal receipt recovery could not be persisted"};
        }
        receipt = std::move(recovered);
    }
    if (!impl_->prune_expired_terminal_receipts(now)) {
        impl_->clear();
        return {ControlOperationStoreStatus::PersistenceError,
                {},
                "expired terminal receipts could not be pruned durably"};
    }
    impl_->opened = true;
    return {ControlOperationStoreStatus::Opened, {}, {}};
}

bool ControlOperationStore::is_open() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->opened;
}

ControlOperationStoreResult ControlOperationStore::admit(ControlOperationBinding binding) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->opened)
        return {ControlOperationStoreStatus::StoreUnavailable, {}, "receipt store is not open"};
    const auto now = detail::unix_milliseconds(impl_->clock());
    if (!impl_->prune_expired_terminal_receipts(now)) {
        impl_->opened = false;
        impl_->clear();
        return {ControlOperationStoreStatus::PersistenceError,
                {},
                "expired terminal receipts could not be pruned durably"};
    }
    if (!detail::valid_binding(binding))
        return {ControlOperationStoreStatus::InvalidRequest, {}, "operation binding is invalid"};

    const auto idempotency_scope = detail::idempotency_scope_hash(binding);
    if (const auto found = impl_->by_idempotency_scope.find(idempotency_scope);
        found != impl_->by_idempotency_scope.end()) {
        const auto& existing = impl_->by_id.at(found->second);
        const auto request_scope = detail::request_scope_hash(binding);
        if (const auto request = impl_->by_request_scope.find(request_scope);
            request != impl_->by_request_scope.end() &&
            request->second != existing.receipt_id.value) {
            return {ControlOperationStoreStatus::RequestIdConflict,
                    impl_->by_id.at(request->second), "request id was already used by this client"};
        }
        if (detail::idempotency_content_hash(existing.binding) !=
            detail::idempotency_content_hash(binding)) {
            return {ControlOperationStoreStatus::IdempotencyConflict, existing,
                    "idempotency key was already used with different content"};
        }
        if (control_receipt_state_is_terminal(existing.state) &&
            detail::elapsed_at_least(now, existing.updated_at_unix_ms,
                                     impl_->config.replay_window)) {
            return {ControlOperationStoreStatus::ReplayWindowExpired, existing,
                    "terminal receipt is outside the replay window"};
        }
        return {ControlOperationStoreStatus::Replay, existing, {}};
    }

    const auto request_scope = detail::request_scope_hash(binding);
    if (const auto found = impl_->by_request_scope.find(request_scope);
        found != impl_->by_request_scope.end()) {
        return {ControlOperationStoreStatus::RequestIdConflict, impl_->by_id.at(found->second),
                "request id was already used by this client"};
    }
    if (impl_->by_id.size() >= impl_->config.max_receipts)
        return {
            ControlOperationStoreStatus::ResourceExhausted, {}, "receipt capacity is exhausted"};

    std::size_t active_for_client = 0;
    std::size_t active_for_registration_instance = 0;
    for (const auto& [_, existing] : impl_->by_id) {
        if (control_receipt_state_is_terminal(existing.state))
            continue;
        if (existing.binding.client_id == binding.client_id)
            ++active_for_client;
        if (existing.binding.registration_id == binding.registration_id &&
            existing.binding.instance_id == binding.instance_id) {
            ++active_for_registration_instance;
        }
    }
    if (active_for_client >= impl_->config.max_active_receipts_per_client) {
        return {ControlOperationStoreStatus::ResourceExhausted,
                {},
                "active receipt client quota is exhausted"};
    }
    if (active_for_registration_instance >=
        impl_->config.max_active_receipts_per_registration_instance) {
        return {ControlOperationStoreStatus::ResourceExhausted,
                {},
                "active receipt registration/instance quota is exhausted"};
    }

    const auto id = detail::random_receipt_id();
    if (!id)
        return {ControlOperationStoreStatus::ResourceExhausted,
                {},
                "receipt identity entropy is unavailable"};
    ControlOperationReceipt receipt{
        kControlOperationReceiptSchemaVersion,
        ControlReceiptId{*id},
        ControlReceiptState::Admitted,
        false,
        {},
        std::move(binding),
        {},
        ControlOperationResult{},
        now,
        now,
    };
    receipt.binding_hash = detail::binding_hash(receipt.binding);
    if (!impl_->persist(receipt)) {
        return {ControlOperationStoreStatus::PersistenceError,
                {},
                "admitted receipt could not be persisted"};
    }
    impl_->by_idempotency_scope.emplace(idempotency_scope, receipt.receipt_id.value);
    impl_->by_request_scope.emplace(request_scope, receipt.receipt_id.value);
    impl_->by_id.emplace(receipt.receipt_id.value, receipt);
    return {ControlOperationStoreStatus::Admitted, receipt, {}};
}

ControlOperationStoreResult ControlOperationStore::begin(const ControlReceiptId& receipt_id) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->opened)
        return {ControlOperationStoreStatus::StoreUnavailable, {}, "receipt store is not open"};
    if (!detail::valid_receipt_id(receipt_id.value))
        return {ControlOperationStoreStatus::InvalidRequest, {}, "receipt identity is invalid"};
    const auto found = impl_->by_id.find(receipt_id.value);
    if (found == impl_->by_id.end())
        return {ControlOperationStoreStatus::NotFound, {}, "receipt was not found"};
    if (found->second.state != ControlReceiptState::Admitted) {
        return {ControlOperationStoreStatus::InvalidTransition, found->second,
                "only an admitted receipt can begin execution"};
    }

    auto updated = found->second;
    const auto now = detail::unix_milliseconds(impl_->clock());
    if (updated.cancellation_requested) {
        updated.state = ControlReceiptState::Cancelled;
        updated.result.result_code = ControlResultCode::Cancelled;
        updated.result.explanation = "operation cancelled before execution";
        updated.result.cancellation_reason = updated.cancellation_reason;
    } else if (updated.binding.deadline_unix_ms <= now) {
        updated.state = ControlReceiptState::Failed;
        updated.result.result_code = ControlResultCode::DeadlineExceeded;
        updated.result.explanation = "operation deadline elapsed before execution";
    } else {
        updated.state = ControlReceiptState::Running;
    }
    updated.updated_at_unix_ms = std::max(updated.created_at_unix_ms, now);
    if (!impl_->persist(updated)) {
        return {ControlOperationStoreStatus::PersistenceError, found->second,
                "receipt start transition could not be persisted"};
    }
    found->second = updated;
    return {ControlOperationStoreStatus::Transitioned, updated, {}};
}

ControlOperationStoreResult ControlOperationStore::transition(const ControlReceiptId& receipt_id,
                                                              ControlReceiptState expected,
                                                              ControlReceiptState next,
                                                              ControlOperationResult result) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->opened)
        return {ControlOperationStoreStatus::StoreUnavailable, {}, "receipt store is not open"};
    if (!detail::valid_receipt_id(receipt_id.value) ||
        !detail::normalize_transition_result(next, result)) {
        return {ControlOperationStoreStatus::InvalidRequest, {}, "transition request is invalid"};
    }
    const auto found = impl_->by_id.find(receipt_id.value);
    if (found == impl_->by_id.end())
        return {ControlOperationStoreStatus::NotFound, {}, "receipt was not found"};
    if (found->second.state != expected || !detail::valid_store_transition(expected, next)) {
        return {ControlOperationStoreStatus::InvalidTransition, found->second,
                "receipt state transition is not legal"};
    }
    auto updated = found->second;
    updated.state = next;
    updated.result = std::move(result);
    updated.updated_at_unix_ms =
        std::max(updated.created_at_unix_ms, detail::unix_milliseconds(impl_->clock()));
    if (!impl_->persist(updated)) {
        return {ControlOperationStoreStatus::PersistenceError, found->second,
                "receipt transition could not be persisted"};
    }
    found->second = updated;
    return {ControlOperationStoreStatus::Transitioned, updated, {}};
}

ControlOperationStoreResult
ControlOperationStore::request_cancellation(const ControlReceiptId& receipt_id,
                                            std::string reason) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->opened)
        return {ControlOperationStoreStatus::StoreUnavailable, {}, "receipt store is not open"};
    if (!detail::valid_receipt_id(receipt_id.value) || reason.empty() ||
        !control_protocol_detail::valid_text(reason,
                                             kControlReceiptMaximumCancellationReasonBytes)) {
        return {ControlOperationStoreStatus::InvalidRequest, {}, "cancellation request is invalid"};
    }
    const auto found = impl_->by_id.find(receipt_id.value);
    if (found == impl_->by_id.end())
        return {ControlOperationStoreStatus::NotFound, {}, "receipt was not found"};
    if (control_receipt_state_is_terminal(found->second.state)) {
        return {ControlOperationStoreStatus::InvalidTransition, found->second,
                "terminal receipts cannot be cancelled"};
    }
    if (found->second.cancellation_requested) {
        return {ControlOperationStoreStatus::CancellationRequested, found->second, {}};
    }

    auto updated = found->second;
    updated.cancellation_requested = true;
    updated.cancellation_reason = std::move(reason);
    updated.updated_at_unix_ms =
        std::max(updated.created_at_unix_ms, detail::unix_milliseconds(impl_->clock()));
    if (!impl_->persist(updated)) {
        return {ControlOperationStoreStatus::PersistenceError, found->second,
                "cancellation request could not be persisted"};
    }
    found->second = updated;
    return {ControlOperationStoreStatus::CancellationRequested, updated, {}};
}

std::optional<ControlOperationReceipt>
ControlOperationStore::receipt(const ControlReceiptId& receipt_id) const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->opened)
        return std::nullopt;
    const auto found = impl_->by_id.find(receipt_id.value);
    return found == impl_->by_id.end() ? std::nullopt
                                       : std::optional<ControlOperationReceipt>(found->second);
}

std::vector<ControlOperationReceipt> ControlOperationStore::receipts() const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->opened)
        return {};
    std::vector<ControlOperationReceipt> result;
    result.reserve(impl_->by_id.size());
    for (const auto& [_, receipt] : impl_->by_id)
        result.push_back(receipt);
    std::ranges::sort(result, {}, [](const auto& receipt) { return receipt.receipt_id.value; });
    return result;
}

} // namespace pulp::inspect
