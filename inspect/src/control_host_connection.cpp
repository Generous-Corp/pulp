#include <pulp/inspect/control_host_connection.hpp>

#include <pulp/events/interprocess_connection.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace pulp::inspect {
namespace {

using events::InterprocessConnection;
using events::IpcTransport;

ControlExecutionOutcome normalize_host_completion(ControlExecutionOutcome outcome) {
    const bool unsupported_outputs =
        !outcome.result.artifacts.empty() || !outcome.result.evidence_ids.empty();
    const bool completed = outcome.terminal_state == ControlReceiptState::Completed &&
                           !outcome.result.result_code &&
                           outcome.result.cancellation_reason.empty();
    const bool failed = outcome.terminal_state == ControlReceiptState::Failed &&
                        outcome.result.result_code &&
                        outcome.result.result_code != ControlResultCode::Cancelled &&
                        outcome.result.result_code != ControlResultCode::CompletedAfterRevocation &&
                        outcome.result.cancellation_reason.empty();
    const bool cancelled = outcome.terminal_state == ControlReceiptState::Cancelled &&
                           outcome.result.result_code == ControlResultCode::Cancelled &&
                           !outcome.result.cancellation_reason.empty();
    if (!unsupported_outputs && (completed || failed || cancelled))
        return outcome;
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result = {.result_code = ControlResultCode::InternalError,
                   .explanation =
                       unsupported_outputs
                           ? "host result artifacts are not supported by this protocol revision"
                           : "host executor returned a non-authorable terminal result"},
    };
}

ControlHostCompleteEnvelope completion(std::string route_id, ControlExecutionOutcome outcome) {
    outcome = normalize_host_completion(std::move(outcome));
    return {
        .route_id = std::move(route_id),
        .terminal_state = outcome.terminal_state,
        .result_code = outcome.result.result_code,
        .retry = outcome.result.retry,
        .explanation = std::move(outcome.result.explanation),
        .detail_json = std::move(outcome.result.detail_json),
        .cancellation_reason = std::move(outcome.result.cancellation_reason),
    };
}

} // namespace

struct ControlHostConnection::Impl : std::enable_shared_from_this<Impl> {
    struct Job {
        ControlHostExecuteEnvelope execute;
        std::atomic<bool> cancelled{false};
    };

    ControlHostConnectionConfig config;
    ControlOperationExecutor executor;
    InterprocessConnection connection;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable opened_ready;
    std::condition_variable poison_ready;
    std::deque<std::shared_ptr<Job>> queue;
    std::unordered_map<std::string, std::shared_ptr<Job>> active;
    std::optional<ControlHostOpenResult> opened;
    std::string expected_open_request;
    ControlRegistrationId registration_id;
    std::string last_error;
    std::string last_explanation;
    std::thread worker;
    std::thread poison_worker;
    bool connected = false;
    bool host_open = false;
    bool stopping = false;
    bool poison_requested = false;
    bool poison_in_progress = false;
    bool poison_shutdown = false;
    std::atomic<std::uint64_t> next_request{1};

    Impl(ControlHostConnectionConfig config_in, ControlOperationExecutor executor_in)
        : config(std::move(config_in)), executor(std::move(executor_in)) {
        connection.set_max_message_bytes(kControlMaximumEnvelopeBytes);
        connection.set_write_timeout(config.write_timeout);
        connection.set_frame_read_timeout(config.frame_read_timeout);
        connection.set_on_message(
            [this](const void* data, std::size_t size) { receive(data, size); });
        connection.set_on_disconnected([this] { disconnected(); });
        poison_worker = std::thread([this] { run_poison(); });
    }

    bool send(const ControlEnvelope& envelope) {
        const auto encoded = encode_control_envelope(envelope);
        return !encoded.empty() && connection.send_message(encoded);
    }

    bool send_completion(std::string_view route, ControlExecutionOutcome outcome) {
        if (send(ControlEnvelope{.payload = completion(std::string(route), std::move(outcome))}))
            return true;
        fail("host-completion-send-failed",
             "the final host completion could not be encoded or delivered");
        return false;
    }

    void fail(std::string code, std::string explanation) {
        {
            std::lock_guard lock(mutex);
            last_error = std::move(code);
            last_explanation = std::move(explanation);
            connected = false;
            host_open = false;
            stopping = true;
            for (auto& [route, job] : active) {
                (void)route;
                job->cancelled.store(true, std::memory_order_release);
            }
            queue.clear();
            if (!poison_shutdown)
                poison_requested = true;
        }
        ready.notify_all();
        opened_ready.notify_all();
        poison_ready.notify_one();
    }

    void run_poison() {
        for (;;) {
            {
                std::unique_lock lock(mutex);
                poison_ready.wait(lock, [&] { return poison_requested || poison_shutdown; });
                if (poison_shutdown)
                    return;
                poison_requested = false;
                poison_in_progress = true;
            }
            connection.disconnect();
            {
                std::lock_guard lock(mutex);
                poison_in_progress = false;
            }
            poison_ready.notify_all();
        }
    }

    void wait_for_poison() {
        std::unique_lock lock(mutex);
        poison_ready.wait(lock, [&] { return !poison_requested && !poison_in_progress; });
    }

    void shutdown_poison() noexcept {
        {
            std::lock_guard lock(mutex);
            poison_shutdown = true;
            poison_requested = false;
        }
        poison_ready.notify_all();
        if (poison_worker.joinable())
            poison_worker.join();
    }

    void disconnected() {
        {
            std::lock_guard lock(mutex);
            connected = false;
            host_open = false;
            stopping = true;
            if (last_error.empty()) {
                last_error = "connection-lost";
                last_explanation = "the host carrier disconnected";
            }
            for (auto& [route, job] : active) {
                (void)route;
                job->cancelled.store(true, std::memory_order_release);
            }
        }
        ready.notify_all();
        opened_ready.notify_all();
    }

    void receive(const void* data, std::size_t size) {
        ControlProtocolDiagnostics diagnostics;
        auto envelope = decode_control_envelope(
            std::string_view(static_cast<const char*>(data), size), &diagnostics);
        if (!envelope ||
            !control_envelope_allowed(*envelope, ControlEnvelopeDirection::BrokerToHost)) {
            fail("malformed-host-frame",
                 envelope ? "the broker sent a role-invalid host frame" : diagnostics.explanation);
            return;
        }
        if (auto* result = std::get_if<ControlHostOpenResult>(&envelope->payload)) {
            bool valid = false;
            {
                std::lock_guard lock(mutex);
                valid = !expected_open_request.empty() &&
                        result->request_id == expected_open_request && !opened;
                if (valid)
                    opened = std::move(*result);
            }
            if (!valid)
                fail("unexpected-host-opened", "host-opened did not match an active request");
            else
                opened_ready.notify_all();
            return;
        }
        if (const auto* cancel = std::get_if<ControlHostCancelEnvelope>(&envelope->payload)) {
            std::lock_guard lock(mutex);
            const auto found = active.find(cancel->route_id);
            if (found != active.end())
                found->second->cancelled.store(true, std::memory_order_release);
            return;
        }
        const auto* execute = std::get_if<ControlHostExecuteEnvelope>(&envelope->payload);
        if (!execute) {
            fail("unexpected-host-frame", "the broker sent an unsupported host frame");
            return;
        }
        bool overflow = false;
        bool duplicate = false;
        {
            std::lock_guard lock(mutex);
            if (!host_open || stopping) {
                overflow = true;
            } else if (active.contains(execute->route_id)) {
                duplicate = true;
            } else if (active.size() >= config.maximum_queued_executions) {
                overflow = true;
            } else {
                auto job = std::make_shared<Job>();
                job->execute = *execute;
                active.emplace(execute->route_id, job);
                queue.push_back(std::move(job));
            }
        }
        if (duplicate) {
            fail("duplicate-host-route", "the broker repeated an active host route identity");
        } else if (overflow) {
            (void)send_completion(execute->route_id,
                                  {.terminal_state = ControlReceiptState::Failed,
                                   .result = {.result_code = ControlResultCode::ResourceExhausted,
                                              .retry = ControlRetryClassification::AfterBackoff,
                                              .explanation = "the host execution queue is full"}});
        } else {
            ready.notify_one();
        }
    }

    void execute_job(const std::shared_ptr<Job>& job) {
        const auto& wire = job->execute;
        ControlAdmissionPlan plan;
        {
            std::lock_guard lock(mutex);
            plan.registration_id = registration_id;
        }
        plan.receipt_id = ControlReceiptId{wire.receipt_id};
        plan.operation_id = wire.operation_id;
        plan.operation_version = wire.operation_version;
        plan.deadline_unix_ms = wire.deadline_unix_ms;
        plan.expected_state_generation = wire.expected_state_generation;
        ControlRequestEnvelope request{
            .request_id = wire.route_id,
            .registration_id = plan.registration_id.value,
            .operation_id = wire.operation_id,
            .operation_version = wire.operation_version,
            .deadline_unix_ms = wire.deadline_unix_ms,
            .expected_state_generation = wire.expected_state_generation,
            .params_json = wire.params_json,
        };
        const ControlExecutionContext context{
            .report_progress =
                [this, route = wire.route_id](std::uint64_t current, std::uint64_t total,
                                              std::string detail) {
                    return send(ControlEnvelope{.payload = ControlHostProgressEnvelope{
                                                    .route_id = route,
                                                    .current = current,
                                                    .total = total,
                                                    .detail_json = std::move(detail),
                                                }});
                },
            .checkpoint =
                [job] {
                    return job->cancelled.load(std::memory_order_acquire)
                               ? ControlExecutionCheckpoint::Cancelled
                               : ControlExecutionCheckpoint::Continue;
                },
            .complete_deferred =
                [impl = shared_from_this(),
                 route = wire.route_id](ControlExecutionOutcome outcome) {
                    if (impl->send_completion(route, std::move(outcome))) {
                        std::lock_guard lock(impl->mutex);
                        impl->active.erase(route);
                    }
                },
        };
        auto outcome = executor(plan, request, context);
        if (!outcome.deferred) {
            if (send_completion(wire.route_id, std::move(outcome))) {
                std::lock_guard lock(mutex);
                active.erase(wire.route_id);
            }
        }
    }

    void run() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping)
                    return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            execute_job(job);
        }
    }
};

ControlHostConnection::ControlHostConnection(ControlHostConnectionConfig config,
                                             ControlOperationExecutor executor)
    : impl_(std::make_shared<Impl>(std::move(config), std::move(executor))) {}

ControlHostConnection::~ControlHostConnection() {
    disconnect();
    impl_->shutdown_poison();
}

bool ControlHostConnection::connect() {
    disconnect();
    impl_->wait_for_poison();
    if (impl_->config.endpoint_path.empty() || !impl_->config.endpoint_path.is_absolute() ||
        impl_->config.maximum_queued_executions == 0 || !impl_->executor)
        return false;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = false;
        impl_->last_error.clear();
        impl_->last_explanation.clear();
    }
    if (!impl_->connection.connect(impl_->config.endpoint_path.string(), IpcTransport::LocalSocket,
                                   impl_->config.connect_timeout) ||
        !verify_control_peer(impl_->connection, impl_->config.expected_broker)) {
        impl_->fail("broker-verification-failed",
                    "the live local peer did not match the expected broker");
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->connected = true;
    }
    impl_->worker = std::thread([impl = impl_] { impl->run(); });
    return true;
}

ControlHostOpenResult ControlHostConnection::open_host(std::string_view admission_id,
                                                       std::chrono::milliseconds timeout) {
    const auto request_id = "host-open-" + std::to_string(impl_->next_request.fetch_add(1));
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->connected || impl_->host_open || admission_id.empty() || timeout.count() <= 0)
            return {.request_id = request_id,
                    .error_code = "invalid-host-open",
                    .explanation = "a connected unopened host and valid admission are required"};
        impl_->opened.reset();
        impl_->expected_open_request = request_id;
    }
    if (!impl_->send(ControlEnvelope{.payload = ControlHostOpenEnvelope{
                                         .request_id = request_id,
                                         .admission_id = std::string(admission_id),
                                     }})) {
        impl_->fail("send-failed", "the host-open frame could not be sent");
    }
    std::unique_lock lock(impl_->mutex);
    if (!impl_->opened_ready.wait_for(lock, timeout,
                                      [&] { return impl_->opened || !impl_->connected; })) {
        lock.unlock();
        impl_->fail("timeout", "host-open timed out");
        return {.request_id = request_id,
                .error_code = "timeout",
                .explanation = "host-open timed out"};
    }
    if (!impl_->opened)
        return {.request_id = request_id,
                .error_code = impl_->last_error,
                .explanation = impl_->last_explanation};
    auto result = std::move(*impl_->opened);
    impl_->opened.reset();
    impl_->expected_open_request.clear();
    impl_->host_open = result.accepted;
    if (result.accepted)
        impl_->registration_id = ControlRegistrationId{result.registration_id};
    return result;
}

void ControlHostConnection::disconnect() noexcept {
    if (!impl_)
        return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        impl_->connected = false;
        impl_->host_open = false;
        for (auto& [route, job] : impl_->active) {
            (void)route;
            job->cancelled.store(true, std::memory_order_release);
        }
    }
    impl_->ready.notify_all();
    impl_->opened_ready.notify_all();
    impl_->connection.disconnect();
    if (impl_->worker.joinable())
        impl_->worker.join();
    std::lock_guard lock(impl_->mutex);
    impl_->queue.clear();
    impl_->active.clear();
}

bool ControlHostConnection::is_connected() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->connected && impl_->connection.is_connected();
}

bool ControlHostConnection::is_host_open() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->host_open;
}

std::string ControlHostConnection::last_error_code() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->last_error;
}

std::string ControlHostConnection::last_error_explanation() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->last_explanation;
}

} // namespace pulp::inspect
