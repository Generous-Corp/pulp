#include <pulp/inspect/session.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <exception>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::inspect {
namespace {

bool contains(std::span<const InspectorCapability> capabilities,
              InspectorCapability capability) {
    return std::find(capabilities.begin(), capabilities.end(), capability) !=
           capabilities.end();
}

void append_unique(std::vector<InspectorCapability>& capabilities,
                   InspectorCapability capability) {
    if (!contains(capabilities, capability))
        capabilities.push_back(capability);
}

std::string authorization_data(std::string_view method,
                               InspectorCapability capability,
                               InspectorProfile profile) {
    auto data = choc::value::createObject("");
    data.addMember("method", choc::value::createString(method));
    data.addMember("capability",
                   choc::value::createString(capability_id(capability)));
    data.addMember("profile",
                   choc::value::createString(profile_id(profile)));
    return choc::json::toString(data, false);
}

std::string lease_result_id(ControllerLeaseResult result) {
    switch (result) {
        case ControllerLeaseResult::Acquired: return "acquired";
        case ControllerLeaseResult::Renewed: return "renewed";
        case ControllerLeaseResult::HeldByOther: return "held_by_other";
        case ControllerLeaseResult::InvalidOwner: return "invalid_owner";
    }
    return "invalid_owner";
}

std::optional<InspectorMessage> validate_request_shape(
    const InspectorMessage& request) {
    const auto* method = find_inspector_method(request.method);
    if (request.id == 0 ||
        (method && method->kind != InspectorMethodKind::Request)) {
        return make_error(request.id,
                          "Inspector messages sent to a session must be requests",
                          "invalid_request");
    }
    return std::nullopt;
}

void deliver_scope_ends(
    const InspectorSessionInfo& info,
    std::vector<InspectorControllerLease::EndedScope> ended,
    const InspectorSession::ControllerScopeEndHandler& handler) {
    if (!handler)
        return;
    for (auto& event : ended) {
        try {
            handler({info.session_id, info.instance_id, std::move(event.owner),
                     event.reason});
        } catch (...) {
            // Cleanup notification must not unwind a transport thread or the
            // idle-expiry worker. Host callbacks own their error reporting.
        }
    }
}

void deliver_client_disconnect(
    const InspectorSession::ClientDisconnectHandler& handler,
    std::string_view client_id) noexcept {
    if (!handler)
        return;
    try {
        handler(client_id);
    } catch (...) {
        // A transport-loss callback must never escape the reader/cleanup
        // thread. Domain cleanup is best-effort once the authenticated client
        // is gone; throwing cannot restore that connection.
    }
}

void append_audit(const std::shared_ptr<InspectorAuditLog>& audit_log,
                  const InspectorSessionInfo& info,
                  std::string_view client_id,
                  const InspectorMessage& request,
                  InspectorCapability capability,
                  InspectorAuditOutcome outcome,
                  std::string error_code = {}) {
    if (!audit_log)
        return;
    audit_log->append({0, request.id, info.session_id, info.instance_id,
                       std::string(client_id), request.method, capability,
                       outcome, std::move(error_code)});
}

} // namespace

InspectorAccessPolicy::InspectorAccessPolicy(InspectorPolicyConfig config)
    : profile_(config.profile) {
    for (const auto capability : config.available_capabilities) {
        if (!capability_is_grantable(capability))
            continue;
        if (capability == InspectorCapability::RuntimeEval &&
            !config.runtime_eval_enabled)
            continue;
        append_unique(available_, capability);
    }

    const auto requested = profile_ == InspectorProfile::Custom
        ? std::span<const InspectorCapability>(config.custom_capabilities)
        : profile_capabilities(profile_);
    const bool runtime_eval_requested = config.runtime_eval_enabled &&
        (profile_ == InspectorProfile::Develop ||
         (profile_ == InspectorProfile::Custom &&
          contains(requested, InspectorCapability::RuntimeEval)));
    const bool can_acquire_controller =
        contains(available_, InspectorCapability::SessionControl) &&
        contains(requested, InspectorCapability::SessionControl);
    for (const auto capability : requested) {
        if (capability == InspectorCapability::RuntimeEval)
            continue;
        if (!contains(available_, capability))
            continue;
        if (capability != InspectorCapability::SessionControl &&
            capability_requires_controller_lease(capability) &&
            !can_acquire_controller) {
            continue;
        }
        append_unique(effective_, capability);
    }
    if (runtime_eval_requested && can_acquire_controller &&
        contains(available_, InspectorCapability::RuntimeEval)) {
        append_unique(effective_, InspectorCapability::RuntimeEval);
    }
}

bool InspectorAccessPolicy::is_available(InspectorCapability capability) const {
    return contains(available_, capability);
}

bool InspectorAccessPolicy::is_granted(InspectorCapability capability) const {
    return contains(effective_, capability);
}

std::optional<InspectorMessage> InspectorAccessPolicy::authorize(
    const InspectorMessage& request,
    bool owns_controller_lease) const {
    if (auto invalid = validate_request_shape(request))
        return invalid;
    const auto* method = find_inspector_method(request.method);
    if (!method) {
        auto data = choc::value::createObject("");
        data.addMember("method", choc::value::createString(request.method));
        return make_error(request.id,
                          "Unknown inspector method",
                          "method_not_found",
                          choc::json::toString(data, false));
    }
    const auto capability = method->capability;
    const auto data = authorization_data(request.method, capability, profile_);
    if (!capability_is_grantable(capability) || !is_available(capability)) {
        return make_error(request.id,
                          "Inspector capability is unavailable",
                          "capability_unavailable",
                          data);
    }
    if (!is_granted(capability)) {
        return make_error(request.id,
                          "Inspector capability is not granted",
                          "capability_denied",
                          data);
    }
    if (capability_requires_controller_lease(capability) &&
        !owns_controller_lease) {
        return make_error(request.id,
                          "A controller lease is required",
                          "controller_lease_required",
                          data);
    }
    return std::nullopt;
}

InspectorControllerLease::InspectorControllerLease(
    std::chrono::milliseconds ttl,
    Clock clock)
    : ttl_(std::max(ttl, std::chrono::milliseconds(1))),
      clock_(std::move(clock)) {}

void InspectorControllerLease::expire_if_needed() {
    if (!owner_.empty() && clock_() >= expires_at_) {
        if (active_operations_ != 0) {
            // Expiry fences admission immediately. Keep the identity only so
            // the operations admitted before expiry can drain safely.
            release_pending_ = true;
            pending_reason_ = TestInputReleaseReason::ControllerExpired;
        } else {
            finish_scope(TestInputReleaseReason::ControllerExpired);
        }
    }
}

ControllerLeaseResult InspectorControllerLease::acquire(
    std::string_view owner) {
    if (owner.empty())
        return ControllerLeaseResult::InvalidOwner;
    expire_if_needed();
    if (release_pending_ || (!owner_.empty() && owner_ != owner))
        return ControllerLeaseResult::HeldByOther;
    const bool renewing = owner_ == owner;
    owner_ = std::string(owner);
    expires_at_ = clock_() + ttl_;
    return renewing ? ControllerLeaseResult::Renewed
                    : ControllerLeaseResult::Acquired;
}

ControllerLeaseResult InspectorControllerLease::renew(
    std::string_view owner) {
    if (owner.empty())
        return ControllerLeaseResult::InvalidOwner;
    expire_if_needed();
    if (release_pending_ || owner_ != owner)
        return ControllerLeaseResult::HeldByOther;
    expires_at_ = clock_() + ttl_;
    return ControllerLeaseResult::Renewed;
}

bool InspectorControllerLease::release(std::string_view owner) {
    return release_with_reason(owner,
                               TestInputReleaseReason::ControllerReleased);
}

bool InspectorControllerLease::release_with_reason(
    std::string_view owner,
    TestInputReleaseReason reason) {
    expire_if_needed();
    if (owner.empty() || owner_ != owner)
        return false;
    if (active_operations_ != 0) {
        release_pending_ = true;
        pending_reason_ = reason;
        return true;
    }
    finish_scope(reason);
    return true;
}

void InspectorControllerLease::disconnect(std::string_view owner) {
    (void)release_with_reason(owner,
                              TestInputReleaseReason::ClientDisconnected);
}

bool InspectorControllerLease::owns(std::string_view owner) {
    expire_if_needed();
    return !owner.empty() && owner_ == owner && !release_pending_;
}

std::optional<std::string> InspectorControllerLease::owner() {
    expire_if_needed();
    if (owner_.empty())
        return std::nullopt;
    return owner_;
}

std::chrono::milliseconds InspectorControllerLease::remaining() {
    expire_if_needed();
    if (owner_.empty())
        return std::chrono::milliseconds(0);
    return std::max(
        std::chrono::milliseconds(0),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            expires_at_ - clock_()));
}

bool InspectorControllerLease::begin_operation(std::string_view owner) {
    expire_if_needed();
    if (owner.empty() || owner_ != owner || release_pending_)
        return false;
    ++active_operations_;
    return true;
}

void InspectorControllerLease::end_operation(std::string_view owner) {
    if (active_operations_ == 0 || owner_.empty() || owner_ != owner)
        return;
    --active_operations_;
    if (active_operations_ == 0 &&
        (release_pending_ || clock_() >= expires_at_)) {
        finish_scope(release_pending_
                         ? pending_reason_
                         : TestInputReleaseReason::ControllerExpired);
    }
}

void InspectorControllerLease::terminate() {
    expire_if_needed();
    if (owner_.empty())
        return;
    if (active_operations_ != 0) {
        release_pending_ = true;
        pending_reason_ = TestInputReleaseReason::SessionTeardown;
        return;
    }
    finish_scope(TestInputReleaseReason::SessionTeardown);
}

std::vector<InspectorControllerLease::EndedScope>
InspectorControllerLease::take_ended_scopes() {
    auto ended = std::move(ended_scopes_);
    ended_scopes_.clear();
    return ended;
}

void InspectorControllerLease::finish_scope(TestInputReleaseReason reason) {
    if (!owner_.empty())
        ended_scopes_.push_back({owner_, reason});
    owner_.clear();
    release_pending_ = false;
    pending_reason_ = TestInputReleaseReason::ControllerReleased;
}

class InspectorSession::State {
public:
    State(InspectorSessionInfo session_info,
          ContextRequestHandler request_handler,
          std::chrono::milliseconds lease_ttl,
          InspectorControllerLease::Clock clock)
        : info(std::move(session_info)),
          handler(std::move(request_handler)),
          lease(lease_ttl, std::move(clock)),
          expiry_thread([this](std::stop_token stop) {
              std::unique_lock lock(mutex);
              while (!stop.stop_requested()) {
                  const auto wait = lease.owner().has_value()
                      ? std::max(lease.remaining(),
                                 std::chrono::milliseconds(1))
                      : std::chrono::seconds(1);
                  auto ended = lease.take_ended_scopes();
                  auto callback = controller_scope_end_handler;
                  if (!ended.empty()) {
                      lock.unlock();
                      deliver_scope_ends(info, std::move(ended), callback);
                      lock.lock();
                      continue;
                  }
                  expiry_cv.wait_for(lock, wait);
              }
              auto keep_alive = std::move(expiry_keep_alive);
              lock.unlock();
              (void)keep_alive;
          }) {}

    ~State() {
        request_expiry_stop();
        join_expiry();
    }

    void request_expiry_stop() {
        expiry_thread.request_stop();
        expiry_cv.notify_all();
    }

    bool on_expiry_thread() const {
        return expiry_thread.joinable() &&
               expiry_thread.get_id() == std::this_thread::get_id();
    }

    void join_expiry() {
        if (expiry_thread.joinable() && !on_expiry_thread())
            expiry_thread.join();
    }

    void retain_until_expiry_exit(std::shared_ptr<State> state) {
        std::lock_guard lock(mutex);
        expiry_keep_alive = std::move(state);
        expiry_thread.detach();
    }

    bool begin_dispatch() {
        const auto caller = std::this_thread::get_id();
        std::unique_lock lock(dispatch_mutex);
        if (!dispatch_accepting)
            return false;
        if (dispatch_active && dispatch_owner == caller) {
            ++dispatch_recursion;
            return true;
        }
        dispatch_cv.wait(lock, [this] {
            return !dispatch_accepting || !dispatch_active;
        });
        if (!dispatch_accepting)
            return false;
        dispatch_active = true;
        dispatch_owner = caller;
        dispatch_recursion = 1;
        return true;
    }

    void end_dispatch() {
        std::lock_guard lock(dispatch_mutex);
        if (!dispatch_active ||
            dispatch_owner != std::this_thread::get_id() ||
            dispatch_recursion == 0) {
            return;
        }
        if (--dispatch_recursion != 0)
            return;
        dispatch_active = false;
        dispatch_owner = {};
        dispatch_cv.notify_all();
    }

    InspectorSessionInfo info;
    ContextRequestHandler handler;
    InspectorControllerLease lease;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    ControllerScopeEndHandler controller_scope_end_handler;
    ClientDisconnectHandler client_disconnect_handler;
    std::unordered_map<std::string, std::uint64_t> client_epochs;
    std::unordered_map<std::string, std::size_t> admitted_client_dispatches;
    std::unordered_set<std::string> pending_client_disconnects;
    std::shared_ptr<InspectorAuditLog> audit_log;
    std::mutex mutex;
    std::condition_variable expiry_cv;
    std::mutex dispatch_mutex;
    std::condition_variable dispatch_cv;
    bool dispatch_accepting = true;
    bool dispatch_active = false;
    std::thread::id dispatch_owner;
    std::size_t dispatch_recursion = 0;
    std::shared_ptr<State> expiry_keep_alive;
    std::jthread expiry_thread;
};

InspectorSession::InspectorSession(InspectorSessionInfo info,
                                   InspectorPolicyConfig policy,
                                   RequestHandler handler,
                                   std::chrono::milliseconds lease_ttl,
                                   InspectorControllerLease::Clock clock)
    : InspectorSession(
          std::move(info), std::move(policy),
          [handler = std::move(handler)](
              const InspectorRequestContext&,
              const InspectorMessage& request) {
              return handler(request);
          },
          lease_ttl, std::move(clock)) {}

InspectorSession::InspectorSession(InspectorSessionInfo info,
                                   InspectorPolicyConfig policy,
                                   ContextRequestHandler handler,
                                   std::chrono::milliseconds lease_ttl,
                                   InspectorControllerLease::Clock clock)
    : info_(std::move(info)),
      policy_(std::move(policy)),
      state_(std::make_shared<State>(
          info_, std::move(handler), lease_ttl, std::move(clock))) {}

InspectorSession::~InspectorSession() {
    auto state = std::move(state_);
    if (!state)
        return;

    ControllerScopeEndHandler handler;
    std::vector<InspectorControllerLease::EndedScope> ended;
    {
        std::lock_guard lock(state->mutex);
        state->lease.terminate();
        ended = state->lease.take_ended_scopes();
        handler = state->controller_scope_end_handler;
    }
    state->request_expiry_stop();

    if (state->on_expiry_thread()) {
        // The expiry callback is allowed to destroy its owning session. Keep
        // State alive until the worker exits, and detach its thread object so
        // final destruction at the end of the worker cannot self-join.
        deliver_scope_ends(info_, std::move(ended), handler);
        auto* raw_state = state.get();
        raw_state->retain_until_expiry_exit(std::move(state));
        return;
    }

    state->join_expiry();
    deliver_scope_ends(info_, std::move(ended), handler);
}

InspectorMessage InspectorSession::handle(std::string_view client_id,
                                          const InspectorMessage& request) {
    const auto state = state_;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    std::shared_ptr<InspectorAuditLog> audit_log;
    ControllerScopeEndHandler scope_end_handler;
    std::vector<InspectorControllerLease::EndedScope> ended_scopes;
    std::optional<InspectorMessage> immediate_response;
    InspectorCapability capability = InspectorCapability::Unavailable;
    bool controller_operation = false;
    std::uint64_t client_epoch = 0;
    {
        std::lock_guard lock(state->mutex);
        if (auto invalid = validate_request_shape(request))
            return std::move(*invalid);
        audit_log = state->audit_log;
        scope_end_handler = state->controller_scope_end_handler;
        if (request.method.rfind("Session.", 0) == 0) {
            immediate_response = handle_session_method(client_id, request);
            ended_scopes = state->lease.take_ended_scopes();
        } else {
            const auto* method = find_inspector_method(request.method);
            const bool requires_controller =
                method && capability_requires_controller_lease(method->capability);
            if (method)
                capability = method->capability;
            const bool owns_controller = state->lease.owns(client_id);
            ended_scopes = state->lease.take_ended_scopes();
            if (auto denied = policy_.authorize(request, owns_controller)) {
                immediate_response = std::move(*denied);
            } else if (requires_controller && !state->lease.begin_operation(client_id)) {
                immediate_response = make_error(request.id, "A controller lease is required",
                                                "controller_lease_required");
            } else {
                controller_operation = requires_controller;
                main_thread_rpc = state->main_thread_rpc;
                client_epoch = state->client_epochs[std::string(client_id)];
                ++state->admitted_client_dispatches[std::string(client_id)];
            }
        }
    }
    state->expiry_cv.notify_all();
    deliver_scope_ends(info_, std::move(ended_scopes), scope_end_handler);
    if (immediate_response) {
        if (capability_requires_controller_lease(capability)) {
            append_audit(audit_log, info_, client_id, request, capability,
                         InspectorAuditOutcome::Denied, immediate_response->error_code);
        }
        return std::move(*immediate_response);
    }

    const auto owned_client_id = std::string(client_id);
    const auto owned_info = info_;
    const auto finish_operation = [state, owned_client_id, owned_info,
                                   controller_operation] {
        ControllerScopeEndHandler handler;
        ClientDisconnectHandler disconnect_handler;
        std::vector<InspectorControllerLease::EndedScope> ended;
        bool deliver_disconnect = false;
        {
            std::lock_guard lock(state->mutex);
            if (controller_operation) {
                state->lease.end_operation(owned_client_id);
                ended = state->lease.take_ended_scopes();
                handler = state->controller_scope_end_handler;
            }
            auto admitted = state->admitted_client_dispatches.find(owned_client_id);
            if (admitted != state->admitted_client_dispatches.end() &&
                --admitted->second == 0) {
                state->admitted_client_dispatches.erase(admitted);
                state->client_epochs.erase(owned_client_id);
                deliver_disconnect =
                    state->pending_client_disconnects.erase(owned_client_id) != 0;
                if (deliver_disconnect) {
                    disconnect_handler = state->client_disconnect_handler;
                }
            }
        }
        deliver_scope_ends(owned_info, std::move(ended), handler);
        if (deliver_disconnect)
            deliver_client_disconnect(disconnect_handler, owned_client_id);
    };
    if (!state->handler) {
        finish_operation();
        auto response = make_error(request.id, "No inspector dispatch handler is attached",
                                   "dispatch_unavailable");
        if (controller_operation) {
            append_audit(audit_log, info_, client_id, request, capability,
                         InspectorAuditOutcome::Rejected, response.error_code);
        }
        return response;
    }
    const auto owned_request = request;
    auto operation = [state, owned_request, owned_client_id, owned_info, audit_log, capability,
                      controller_operation, client_epoch] {
        InspectorMessage response;
        if (!state->begin_dispatch()) {
            response =
                make_error(owned_request.id, "Inspector dispatch was cancelled during teardown",
                           "dispatch_cancelled");
        } else {
            struct DispatchGuard {
                std::shared_ptr<State> state;
                ~DispatchGuard() {
                    state->end_dispatch();
                }
            } guard{state};
            {
                std::lock_guard lock(state->mutex);
                const auto current_epoch = state->client_epochs[owned_client_id];
                if (current_epoch != client_epoch) {
                    response = make_error(
                        owned_request.id,
                        "Inspector dispatch was cancelled after client disconnect",
                        "client_disconnected");
                }
            }
            if (!response.is_error) {
                try {
                    response = state->handler(
                        InspectorRequestContext{owned_client_id}, owned_request);
                } catch (const std::exception& error) {
                    response = make_error(owned_request.id,
                                          std::string("Inspector dispatch failed: ") + error.what(),
                                          "dispatch_failed");
                } catch (...) {
                    response = make_error(owned_request.id, "Inspector dispatch failed",
                                          "dispatch_failed");
                }
            }
        }
        if (controller_operation) {
            append_audit(audit_log, owned_info, owned_client_id, owned_request, capability,
                         response.is_error ? InspectorAuditOutcome::Rejected
                                           : InspectorAuditOutcome::Applied,
                         response.error_code);
        }
        return response;
    };

    if (!main_thread_rpc) {
        auto response = [&] {
            try {
                return operation();
            } catch (const std::exception& error) {
                return make_error(request.id,
                                  std::string("Inspector dispatch failed: ") + error.what(),
                                  "dispatch_failed");
            } catch (...) {
                return make_error(request.id, "Inspector dispatch failed", "dispatch_failed");
            }
        }();
        finish_operation();
        return response;
    }
    return main_thread_rpc->call(request.id, std::move(operation), finish_operation);
}

void InspectorSession::suspend_dispatches() {
    const auto state = state_;
    {
        std::lock_guard lock(state->dispatch_mutex);
        state->dispatch_accepting = false;
    }
    state->dispatch_cv.notify_all();

    ControllerScopeEndHandler handler;
    std::vector<InspectorControllerLease::EndedScope> ended;
    {
        std::lock_guard lock(state->mutex);
        state->lease.terminate();
        ended = state->lease.take_ended_scopes();
        handler = state->controller_scope_end_handler;
    }
    deliver_scope_ends(info_, std::move(ended), handler);
}

void InspectorSession::resume_dispatches() {
    const auto state = state_;
    {
        std::lock_guard lock(state->dispatch_mutex);
        state->dispatch_accepting = true;
    }
    state->dispatch_cv.notify_all();
}

bool InspectorSession::dispatches_accepting() const {
    const auto state = state_;
    std::lock_guard lock(state->dispatch_mutex);
    return state->dispatch_accepting;
}

void InspectorSession::set_main_thread_rpc(
    std::shared_ptr<InspectorMainThreadRpc> rpc) {
    std::lock_guard lock(state_->mutex);
    state_->main_thread_rpc = std::move(rpc);
}

void InspectorSession::set_controller_scope_end_handler(
    ControllerScopeEndHandler handler) {
    std::lock_guard lock(state_->mutex);
    state_->controller_scope_end_handler = std::move(handler);
}

void InspectorSession::set_client_disconnect_handler(
    ClientDisconnectHandler handler) {
    std::lock_guard lock(state_->mutex);
    state_->client_disconnect_handler = std::move(handler);
}

void InspectorSession::set_audit_log(
    std::shared_ptr<InspectorAuditLog> audit_log) {
    std::lock_guard lock(state_->mutex);
    state_->audit_log = std::move(audit_log);
}

void InspectorSession::disconnect(std::string_view client_id) {
    ControllerScopeEndHandler handler;
    ClientDisconnectHandler disconnect_handler;
    std::vector<InspectorControllerLease::EndedScope> ended;
    const auto owned_client_id = std::string(client_id);
    bool deliver_disconnect = false;
    {
        std::lock_guard lock(state_->mutex);
        state_->lease.disconnect(owned_client_id);
        ended = state_->lease.take_ended_scopes();
        handler = state_->controller_scope_end_handler;
        if (state_->admitted_client_dispatches.contains(owned_client_id)) {
            ++state_->client_epochs[owned_client_id];
            state_->pending_client_disconnects.insert(owned_client_id);
        } else {
            state_->pending_client_disconnects.erase(owned_client_id);
            disconnect_handler = state_->client_disconnect_handler;
            deliver_disconnect = true;
        }
    }
    deliver_scope_ends(info_, std::move(ended), handler);
    if (deliver_disconnect)
        deliver_client_disconnect(disconnect_handler, owned_client_id);
}

InspectorMessage InspectorSession::handle_session_method(
    std::string_view client_id,
    const InspectorMessage& request) {
    if (request.method == methods::kSessionGetCapabilities) {
        if (auto denied = policy_.authorize(request, false))
            return std::move(*denied);

        auto available = choc::value::createEmptyArray();
        for (const auto capability : policy_.available_capabilities())
            available.addArrayElement(
                choc::value::createString(capability_id(capability)));
        auto effective = choc::value::createEmptyArray();
        for (const auto capability : policy_.effective_capabilities())
            effective.addArrayElement(
                choc::value::createString(capability_id(capability)));

        auto result = choc::value::createObject("");
        result.addMember("sessionId",
                         choc::value::createString(info_.session_id));
        result.addMember("instanceId",
                         choc::value::createString(info_.instance_id));
        result.addMember("pluginId",
                         choc::value::createString(info_.plugin_id));
        result.addMember("protocolVersion",
                         choc::value::createString(info_.protocol_version));
        result.addMember("profile",
                         choc::value::createString(profile_id(policy_.profile())));
        result.addMember("available", available);
        result.addMember("effective", effective);
        if (const auto owner = state_->lease.owner())
            result.addMember("controller",
                             choc::value::createString(*owner));
        return make_response(request.id,
                             choc::json::toString(result, false));
    }

    const bool is_lease_method =
        request.method == methods::kSessionAcquireController ||
        request.method == methods::kSessionRenewController ||
        request.method == methods::kSessionReleaseController;
    if (!is_lease_method) {
        if (auto denied = policy_.authorize(request, false))
            return std::move(*denied);
        return make_error(request.id,
                          "Unknown session method",
                          "method_not_found");
    }

    // Acquiring the lease cannot itself require a pre-existing lease, but it
    // still requires the session.control capability.
    const auto* descriptor = find_inspector_method(request.method);
    if (!descriptor || !policy_.is_available(descriptor->capability)) {
        return make_error(request.id,
                          "Inspector capability is unavailable",
                          "capability_unavailable",
                          authorization_data(request.method,
                                             InspectorCapability::SessionControl,
                                             policy_.profile()));
    }
    if (!policy_.is_granted(descriptor->capability)) {
        return make_error(request.id,
                          "Inspector capability is not granted",
                          "capability_denied",
                          authorization_data(request.method,
                                             InspectorCapability::SessionControl,
                                             policy_.profile()));
    }

    if (request.method == methods::kSessionReleaseController) {
        if (!state_->lease.release(client_id)) {
            return make_error(request.id,
                              "The caller does not own the controller lease",
                              "controller_lease_not_owned");
        }
        return make_response(request.id, R"({"released":true})");
    }

    const auto result = request.method == methods::kSessionAcquireController
        ? state_->lease.acquire(client_id)
        : state_->lease.renew(client_id);
    if (result == ControllerLeaseResult::HeldByOther) {
        return make_error(request.id,
                          "The controller lease is held by another client",
                          "controller_lease_conflict");
    }
    if (result == ControllerLeaseResult::InvalidOwner) {
        return make_error(request.id,
                          "Authenticated client identity is required",
                          "invalid_client_identity");
    }

    auto response = choc::value::createObject("");
    response.addMember("status",
                       choc::value::createString(lease_result_id(result)));
    response.addMember("ttlMs",
                       choc::value::createInt64(
                           state_->lease.remaining().count()));
    return make_response(request.id,
                         choc::json::toString(response, false));
}

} // namespace pulp::inspect
