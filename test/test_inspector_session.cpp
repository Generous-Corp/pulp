#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/authentication.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>
#include <pulp/inspect/session.hpp>

#include <choc/text/choc_JSON.h>

#include "bounded_event_queue.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;
using pulp::inspect::ControllerLeaseResult;
using pulp::inspect::InspectorAuthChallenge;
using pulp::inspect::InspectorAuthVerifier;
using pulp::inspect::InspectorAccessPolicy;
using pulp::inspect::InspectorCapability;
using pulp::inspect::InspectorControllerLease;
using pulp::inspect::InspectorMainThreadRpc;
using pulp::inspect::InspectorPolicyConfig;
using pulp::inspect::InspectorProfile;
using pulp::inspect::InspectorSession;
using pulp::inspect::InspectorSessionInfo;
using pulp::inspect::make_request;
using pulp::inspect::make_response;
using pulp::inspect::make_event;
using pulp::inspect::make_inspector_auth_proof;
using pulp::inspect::verify_inspector_server_auth_proof;
using pulp::inspect::detail::BoundedEventQueue;
using pulp::inspect::detail::EventQueuePushResult;

static_assert(!std::is_copy_constructible_v<InspectorAuthVerifier>);
static_assert(!std::is_move_constructible_v<InspectorAuthVerifier>);

namespace {

std::vector<InspectorCapability> fixture_capabilities() {
    return {
        InspectorCapability::SessionDescribe,
        InspectorCapability::SessionControl,
        InspectorCapability::StateRead,
        InspectorCapability::StateWrite,
        InspectorCapability::RuntimeEval,
    };
}

InspectorPolicyConfig policy(InspectorProfile profile) {
    InspectorPolicyConfig config;
    config.profile = profile;
    config.available_capabilities = fixture_capabilities();
    return config;
}

} // namespace

TEST_CASE("only telemetry stream events use lossy delivery",
          "[inspect][session][events][backpressure]") {
    CHECK(pulp::inspect::inspector_event_is_lossy("Audio.levels"));
    CHECK(pulp::inspect::inspector_event_is_lossy("Motion.sample"));
    CHECK_FALSE(
        pulp::inspect::inspector_event_is_lossy("State.parameterChanged"));
    CHECK_FALSE(
        pulp::inspect::inspector_event_is_lossy("Console.messageAdded"));
    CHECK_FALSE(
        pulp::inspect::inspector_event_is_lossy("Performance.metrics"));
    CHECK_FALSE(pulp::inspect::inspector_event_is_lossy("Unknown.event"));
}

TEST_CASE("bounded event queue evicts only lossy entries",
          "[inspect][session][events][backpressure]") {
    BoundedEventQueue<int> queue(2);
    REQUIRE(queue.push(1, false) == EventQueuePushResult::Queued);
    REQUIRE(queue.push(2, true) == EventQueuePushResult::Queued);
    CHECK(queue.push(3, false) == EventQueuePushResult::Queued);
    REQUIRE(queue.take_front() == 1);
    REQUIRE(queue.take_front() == 3);

    REQUIRE(queue.push(4, false) == EventQueuePushResult::Queued);
    REQUIRE(queue.push(5, false) == EventQueuePushResult::Queued);
    CHECK(queue.push(6, true) == EventQueuePushResult::DroppedLossy);
    CHECK(queue.push(7, false) == EventQueuePushResult::ReliableOverflow);
    REQUIRE(queue.take_front() == 4);
    REQUIRE(queue.take_front() == 5);
}

TEST_CASE("InspectorAccessPolicy resolves named profiles against host availability",
          "[inspect][session][policy]") {
    InspectorAccessPolicy observe(policy(InspectorProfile::Observe));
    CHECK(observe.is_available(InspectorCapability::StateWrite));
    CHECK(observe.is_granted(InspectorCapability::StateRead));
    CHECK_FALSE(observe.is_granted(InspectorCapability::StateWrite));
    CHECK_FALSE(observe.is_available(InspectorCapability::RuntimeEval));

    auto denied = observe.authorize(
        make_request(1, "State.setParameter"), false);
    REQUIRE(denied.has_value());
    CHECK(denied->is_error);
    CHECK(denied->error_code == "capability_denied");

    auto unavailable = observe.authorize(
        make_request(2, "Capture.screenshot"), false);
    REQUIRE(unavailable.has_value());
    CHECK(unavailable->error_code == "capability_unavailable");

    for (const auto* reserved :
         {"Trace.query", "Trace.explain"}) {
        auto reserved_response =
            observe.authorize(make_request(2, reserved), false);
        REQUIRE(reserved_response.has_value());
        CHECK(reserved_response->error_code == "capability_unavailable");
    }

    auto unknown = observe.authorize(
        make_request(3, "Unknown.method"), false);
    REQUIRE(unknown.has_value());
    CHECK(unknown->error_code == "method_not_found");

    auto event = make_event("DOM.documentUpdated");
    event.id = 4;
    auto invalid_event = observe.authorize(event, false);
    REQUIRE(invalid_event.has_value());
    CHECK(invalid_event->error_code == "invalid_request");

    auto zero_id = observe.authorize(
        make_request(0, "State.getParameters"), false);
    REQUIRE(zero_id.has_value());
    CHECK(zero_id->error_code == "invalid_request");
}

TEST_CASE("custom policy is an exact allow-list and runtime eval is separately gated",
          "[inspect][session][policy][runtime-eval]") {
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Custom;
    config.custom_capabilities = {
        InspectorCapability::SessionControl,
        InspectorCapability::StateRead,
        InspectorCapability::RuntimeEval,
    };
    config.available_capabilities = fixture_capabilities();

    InspectorAccessPolicy gated(config);
    CHECK(gated.is_granted(InspectorCapability::StateRead));
    CHECK_FALSE(gated.is_available(InspectorCapability::RuntimeEval));
    CHECK_FALSE(gated.is_granted(InspectorCapability::RuntimeEval));

    config.runtime_eval_enabled = true;
    InspectorAccessPolicy acknowledged(config);
    CHECK(acknowledged.is_available(InspectorCapability::RuntimeEval));
    CHECK(acknowledged.is_granted(InspectorCapability::RuntimeEval));
    auto lease_required = acknowledged.authorize(
        make_request(1, "Runtime.evaluate"), false);
    REQUIRE(lease_required.has_value());
    CHECK(lease_required->error_code == "controller_lease_required");
    CHECK_FALSE(acknowledged.authorize(
        make_request(2, "Runtime.evaluate"), true).has_value());
}

TEST_CASE("lease-requiring capabilities need effective session control",
          "[inspect][session][policy][controller-prerequisite]") {
    InspectorPolicyConfig config;
    config.profile = InspectorProfile::Custom;
    config.custom_capabilities = {
        InspectorCapability::StateWrite,
        InspectorCapability::TraceControl,
    };
    config.available_capabilities = {
        InspectorCapability::StateWrite,
        InspectorCapability::TraceControl,
        InspectorCapability::SessionControl,
    };

    InspectorAccessPolicy missing_grant(config);
    CHECK(missing_grant.is_available(InspectorCapability::StateWrite));
    CHECK_FALSE(missing_grant.is_granted(InspectorCapability::StateWrite));
    CHECK_FALSE(missing_grant.is_granted(InspectorCapability::TraceControl));

    config.custom_capabilities.push_back(
        InspectorCapability::SessionControl);
    InspectorAccessPolicy executable(config);
    CHECK(executable.is_granted(InspectorCapability::SessionControl));
    CHECK(executable.is_granted(InspectorCapability::StateWrite));
    CHECK(executable.is_granted(InspectorCapability::TraceControl));

    config.available_capabilities.erase(
        std::remove(config.available_capabilities.begin(),
                    config.available_capabilities.end(),
                    InspectorCapability::SessionControl),
        config.available_capabilities.end());
    InspectorAccessPolicy unavailable_controller(config);
    CHECK_FALSE(
        unavailable_controller.is_granted(InspectorCapability::SessionControl));
    CHECK_FALSE(
        unavailable_controller.is_granted(InspectorCapability::StateWrite));
    CHECK_FALSE(
        unavailable_controller.is_granted(InspectorCapability::TraceControl));
}

TEST_CASE("controller lease has one owner, expires, and releases on disconnect",
          "[inspect][session][lease]") {
    auto now = std::chrono::steady_clock::time_point{};
    InspectorControllerLease lease(100ms, [&] { return now; });

    CHECK(lease.acquire("") == ControllerLeaseResult::InvalidOwner);
    CHECK(lease.acquire("alpha") == ControllerLeaseResult::Acquired);
    CHECK(lease.owns("alpha"));
    CHECK(lease.acquire("beta") == ControllerLeaseResult::HeldByOther);
    CHECK(lease.renew("alpha") == ControllerLeaseResult::Renewed);

    now += 101ms;
    CHECK_FALSE(lease.owns("alpha"));
    CHECK(lease.acquire("beta") == ControllerLeaseResult::Acquired);
    lease.disconnect("beta");
    CHECK_FALSE(lease.owner().has_value());
}

TEST_CASE("InspectorSession enforces capability and controller lease before dispatch",
          "[inspect][session][dispatch]") {
    auto config = policy(InspectorProfile::Develop);
    int dispatches = 0;
    InspectorSession session(
        InspectorSessionInfo{"session-1", "instance-1", "fixture"},
        std::move(config),
        [&](const auto& request) {
            ++dispatches;
            return make_response(request.id, R"({"applied":true})");
        });

    auto read = session.handle("reader", make_request(1, "State.getParameters"));
    CHECK_FALSE(read.is_error);
    CHECK(dispatches == 1);

    auto denied =
        session.handle("writer", make_request(2, "State.setParameter"));
    CHECK(denied.is_error);
    CHECK(denied.error_code == "controller_lease_required");
    CHECK(dispatches == 1);

    auto invalid_acquire = session.handle(
        "writer", make_request(0, "Session.acquireController"));
    CHECK(invalid_acquire.is_error);
    CHECK(invalid_acquire.error_code == "invalid_request");

    auto acquired = session.handle(
        "writer", make_request(3, "Session.acquireController"));
    CHECK_FALSE(acquired.is_error);

    auto conflict = session.handle(
        "other", make_request(4, "Session.acquireController"));
    CHECK(conflict.is_error);
    CHECK(conflict.error_code == "controller_lease_conflict");

    auto write =
        session.handle("writer", make_request(5, "State.setParameter"));
    CHECK_FALSE(write.is_error);
    CHECK(dispatches == 2);

    session.disconnect("writer");
    auto reacquired = session.handle(
        "other", make_request(6, "Session.acquireController"));
    CHECK_FALSE(reacquired.is_error);
}

TEST_CASE("InspectorSession invokes domain handlers outside its lease mutex",
          "[inspect][session][dispatch][concurrency]") {
    auto config = policy(InspectorProfile::Develop);
    InspectorSession* session_ptr = nullptr;
    InspectorSession session(
        InspectorSessionInfo{"session-unlocked", "instance-1", "fixture"},
        std::move(config),
        [&](const auto& request) {
            session_ptr->disconnect("reader");
            return make_response(request.id, R"({"unlocked":true})");
        });
    session_ptr = &session;

    const auto response =
        session.handle("reader", make_request(1, "State.getParameters"));
    CHECK_FALSE(response.is_error);
}

TEST_CASE("InspectorSession serializes domain handlers across clients",
          "[inspect][session][dispatch][concurrency]") {
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    bool release = false;
    InspectorSession session(
        {"session-serialized", "instance-1", "fixture"},
        policy(InspectorProfile::Develop),
        [&](const auto& request) {
            std::unique_lock lock(mutex);
            ++entered;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
            return make_response(request.id, R"({"serialized":true})");
        });

    pulp::inspect::InspectorMessage first_response;
    pulp::inspect::InspectorMessage second_response;
    std::thread first([&] {
        first_response =
            session.handle("alpha", make_request(1, "State.getParameters"));
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return entered == 1; }));
    }
    std::thread second([&] {
        second_response =
            session.handle("beta", make_request(2, "State.getParameters"));
    });

    std::this_thread::sleep_for(50ms);
    {
        std::lock_guard lock(mutex);
        CHECK(entered == 1);
        release = true;
    }
    cv.notify_all();
    first.join();
    second.join();

    CHECK_FALSE(first_response.is_error);
    CHECK_FALSE(second_response.is_error);
    CHECK(entered == 2);
}

TEST_CASE("InspectorSession cancels queued dispatch during teardown",
          "[inspect][session][dispatch][concurrency][teardown]") {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    InspectorSession session(
        {"session-cancel-queued", "instance-1", "fixture"},
        policy(InspectorProfile::Develop),
        [&](const auto& request) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
            return make_response(request.id, R"({"completed":true})");
        });

    pulp::inspect::InspectorMessage active_response;
    pulp::inspect::InspectorMessage queued_response;
    std::thread active([&] {
        active_response =
            session.handle("alpha", make_request(1, "State.getParameters"));
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return entered; }));
    }
    std::thread queued([&] {
        queued_response =
            session.handle("beta", make_request(2, "State.getParameters"));
    });

    std::this_thread::sleep_for(20ms);
    session.suspend_dispatches();
    queued.join();
    CHECK(queued_response.is_error);
    CHECK(queued_response.error_code == "dispatch_cancelled");

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    active.join();
    CHECK_FALSE(active_response.is_error);
}

TEST_CASE("InspectorSession rejects recursive dispatch after suspension",
          "[inspect][session][dispatch][reentrant][teardown]") {
    InspectorSession* session_ptr = nullptr;
    pulp::inspect::InspectorMessage nested_response;
    InspectorSession session(
        {"session-cancel-recursive", "instance-1", "fixture"},
        policy(InspectorProfile::Develop),
        [&](const auto& request) {
            session_ptr->suspend_dispatches();
            nested_response = session_ptr->handle(
                "nested", make_request(2, "State.getParameters"));
            return make_response(request.id, R"({"outer":true})");
        });
    session_ptr = &session;

    const auto outer =
        session.handle("outer", make_request(1, "State.getParameters"));
    CHECK_FALSE(outer.is_error);
    CHECK(nested_response.is_error);
    CHECK(nested_response.error_code == "dispatch_cancelled");
}

TEST_CASE("controller lease handoff waits for an admitted mutation",
          "[inspect][session][lease][concurrency]") {
    auto now = std::chrono::steady_clock::time_point{};
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    InspectorSession session(
        {"session-fenced", "instance-1", "fixture"},
        policy(InspectorProfile::Develop),
        [&](const auto& request) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
            return make_response(request.id, R"({"applied":true})");
        },
        100ms,
        [&] { return now; });

    REQUIRE_FALSE(
        session.handle("alpha",
                       make_request(1, "Session.acquireController"))
            .is_error);

    pulp::inspect::InspectorMessage mutation;
    std::thread mutator([&] {
        mutation = session.handle(
            "alpha", make_request(2, "State.setParameter"));
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return entered; }));
    }

    now += 101ms;
    const auto expired_owner_mutation = session.handle(
        "alpha", make_request(3, "State.setParameter"));
    CHECK(expired_owner_mutation.is_error);
    CHECK(expired_owner_mutation.error_code ==
          "controller_lease_required");
    session.disconnect("alpha");
    const auto fenced = session.handle(
        "beta", make_request(4, "Session.acquireController"));
    CHECK(fenced.is_error);
    CHECK(fenced.error_code == "controller_lease_conflict");

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    mutator.join();
    CHECK_FALSE(mutation.is_error);

    const auto acquired = session.handle(
        "beta", make_request(5, "Session.acquireController"));
    CHECK_FALSE(acquired.is_error);
}

TEST_CASE("InspectorSession contains exceptions from domain handlers",
          "[inspect][session][dispatch][exception]") {
    auto config = policy(InspectorProfile::Develop);
    InspectorSession session(
        InspectorSessionInfo{"session-throws", "instance-1", "fixture"},
        std::move(config),
        [](const auto&) -> pulp::inspect::InspectorMessage {
            throw std::runtime_error("fixture failure");
        });

    const auto response =
        session.handle("reader", make_request(1, "State.getParameters"));
    CHECK(response.is_error);
    CHECK(response.error_code == "dispatch_failed");
    CHECK(response.params_json.find("fixture failure") != std::string::npos);
}

TEST_CASE("InspectorSession reports effective authority without dispatching",
          "[inspect][session][capabilities]") {
    auto config = policy(InspectorProfile::Observe);
    InspectorSession session(
        InspectorSessionInfo{"session-7", "instance-9", "plugin.example", "1"},
        std::move(config),
        [](const auto& request) {
            return make_response(request.id, "{}");
        });

    const auto response = session.handle(
        "observer", make_request(1, "Session.getCapabilities"));
    REQUIRE_FALSE(response.is_error);
    const auto result = choc::json::parse(response.params_json);
    CHECK(result["sessionId"].getString() == "session-7");
    CHECK(result["instanceId"].getString() == "instance-9");
    CHECK(result["profile"].getString() == "observe");

    auto denied = session.handle(
        "observer", make_request(2, "Session.acquireController"));
    CHECK(denied.is_error);
    CHECK(denied.error_code == "capability_denied");
}

TEST_CASE("inspector authentication binds a one-shot proof to session and protocol",
          "[inspect][session][authentication]") {
    std::vector<std::uint8_t> token(32, 0x5a);
    InspectorAuthChallenge challenge{
        "pulp-inspector-hmac-sha256-v1",
        std::string(64, '1'),
        "session-1",
        "instance-1",
        "publication-1",
        "1",
    };
    const auto proof = make_inspector_auth_proof(token, challenge);
    REQUIRE(proof.has_value());
    auto replacement = challenge;
    replacement.publication_id = "publication-2";
    const auto replacement_proof =
        make_inspector_auth_proof(token, replacement);
    REQUIRE(replacement_proof.has_value());
    CHECK(*replacement_proof != *proof);

    InspectorAuthVerifier verifier(token, challenge);
    const auto server_proof = verifier.authenticate(*proof);
    REQUIRE(server_proof.has_value());
    CHECK(*server_proof != *proof);
    CHECK(verify_inspector_server_auth_proof(
        token, challenge, *proof, *server_proof));
    CHECK(verifier.consumed());
    CHECK_FALSE(verifier.verify(*proof));
    CHECK_FALSE(verify_inspector_server_auth_proof(
        token, challenge, *proof, *proof));

    auto wrong_session = challenge;
    wrong_session.session_id = "session-2";
    InspectorAuthVerifier wrong_session_verifier(token, wrong_session);
    CHECK_FALSE(wrong_session_verifier.verify(*proof));

    auto wrong_instance = challenge;
    wrong_instance.instance_id = "instance-2";
    InspectorAuthVerifier wrong_instance_verifier(token, wrong_instance);
    CHECK_FALSE(wrong_instance_verifier.verify(*proof));

    auto wrong_protocol = challenge;
    wrong_protocol.protocol_version = "2";
    InspectorAuthVerifier wrong_protocol_verifier(token, wrong_protocol);
    CHECK_FALSE(wrong_protocol_verifier.verify(*proof));
}

TEST_CASE("main-thread RPC responds only after the operation is applied",
          "[inspect][session][main-thread-rpc]") {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::function<void()>> queued;
    InspectorMainThreadRpc rpc(
        {1s, 4},
        [&](auto task) {
            std::lock_guard lock(mutex);
            queued.push_back(std::move(task));
            cv.notify_all();
            return true;
        },
        [] { return false; });

    bool applied = false;
    pulp::inspect::InspectorMessage response;
    std::thread caller([&] {
        response = rpc.call(7, [&] {
            applied = true;
            return make_response(7, R"({"applied":true})");
        });
    });

    std::function<void()> task;
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return !queued.empty(); }));
        CHECK_FALSE(applied);
        task = std::move(queued.front());
    }
    task();
    caller.join();

    CHECK(applied);
    CHECK_FALSE(response.is_error);
    CHECK(response.id == 7);
}

TEST_CASE("timed-out queued main-thread work cannot mutate later",
          "[inspect][session][main-thread-rpc][timeout]") {
    std::function<void()> queued;
    InspectorMainThreadRpc rpc(
        {5ms, 1},
        [&](auto task) {
            queued = std::move(task);
            return true;
        },
        [] { return false; });

    int mutations = 0;
    const auto response = rpc.call(8, [&] {
        ++mutations;
        return make_response(8, "{}");
    });
    REQUIRE(response.is_error);
    CHECK(response.error_code == "main_thread_timeout");
    CHECK(mutations == 0);

    REQUIRE(static_cast<bool>(queued));
    queued();
    CHECK(mutations == 0);
}

TEST_CASE("timed-out started main-thread work returns while retaining its slot",
          "[inspect][session][main-thread-rpc][timeout][lifetime]") {
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void()> queued;
    bool operation_started = false;
    bool release_operation = false;
    InspectorMainThreadRpc rpc(
        {5ms, 1},
        [&](auto task) {
            queued = std::move(task);
            cv.notify_all();
            return true;
        },
        [] { return false; });

    pulp::inspect::InspectorMessage response;
    std::atomic<bool> caller_returned{false};
    std::thread caller([&] {
        response = rpc.call(80, [&] {
            std::unique_lock lock(mutex);
            operation_started = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_operation; });
            return make_response(80, R"({"applied":true})");
        });
        caller_returned.store(true, std::memory_order_release);
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] {
            return static_cast<bool>(queued);
        }));
    }

    std::thread executor([&] { queued(); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] {
            return operation_started;
        }));
    }
    const auto caller_deadline =
        std::chrono::steady_clock::now() + 1s;
    while (!caller_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < caller_deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(caller_returned.load(std::memory_order_acquire));
    REQUIRE(response.is_error);
    CHECK(response.error_code == "main_thread_timeout");
    CHECK(response.error_data_json.find(
              R"("mayHaveApplied":true)") != std::string::npos);
    const auto full = rpc.call(
        81, [] { return make_response(81, "{}"); });
    CHECK(full.error_code == "dispatch_queue_full");

    {
        std::lock_guard lock(mutex);
        release_operation = true;
    }
    cv.notify_all();
    executor.join();
    caller.join();

    CHECK(caller_returned.load(std::memory_order_acquire));
}

TEST_CASE("timed-out started work does not block RPC destruction",
          "[inspect][session][main-thread-rpc][timeout][teardown]") {
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void()> queued;
    bool operation_started = false;
    bool release_operation = false;
    auto rpc = std::make_unique<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{5ms, 1},
        [&](auto task) {
            queued = std::move(task);
            cv.notify_all();
            return true;
        },
        [] { return false; });

    pulp::inspect::InspectorMessage response;
    std::atomic<bool> caller_returned{false};
    std::thread caller([&] {
        response = rpc->call(82, [&] {
            std::unique_lock lock(mutex);
            operation_started = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_operation; });
            return make_response(82, R"({"late":true})");
        });
        caller_returned.store(true, std::memory_order_release);
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] {
            return static_cast<bool>(queued);
        }));
    }
    std::thread executor([&] { queued(); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] {
            return operation_started;
        }));
    }
    const auto caller_deadline =
        std::chrono::steady_clock::now() + 1s;
    while (!caller_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < caller_deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(caller_returned.load(std::memory_order_acquire));
    caller.join();
    REQUIRE(response.error_code == "main_thread_timeout");

    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        rpc.reset();
        destroyed.store(true, std::memory_order_release);
    });
    const auto destruction_deadline =
        std::chrono::steady_clock::now() + 1s;
    while (!destroyed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < destruction_deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(destroyed.load(std::memory_order_acquire));

    {
        std::lock_guard lock(mutex);
        release_operation = true;
    }
    cv.notify_all();
    executor.join();
    destroyer.join();
}

TEST_CASE("cancelling main-thread RPC wakes queued callers and rejects new work",
          "[inspect][session][main-thread-rpc][teardown]") {
    std::mutex mutex;
    std::condition_variable cv;
    bool posted = false;
    InspectorMainThreadRpc rpc(
        {1s, 1},
        [&](auto) {
            std::lock_guard lock(mutex);
            posted = true;
            cv.notify_all();
            return true;
        },
        [] { return false; });

    pulp::inspect::InspectorMessage response;
    std::thread caller([&] {
        response = rpc.call(9, [] { return make_response(9, "{}"); });
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return posted; }));
    }
    rpc.cancel();
    caller.join();

    CHECK(response.is_error);
    CHECK(response.error_code == "dispatch_cancelled");
    CHECK(response.id == 9);
    CHECK_FALSE(rpc.active());
    const auto rejected =
        rpc.call(10, [] { return make_response(10, "{}"); });
    CHECK(rejected.error_code == "dispatch_cancelled");
}

TEST_CASE("cancelling main-thread RPC does not wait for running direct work",
          "[inspect][session][main-thread-rpc][teardown]") {
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool release = false;
    InspectorMainThreadRpc rpc(
        {1s, 1},
        [](auto) { return false; },
        [] { return true; });

    pulp::inspect::InspectorMessage response;
    std::thread caller([&] {
        response = rpc.call(11, [&] {
            std::unique_lock lock(mutex);
            started = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
            return make_response(11, "{}");
        });
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, 1s, [&] { return started; }));
    }

    std::atomic<bool> cancel_returned{false};
    std::thread canceller([&] {
        rpc.cancel();
        cancel_returned.store(true, std::memory_order_release);
    });
    const auto cancel_deadline =
        std::chrono::steady_clock::now() + 1s;
    while (!cancel_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < cancel_deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(cancel_returned.load(std::memory_order_acquire));
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    caller.join();
    canceller.join();

    CHECK(cancel_returned.load(std::memory_order_acquire));
    CHECK_FALSE(response.is_error);
    CHECK_FALSE(rpc.active());
}

TEST_CASE("reentrant cancellation preserves a started RPC result",
          "[inspect][session][main-thread-rpc][teardown][reentrant]") {
    InspectorMainThreadRpc* rpc_ptr = nullptr;
    InspectorMainThreadRpc rpc(
        {1s, 1},
        [](auto task) {
            task();
            return true;
        },
        [] { return false; });
    rpc_ptr = &rpc;

    const auto response = rpc.call(12, [&] {
        rpc_ptr->cancel();
        return make_response(12, R"({"applied":true})");
    });
    CHECK_FALSE(response.is_error);
    CHECK(response.id == 12);
    CHECK(response.params_json.find("applied") != std::string::npos);
    CHECK_FALSE(rpc.active());
}
