#include <pulp/inspect/session.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <exception>
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
    const bool can_acquire_controller =
        contains(available_, InspectorCapability::SessionControl) &&
        contains(requested, InspectorCapability::SessionControl);
    for (const auto capability : requested) {
        if (!contains(available_, capability))
            continue;
        if (capability != InspectorCapability::SessionControl &&
            capability_requires_controller_lease(capability) &&
            !can_acquire_controller) {
            continue;
        }
        append_unique(effective_, capability);
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
        } else {
            owner_.clear();
            release_pending_ = false;
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
    expire_if_needed();
    if (owner.empty() || owner_ != owner)
        return false;
    if (active_operations_ != 0) {
        release_pending_ = true;
        return true;
    }
    owner_.clear();
    release_pending_ = false;
    return true;
}

void InspectorControllerLease::disconnect(std::string_view owner) {
    (void)release(owner);
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
        owner_.clear();
        release_pending_ = false;
    }
}

class InspectorSession::State {
public:
    State(RequestHandler request_handler,
          std::chrono::milliseconds lease_ttl,
          InspectorControllerLease::Clock clock)
        : handler(std::move(request_handler)),
          lease(lease_ttl, std::move(clock)) {}

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

    RequestHandler handler;
    InspectorControllerLease lease;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    std::mutex mutex;
    std::mutex dispatch_mutex;
    std::condition_variable dispatch_cv;
    bool dispatch_accepting = true;
    bool dispatch_active = false;
    std::thread::id dispatch_owner;
    std::size_t dispatch_recursion = 0;
};

InspectorSession::InspectorSession(InspectorSessionInfo info,
                                   InspectorPolicyConfig policy,
                                   RequestHandler handler,
                                   std::chrono::milliseconds lease_ttl,
                                   InspectorControllerLease::Clock clock)
    : info_(std::move(info)),
      policy_(std::move(policy)),
      state_(std::make_shared<State>(
          std::move(handler), lease_ttl, std::move(clock))) {}

InspectorMessage InspectorSession::handle(std::string_view client_id,
                                          const InspectorMessage& request) {
    const auto state = state_;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    bool controller_operation = false;
    {
        std::lock_guard lock(state->mutex);
        if (auto invalid = validate_request_shape(request))
            return std::move(*invalid);
        if (request.method.rfind("Session.", 0) == 0)
            return handle_session_method(client_id, request);

        const auto* method = find_inspector_method(request.method);
        const bool requires_controller =
            method &&
            capability_requires_controller_lease(method->capability);
        if (auto denied =
                policy_.authorize(request, state->lease.owns(client_id))) {
            return std::move(*denied);
        }
        if (requires_controller) {
            if (!state->lease.begin_operation(client_id)) {
                return make_error(request.id,
                                  "A controller lease is required",
                                  "controller_lease_required");
            }
            controller_operation = true;
        }
        main_thread_rpc = state->main_thread_rpc;
    }
    const auto owned_client_id = std::string(client_id);
    const auto finish_controller_operation =
        [state, owned_client_id, controller_operation] {
        if (!controller_operation)
            return;
        std::lock_guard lock(state->mutex);
        state->lease.end_operation(owned_client_id);
    };
    if (!state->handler) {
        finish_controller_operation();
        return make_error(request.id,
                          "No inspector dispatch handler is attached",
                          "dispatch_unavailable");
    }
    const auto owned_request = request;
    auto operation = [state, owned_request] {
        if (!state->begin_dispatch()) {
            return make_error(
                owned_request.id,
                "Inspector dispatch was cancelled during teardown",
                "dispatch_cancelled");
        }
        struct DispatchGuard {
            std::shared_ptr<State> state;
            ~DispatchGuard() { state->end_dispatch(); }
        } guard{state};
        return state->handler(owned_request);
    };

    if (!main_thread_rpc) {
        auto response = [&] {
            try {
                return operation();
            } catch (const std::exception& error) {
                return make_error(
                    request.id,
                    std::string("Inspector dispatch failed: ") + error.what(),
                    "dispatch_failed");
            } catch (...) {
                return make_error(request.id,
                                  "Inspector dispatch failed",
                                  "dispatch_failed");
            }
        }();
        finish_controller_operation();
        return response;
    }
    return main_thread_rpc->call(
        request.id, std::move(operation), finish_controller_operation);
}

void InspectorSession::suspend_dispatches() {
    const auto state = state_;
    {
        std::lock_guard lock(state->dispatch_mutex);
        state->dispatch_accepting = false;
    }
    state->dispatch_cv.notify_all();
}

void InspectorSession::resume_dispatches() {
    const auto state = state_;
    {
        std::lock_guard lock(state->dispatch_mutex);
        state->dispatch_accepting = true;
    }
    state->dispatch_cv.notify_all();
}

void InspectorSession::set_main_thread_rpc(
    std::shared_ptr<InspectorMainThreadRpc> rpc) {
    std::lock_guard lock(state_->mutex);
    state_->main_thread_rpc = std::move(rpc);
}

void InspectorSession::disconnect(std::string_view client_id) {
    std::lock_guard lock(state_->mutex);
    state_->lease.disconnect(client_id);
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
