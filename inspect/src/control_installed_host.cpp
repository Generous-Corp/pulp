#include <pulp/inspect/control_installed_host.hpp>

#include <pulp/inspect/control_executor_slot.hpp>
#include <pulp/inspect/control_host_connection.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/inspect/motion_scrubber.hpp>
#include <pulp/runtime/crypto.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::inspect {
namespace {

ControlExecutionOutcome not_implemented() {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = ControlResultCode::NotImplemented,
                       .explanation = "installed host does not implement this operation"}};
}

struct ProjectedAuthority : std::enable_shared_from_this<ProjectedAuthority> {
    struct Subscription {
        std::weak_ptr<ProjectedAuthority> authority;
        std::uint64_t id = 0;
        ~Subscription() {
            if (const auto locked = authority.lock()) {
                std::lock_guard lock(locked->mutex);
                locked->callbacks.erase(id);
            }
        }
    };

    bool is_live() const noexcept {
        return live.load(std::memory_order_acquire);
    }

    std::shared_ptr<void> subscribe(std::function<void()> callback) {
        if (!callback)
            return {};
        std::lock_guard lock(mutex);
        if (!live.load(std::memory_order_acquire))
            return {};
        const auto id = ++next_id;
        callbacks.emplace(id, std::move(callback));
        auto subscription = std::make_shared<Subscription>();
        subscription->authority = weak_from_this();
        subscription->id = id;
        return subscription;
    }

    void end() noexcept {
        if (!live.exchange(false, std::memory_order_acq_rel))
            return;
        std::vector<std::function<void()>> ended;
        {
            std::lock_guard lock(mutex);
            for (auto& [_, callback] : callbacks)
                ended.push_back(std::move(callback));
            callbacks.clear();
        }
        for (auto& callback : ended)
            callback();
    }

    std::atomic<bool> live{true};
    std::mutex mutex;
    std::uint64_t next_id = 0;
    std::unordered_map<std::uint64_t, std::function<void()>> callbacks;
};

} // namespace

struct ControlInstalledHost::State : std::enable_shared_from_this<State> {
    mutable std::mutex mutex;
    std::condition_variable heartbeat_changed;
    ControlOperationExecutorSlot slot;
    std::unique_ptr<ControlHostConnection> connection;
    std::unique_ptr<ControlTraceSessionExecutor> trace;
    std::unique_ptr<ControlHostObservabilityBundle> observability;
    std::unique_ptr<ControlMotionExecutor> motion;
    std::unique_ptr<ControlHostUiExecutor> ui;
    ControlOperationExecutor host_executor;
    ControlHostOpenResult opened;
    ControlHostObservabilityBinding observability_binding;
    std::unordered_map<std::string, std::shared_ptr<ProjectedAuthority>> authorities;
    std::jthread heartbeat_worker;
    std::chrono::milliseconds heartbeat_interval{};
    std::chrono::milliseconds handshake_timeout{};
    MotionInspector* motion_inspector = nullptr;
    MotionScrubber* motion_scrubber = nullptr;
    bool active = false;
    bool stopping = false;

    std::shared_ptr<ProjectedAuthority> authority(std::string_view id, bool create) {
        std::lock_guard lock(mutex);
        const auto found = authorities.find(std::string(id));
        if (found != authorities.end())
            return found->second;
        if (!create || stopping || id.empty())
            return {};
        auto value = std::make_shared<ProjectedAuthority>();
        authorities.emplace(id, value);
        return value;
    }

    void authority_ended(std::string_view id) noexcept {
        std::shared_ptr<ProjectedAuthority> ended;
        {
            std::lock_guard lock(mutex);
            const auto found = authorities.find(std::string(id));
            if (found != authorities.end()) {
                ended = std::move(found->second);
                authorities.erase(found);
            }
        }
        if (ended)
            ended->end();
        if (trace)
            trace->end_authority(id);
        if (observability)
            observability->end_authority(id);
    }

    void end_all() noexcept {
        std::vector<std::shared_ptr<ProjectedAuthority>> ended;
        {
            std::lock_guard lock(mutex);
            for (auto& [_, authority] : authorities)
                ended.push_back(std::move(authority));
            authorities.clear();
            active = false;
        }
        for (auto& authority : ended)
            authority->end();
        if (trace)
            trace->disconnect();
        if (observability)
            observability->disconnect();
        if (ui)
            (void)ui->disconnect();
    }

    bool exact_plan(const ControlAdmissionPlan& plan) const {
        return plan.registration_id.value == opened.registration_id &&
               plan.broker_id.value == opened.broker_id && plan.session_id == opened.session_id &&
               plan.instance_id == opened.instance_id &&
               plan.publication_id == opened.publication_id &&
               plan.instance_generation == opened.instance_generation &&
               plan.manifest_digest == opened.manifest_digest &&
               plan.producer_artifact_digest == opened.producer_artifact_digest &&
               plan.client_id.value == plan.grant_id.value &&
               plan.client_id.value == plan.client_principal;
    }

    bool accepts(const ControlAdmissionPlan& plan) const {
        std::lock_guard lock(mutex);
        return active && !stopping && exact_plan(plan);
    }

    ControlOperationExecutor composite_executor() {
        const auto weak = weak_from_this();
        const auto observability_executor = observability->executor();
        const auto motion_executor = motion->executor();
        return [weak, observability_executor, motion_executor](
                   const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                   const ControlExecutionContext& context) {
            const auto state = weak.lock();
            if (!state || !state->accepts(plan))
                return ControlExecutionOutcome{
                    .terminal_state = ControlReceiptState::Failed,
                    .result = {.result_code = ControlResultCode::PolicyDenied,
                               .explanation = "projected host authority is not exact"}};
            auto authority = state->authority(plan.client_id.value, true);
            if (!authority || !authority->is_live())
                return ControlExecutionOutcome{
                    .terminal_state = ControlReceiptState::Cancelled,
                    .result = {.result_code = ControlResultCode::Cancelled,
                               .explanation = "projected host authority has ended",
                               .cancellation_reason = "authority-ended"}};
            if (request.operation_id == "dev.pulp.trace/session-control@1" ||
                request.operation_id == "dev.pulp.telemetry/subscribe@1")
                return observability_executor(plan, request, context);
            if (request.operation_id == "dev.pulp.trace/control@1")
                return motion_executor(plan, request, context);
            if (state->ui &&
                (request.operation_id == "dev.pulp.ui/capture@1" ||
                 request.operation_id == "dev.pulp.ui/input@1" ||
                 request.operation_id == "dev.pulp.runtime/evaluate@1"))
                return state->ui->executor()(plan, request, context);
            if (state->host_executor)
                return state->host_executor(plan, request, context);
            return not_implemented();
        };
    }

    void run_heartbeat(std::stop_token stop) {
        std::unique_lock lock(mutex);
        while (!stop.stop_requested() && !stopping) {
            if (heartbeat_changed.wait_for(lock, heartbeat_interval,
                                           [&] { return stop.stop_requested() || stopping; }))
                break;
            lock.unlock();
            const bool broker_live = connection && connection->heartbeat(handshake_timeout);
            const bool bundle_live =
                broker_live && observability && observability->heartbeat(observability_binding);
            if (!broker_live || !bundle_live) {
                slot.close();
                if (connection)
                    connection->disconnect();
                end_all();
                return;
            }
            lock.lock();
        }
    }
};

ControlInstalledHost::ControlInstalledHost(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

ControlInstalledHost::~ControlInstalledHost() {
    stop();
}

std::unique_ptr<ControlInstalledHost>
ControlInstalledHost::start(ControlInstalledHostConfig config) {
    if (config.bootstrap.enrollment_id.empty() || !config.main_thread_rpc ||
        !config.trace_inspector || !config.telemetry || !config.motion_inspector ||
        config.heartbeat_interval.count() <= 0 || config.heartbeat_ttl.count() <= 0 ||
        config.heartbeat_interval >= config.heartbeat_ttl || config.handshake_timeout.count() <= 0)
        return nullptr;
    auto state = std::make_shared<State>();
    state->heartbeat_interval = config.heartbeat_interval;
    state->handshake_timeout = config.handshake_timeout;
    state->motion_inspector = config.motion_inspector;
    state->motion_scrubber = config.motion_scrubber;
    state->host_executor = std::move(config.host_executor);
    std::weak_ptr<State> weak = state;
    state->connection = std::make_unique<ControlHostConnection>(
        ControlHostConnectionConfig{
            .endpoint_path = config.bootstrap.endpoint_path,
            .expected_broker = config.bootstrap.expected_broker,
            .connect_timeout = config.handshake_timeout,
            .write_timeout = config.handshake_timeout,
            .frame_read_timeout = config.handshake_timeout,
            .authority_ended = [weak](std::string_view id, std::string_view) {
                if (const auto locked = weak.lock())
                    locked->authority_ended(id);
            },
            .disconnected = [weak] {
                if (const auto locked = weak.lock())
                    locked->end_all();
            }},
        state->slot.executor());
    if (!state->connection->connect())
        return nullptr;
    state->opened = state->connection->open_host_enrollment(config.bootstrap.enrollment_id,
                                                            config.handshake_timeout);
    config.bootstrap.clear();
    if (!state->opened.accepted)
        return nullptr;

    state->trace = ControlTraceSessionExecutor::create({
        .main_thread_rpc = config.main_thread_rpc,
        .trace_inspector = std::move(config.trace_inspector),
        .registration_id = ControlRegistrationId{state->opened.registration_id},
    });
    const auto token = runtime::secure_random_bytes(32);
    if (!state->trace || !token)
        return nullptr;
    state->observability_binding = {
        .registration_id = ControlRegistrationId{state->opened.registration_id},
        .session_id = state->opened.session_id,
        .instance_id = state->opened.instance_id,
        .publication_id = state->opened.publication_id,
        .authentication_token = runtime::hex_encode(*token),
    };
    state->observability = ControlHostObservabilityBundle::create({
        .binding = state->observability_binding,
        .trace_executor = state->trace->executor(),
        .telemetry = std::move(config.telemetry),
        .heartbeat_ttl = config.heartbeat_ttl,
    });
    state->motion = ControlMotionExecutor::create({
        .main_thread_rpc = config.main_thread_rpc,
        .resolve_target = [weak](const ControlAdmissionPlan& plan)
            -> std::optional<ControlMotionTarget> {
            const auto state = weak.lock();
            if (!state || !state->exact_plan(plan))
                return std::nullopt;
            const auto authority = state->authority(plan.client_id.value, false);
            if (!authority)
                return std::nullopt;
            return ControlMotionTarget{
                .registration_id = plan.registration_id,
                .host_tier = ControlHostTier::Standalone,
                .session_id = plan.session_id,
                .instance_id = plan.instance_id,
                .publication_id = plan.publication_id,
                .instance_generation = plan.instance_generation,
                .authority_live = [authority] { return authority->is_live(); },
                .subscribe_authority_end = [authority](std::function<void()> callback) {
                    return authority->subscribe(std::move(callback));
                },
                .inspector = state->motion_inspector,
                .scrubber = state->motion_scrubber,
            };
        },
    });
    if (config.ui) {
        std::optional<ControlInstalledHostUiTargets> ui_targets;
        if (config.ui->make_targets) {
            ui_targets = config.ui->make_targets(state->opened);
            if (!ui_targets)
                return nullptr;
        }
        ControlRegistration registration{
            .registration_id = ControlRegistrationId{state->opened.registration_id},
            .broker_id = ControlBrokerId{state->opened.broker_id},
            .host_tier = ControlHostTier::Standalone,
            .session_id = state->opened.session_id,
            .instance_id = state->opened.instance_id,
            .publication_id = state->opened.publication_id,
            .plugin_id = config.ui->manifest.bundle_id,
            .publisher_id = config.ui->manifest.product_name,
            .manifest_digest = state->opened.manifest_digest,
            .artifact_digest = state->opened.producer_artifact_digest,
            .consent_identity = control_consent_identity(state->opened.manifest_digest,
                                                         state->opened.producer_artifact_digest),
            .profile = config.ui->manifest.profile,
            .capabilities = config.ui->manifest.capabilities,
            .build_id = config.ui->manifest.build_id,
        };
        state->ui = ControlHostUiExecutor::create({
            .binding = {.registration = std::move(registration),
                        .manifest = std::move(config.ui->manifest)},
            .main_thread_rpc = config.main_thread_rpc,
            .capture_source = ui_targets ? std::move(ui_targets->capture_source) : nullptr,
            .target_adapter = ui_targets ? std::move(ui_targets->target_adapter) : nullptr,
            .resolve_authority = [weak](const ControlAdmissionPlan& plan)
                -> std::optional<ControlUiProjectedAuthority> {
                const auto state = weak.lock();
                if (!state || !state->exact_plan(plan))
                    return std::nullopt;
                const auto authority = state->authority(plan.client_id.value, false);
                if (!authority)
                    return std::nullopt;
                return ControlUiProjectedAuthority{
                    .owner = {.authority_id = plan.client_id.value},
                    .authority_live = [authority] { return authority->is_live(); },
                    .subscribe_authority_end = [authority](std::function<void()> callback) {
                        return authority->subscribe(std::move(callback));
                    }};
            },
            .view_generation = ui_targets ? std::move(ui_targets->view_generation) : std::string{},
            .runtime_evaluator = std::move(config.ui->runtime_evaluator),
            .redact_runtime_eval_result = std::move(config.ui->redact_runtime_eval_result),
        });
    }
    if (!state->observability || !state->motion || (config.ui && !state->ui) ||
        !state->slot.install(state->composite_executor()))
        return nullptr;
    const auto ready = state->connection->mark_executor_ready(config.handshake_timeout);
    if (!ready.accepted)
        return nullptr;
    {
        std::lock_guard lock(state->mutex);
        state->active = true;
    }
    state->heartbeat_worker =
        std::jthread([raw = state.get()](std::stop_token stop) { raw->run_heartbeat(stop); });
    return std::unique_ptr<ControlInstalledHost>(new ControlInstalledHost(std::move(state)));
}

bool ControlInstalledHost::ready() const {
    std::lock_guard lock(state_->mutex);
    return state_->active && state_->connection && state_->connection->is_host_open() &&
           state_->observability && state_->observability->ready();
}

const ControlHostOpenResult& ControlInstalledHost::binding() const {
    return state_->opened;
}

void ControlInstalledHost::stop() noexcept {
    if (!state_)
        return;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping)
            return;
        state_->stopping = true;
    }
    state_->heartbeat_worker.request_stop();
    state_->heartbeat_changed.notify_all();
    if (state_->heartbeat_worker.joinable())
        state_->heartbeat_worker.join();
    state_->slot.close();
    if (state_->connection)
        state_->connection->disconnect();
    state_->end_all();
}

} // namespace pulp::inspect
