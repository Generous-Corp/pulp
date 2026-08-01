#include "inspector_client_test_support.hpp"
#include "inspector_async_request_executor.hpp"

using pulp::inspect::InspectorServerShutdownFence;
using pulp::inspect::make_request;

TEST_CASE("reentrant RPC destruction waits for its causal concurrent dispatch",
          "[inspect][server][asynchronous][main-thread][reentrant]"
          "[concurrency][teardown]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Develop;
    policy.runtime_eval_enabled = true;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
        InspectorCapability::SessionControl,
        InspectorCapability::RuntimeEval,
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::function<void()> queued_rpc_task;
    std::thread::id concurrent_dispatch_thread;
    std::thread::id rpc_executor_thread;
    bool concurrent_rpc_returned = false;
    bool release_concurrent_dispatch = false;
    std::atomic<bool> rpc_destruction_returned{false};
    std::atomic<bool> rpc_task_returned{false};
    std::atomic<bool> concurrent_dispatch_returned{false};
    auto rpc = std::make_shared<InspectorMainThreadRpc>(
        InspectorMainThreadRpc::Config{std::chrono::seconds(2), 1},
        [&](auto task) {
            {
                std::lock_guard lock(mutex);
                queued_rpc_task = std::move(task);
            }
            cv.notify_all();
            return true;
        },
        [] { return false; });

    auto server = std::make_unique<InspectorServer>();
    InspectorSession session(
        {"session-causal-concurrent-stop", "instance", "plugin", "1"},
        policy,
        [](const auto& request) { return make_response(request.id, "{}"); });
    session.set_concurrent_request_handler(
        "Runtime.evaluate",
        [&](const auto&, const auto& request) {
            {
                std::lock_guard lock(mutex);
                concurrent_dispatch_thread = std::this_thread::get_id();
            }
            auto response = rpc->call(request.id, [&] {
                server.reset();
                rpc_destruction_returned.store(true, std::memory_order_release);
                return make_response(request.id, R"({"evaluated":true})");
            });
            {
                std::unique_lock lock(mutex);
                concurrent_rpc_returned = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release_concurrent_dispatch; });
            }
            concurrent_dispatch_returned.store(true, std::memory_order_release);
            return response;
        });

    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.main_thread_rpc = rpc;
    config.asynchronous_methods = {"Runtime.evaluate"};
    config.max_asynchronous_requests = 1;
    REQUIRE(server->start_authenticated(std::move(config)));
    const auto shutdown_fence = server->shutdown_fence();
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    REQUIRE_FALSE(client.request("Session.acquireController").is_error);

    InspectorMessage response;
    std::thread requester([&] {
        response = client.request(
            "Runtime.evaluate", R"({"code":"1 + 1"})",
            std::chrono::seconds(2));
    });
    std::function<void()> rpc_task;
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return static_cast<bool>(queued_rpc_task);
        }));
        rpc_task = std::move(queued_rpc_task);
    }

    std::thread rpc_executor([&, task = std::move(rpc_task)]() mutable {
        {
            std::lock_guard lock(mutex);
            rpc_executor_thread = std::this_thread::get_id();
        }
        task();
        rpc_task_returned.store(true, std::memory_order_release);
    });
    bool rpc_response_reached_concurrent_dispatch = false;
    {
        std::unique_lock lock(mutex);
        rpc_response_reached_concurrent_dispatch = cv.wait_for(
            lock, std::chrono::seconds(1), [&] {
                return concurrent_rpc_returned;
            });
    }
    REQUIRE(rpc_response_reached_concurrent_dispatch);

    // Before causal deferral, task() invokes the deferred stop immediately
    // after the operation returns. That stop waits for the concurrent handler,
    // while the concurrent handler is deliberately held above. Causal deferral
    // lets task() return before that handler is released.
    CHECK(rpc_destruction_returned.load(std::memory_order_acquire));
    const auto task_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!rpc_task_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < task_deadline) {
        std::this_thread::yield();
    }
    const bool task_returned_before_dispatch_release =
        rpc_task_returned.load(std::memory_order_acquire);
    CHECK(task_returned_before_dispatch_release);
    {
        std::lock_guard lock(mutex);
        CHECK(concurrent_dispatch_thread != std::thread::id{});
        CHECK(rpc_executor_thread != std::thread::id{});
        CHECK(concurrent_dispatch_thread != rpc_executor_thread);
        release_concurrent_dispatch = true;
    }
    cv.notify_all();
    rpc_executor.join();
    requester.join();

    CHECK(concurrent_dispatch_returned.load(std::memory_order_acquire));
    CHECK_FALSE(server);
    REQUIRE(shutdown_fence.wait_for(std::chrono::seconds(1)));
    CHECK(shutdown_fence.ready());
    if (response.is_error)
        CHECK(response.error_code == "connection_closed");
}

TEST_CASE("asynchronous executor atomically defers cleanup after dequeue",
          "[inspect][server][asynchronous][disconnect][admission]") {
    using pulp::inspect::detail::InspectorAsyncRequestExecutor;
    using pulp::inspect::detail::InspectorConnectedClient;

    std::mutex mutex;
    std::condition_variable cv;
    bool iteration_acquired = false;
    bool release_iteration = false;
    std::atomic<int> execute_calls{0};
    std::atomic<int> cleanup_calls{0};
    auto client = std::make_shared<InspectorConnectedClient>();
    client->client_id = "client-admission-race";
    auto executor = InspectorAsyncRequestExecutor::create({
        .acquire_iteration_lifetime = [&]() -> std::shared_ptr<void> {
            std::unique_lock lock(mutex);
            iteration_acquired = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_iteration; });
            return std::make_shared<int>(1);
        },
        .execute = [&](const auto&) {
            execute_calls.fetch_add(1, std::memory_order_relaxed);
        },
    });
    executor->configure(1);
    const bool submitted = executor->submit({
        client, make_request(1, "Inspector.getInfo", "{}"), 1});
    if (!submitted)
        executor->stop(false);
    REQUIRE(submitted);
    bool acquired = false;
    {
        std::unique_lock lock(mutex);
        acquired = cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return iteration_acquired;
        });
    }
    if (!acquired) {
        {
            std::lock_guard lock(mutex);
            release_iteration = true;
        }
        cv.notify_all();
        executor->stop(false);
    }
    REQUIRE(acquired);

    REQUIRE(executor->cancel_queued_for_client(client, [&] {
        cleanup_calls.fetch_add(1, std::memory_order_relaxed);
    }));
    CHECK(execute_calls.load(std::memory_order_relaxed) == 0);
    CHECK(cleanup_calls.load(std::memory_order_relaxed) == 0);
    {
        std::lock_guard lock(mutex);
        release_iteration = true;
    }
    cv.notify_all();
    const auto cleanup_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (cleanup_calls.load(std::memory_order_relaxed) != 1 &&
           std::chrono::steady_clock::now() < cleanup_deadline) {
        std::this_thread::yield();
    }
    CHECK(execute_calls.load(std::memory_order_relaxed) == 1);
    CHECK(cleanup_calls.load(std::memory_order_relaxed) == 1);
    executor->stop(false);
}

TEST_CASE("asynchronous server dispatch rejects work beyond its configured cap",
          "[inspect][server][asynchronous][resource-limit]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    std::mutex mutex;
    std::condition_variable cv;
    bool handler_entered = false;
    bool release_handler = false;
    std::atomic<int> handler_calls{0};
    InspectorSession session(
        {"session-async-cap", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            handler_calls.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock lock(mutex);
            handler_entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_handler; });
            return make_response(request.id, R"({"completed":true})");
        });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.asynchronous_methods = {"Inspector.getInfo"};
    config.max_asynchronous_requests = 1;
    REQUIRE(start_test_inspector_server(server, std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));

    InspectorMessage first_response;
    std::thread first_request([&] {
        first_response = client.request(
            "Inspector.getInfo", "{}", std::chrono::seconds(2));
    });
    bool entered = false;
    {
        std::unique_lock lock(mutex);
        entered = cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return handler_entered;
        });
    }
    if (!entered) {
        {
            std::lock_guard lock(mutex);
            release_handler = true;
        }
        cv.notify_all();
        first_request.join();
    }
    REQUIRE(entered);

    const auto overflow = client.request(
        "Inspector.getInfo", "{}", std::chrono::seconds(1));
    CHECK(overflow.is_error);
    CHECK(overflow.error_code == "dispatch_queue_full");
    CHECK(handler_calls.load(std::memory_order_relaxed) == 1);

    {
        std::lock_guard lock(mutex);
        release_handler = true;
    }
    cv.notify_all();
    first_request.join();
    CHECK_FALSE(first_response.is_error);
    CHECK(handler_calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("asynchronous server dispatch suppresses zero-id responses",
          "[inspect][server][asynchronous][notification]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    InspectorSession session(
        {"session-async-notification", "instance", "plugin", "1"},
        policy,
        [](const auto& request) {
            return make_response(request.id, R"({"completed":true})");
        });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.asynchronous_methods = {"Inspector.getInfo"};
    config.max_asynchronous_requests = 2;
    REQUIRE(start_test_inspector_server(server, std::move(config)));

    Socket socket;
    REQUIRE(socket.create(SocketType::TCP));
    REQUIRE(socket.set_read_timeout(std::chrono::seconds(1)));
    REQUIRE(socket.connect(
        "127.0.0.1", static_cast<std::uint16_t>(server.port())));
    REQUIRE(authenticate_raw(socket, *token));
    REQUIRE(send_frame(
        socket, pulp::inspect::encode_message(
                    make_request(0, "Inspector.getInfo", "{}"))));
    REQUIRE(send_frame(
        socket, pulp::inspect::encode_message(
                    make_request(2, "Inspector.getInfo", "{}"))));

    const auto response_frame = receive_frame(socket);
    REQUIRE(response_frame.has_value());
    InspectorMessage response;
    REQUIRE(pulp::inspect::decode_message(*response_frame, response));
    CHECK(response.id == 2);
    CHECK_FALSE(response.is_error);
}

TEST_CASE("asynchronous queue rejection suppresses zero-id responses",
          "[inspect][server][asynchronous][notification][resource-limit]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    std::mutex mutex;
    std::condition_variable cv;
    bool handler_entered = false;
    bool release_handler = false;
    InspectorSession session(
        {"session-async-notification-cap", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            std::unique_lock lock(mutex);
            handler_entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_handler; });
            return make_response(request.id, R"({"completed":true})");
        });
    InspectorServer server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.asynchronous_methods = {"Inspector.getInfo"};
    config.max_asynchronous_requests = 1;
    REQUIRE(start_test_inspector_server(server, std::move(config)));

    Socket socket;
    REQUIRE(socket.create(SocketType::TCP));
    REQUIRE(socket.set_read_timeout(std::chrono::seconds(1)));
    REQUIRE(socket.connect(
        "127.0.0.1", static_cast<std::uint16_t>(server.port())));
    REQUIRE(authenticate_raw(socket, *token));
    REQUIRE(send_frame(
        socket, pulp::inspect::encode_message(
                    make_request(2, "Inspector.getInfo", "{}"))));
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return handler_entered;
        }));
    }
    REQUIRE(send_frame(
        socket, pulp::inspect::encode_message(
                    make_request(0, "Inspector.getInfo", "{}"))));
    REQUIRE(socket.set_read_timeout(std::chrono::milliseconds(250)));
    CHECK_FALSE(receive_frame(socket).has_value());
    {
        std::lock_guard lock(mutex);
        release_handler = true;
    }
    cv.notify_all();

    REQUIRE(socket.set_read_timeout(std::chrono::seconds(1)));
    const auto response_frame = receive_frame(socket);
    REQUIRE(response_frame.has_value());
    InspectorMessage response;
    REQUIRE(pulp::inspect::decode_message(*response_frame, response));
    CHECK(response.id == 2);
    CHECK_FALSE(response.is_error);
}

TEST_CASE("disconnect cancels queued asynchronous work before client cleanup",
          "[inspect][server][asynchronous][disconnect][lifecycle]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    std::mutex mutex;
    std::condition_variable cv;
    bool handler_entered = false;
    bool release_handler = false;
    bool stop_reentrantly = false;
    InspectorServer* reentrant_server = nullptr;
    std::atomic<int> handler_calls{0};
    std::atomic<int> cleanup_calls{0};
    InspectorSession session(
        {"session-async-disconnect", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            handler_calls.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock lock(mutex);
            handler_entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_handler; });
            const bool should_stop = stop_reentrantly;
            lock.unlock();
            if (should_stop)
                reentrant_server->stop();
            return make_response(request.id, R"({"completed":true})");
        });
    session.set_client_disconnect_handler([&](std::string_view) {
        cleanup_calls.fetch_add(1, std::memory_order_relaxed);
    });
    InspectorServer server;
    reentrant_server = &server;
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.asynchronous_methods = {"Inspector.getInfo"};
    config.max_asynchronous_requests = 2;
    REQUIRE(start_test_inspector_server(server, std::move(config)));

    Socket socket;
    REQUIRE(socket.create(SocketType::TCP));
    REQUIRE(socket.set_read_timeout(std::chrono::seconds(1)));
    REQUIRE(socket.connect(
        "127.0.0.1", static_cast<std::uint16_t>(server.port())));
    REQUIRE(authenticate_raw(socket, *token));
    REQUIRE(send_frame(
        socket, pulp::inspect::encode_message(
                    make_request(2, "Inspector.getInfo", "{}"))));
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return handler_entered;
        }));
    }

    // The synchronous response proves the connection loop consumed the queued
    // request that precedes it; the sole async worker remains blocked above.
    REQUIRE(send_frame(
        socket, pulp::inspect::encode_message(
                    make_request(3, "Inspector.getInfo", "{}"))));
    REQUIRE(send_frame(
        socket, pulp::inspect::encode_message(
                    make_request(4, "Session.getCapabilities", "{}"))));
    const auto synchronous_frame = receive_frame(socket);
    REQUIRE(synchronous_frame.has_value());
    InspectorMessage synchronous_response;
    REQUIRE(pulp::inspect::decode_message(
        *synchronous_frame, synchronous_response));
    REQUIRE(synchronous_response.id == 4);
    REQUIRE_FALSE(synchronous_response.is_error);

    socket.close();
    const auto disconnect_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (server.client_count() != 0 &&
           std::chrono::steady_clock::now() < disconnect_deadline) {
        std::this_thread::yield();
    }
    REQUIRE(server.client_count() == 0);
    CHECK(cleanup_calls.load(std::memory_order_relaxed) == 0);

    const auto release_and_verify = [&](bool reentrant_stop = false) {
        {
            std::lock_guard lock(mutex);
            stop_reentrantly = reentrant_stop;
            release_handler = true;
        }
        cv.notify_all();
        const auto cleanup_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (cleanup_calls.load(std::memory_order_relaxed) != 1 &&
               std::chrono::steady_clock::now() < cleanup_deadline) {
            std::this_thread::yield();
        }
        {
            std::unique_lock lock(mutex);
            CHECK_FALSE(cv.wait_for(
                lock, std::chrono::milliseconds(100), [&] {
                    return handler_calls.load(std::memory_order_relaxed) != 1;
                }));
        }
        CHECK(handler_calls.load(std::memory_order_relaxed) == 1);
        CHECK(cleanup_calls.load(std::memory_order_relaxed) == 1);
    };

    SECTION("normal completion delivers deferred cleanup") {
        release_and_verify();
    }

    SECTION("reentrant stop cannot retarget deferred cleanup") {
        release_and_verify(true);
    }

    SECTION("concurrent stop cannot lose deferred cleanup") {
        std::atomic<bool> stop_started{false};
        std::atomic<bool> stop_returned{false};
        std::thread stopper([&] {
            stop_started.store(true, std::memory_order_release);
            server.stop();
            stop_returned.store(true, std::memory_order_release);
        });
        const auto stop_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!stop_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < stop_deadline) {
            std::this_thread::yield();
        }
        REQUIRE(stop_started.load(std::memory_order_acquire));
        CHECK_FALSE(stop_returned.load(std::memory_order_acquire));
        release_and_verify();
        stopper.join();
        CHECK(stop_returned.load(std::memory_order_acquire));
    }
}

TEST_CASE("shutdown fence retains an active asynchronous server worker",
          "[inspect][server][asynchronous][shutdown-fence][owner-lifetime]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    std::mutex mutex;
    std::condition_variable cv;
    bool handler_entered = false;
    bool release_handler = false;
    InspectorSession session(
        {"session-async-fence", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            std::unique_lock lock(mutex);
            handler_entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_handler; });
            return make_response(request.id, R"({"completed":true})");
        });
    auto server = std::make_unique<InspectorServer>();
    const auto shutdown_fence = server->shutdown_fence();
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.asynchronous_methods = {"Inspector.getInfo"};
    config.max_asynchronous_requests = 1;
    REQUIRE(start_test_inspector_server(*server, std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));

    InspectorMessage response;
    std::thread requester([&] {
        response = client.request(
            "Inspector.getInfo", "{}", std::chrono::seconds(2));
    });
    bool entered = false;
    {
        std::unique_lock lock(mutex);
        entered = cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return handler_entered;
        });
    }
    if (!entered) {
        {
            std::lock_guard lock(mutex);
            release_handler = true;
        }
        cv.notify_all();
        requester.join();
    }
    REQUIRE(entered);

    std::atomic<bool> destroy_started{false};
    std::atomic<bool> destroy_returned{false};
    std::thread destroyer([&] {
        destroy_started.store(true, std::memory_order_release);
        server.reset();
        destroy_returned.store(true, std::memory_order_release);
    });
    const auto destroy_start_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!destroy_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < destroy_start_deadline) {
        std::this_thread::yield();
    }
    const auto destruction_observed =
        destroy_started.load(std::memory_order_acquire);
    if (!destruction_observed) {
        {
            std::lock_guard lock(mutex);
            release_handler = true;
        }
        cv.notify_all();
        requester.join();
        destroyer.join();
    }
    REQUIRE(destruction_observed);
    CHECK_FALSE(shutdown_fence.wait_for(std::chrono::milliseconds(50)));
    CHECK_FALSE(shutdown_fence.ready());
    CHECK_FALSE(destroy_returned.load(std::memory_order_acquire));

    {
        std::lock_guard lock(mutex);
        release_handler = true;
    }
    cv.notify_all();
    requester.join();
    destroyer.join();
    CHECK(destroy_returned.load(std::memory_order_acquire));
    CHECK_FALSE(server);
    REQUIRE(shutdown_fence.wait_for(std::chrono::seconds(1)));
    CHECK(shutdown_fence.ready());
    if (response.is_error)
        CHECK(response.error_code == "connection_closed");
}

TEST_CASE("asynchronous worker can destroy its server without escaping the shutdown fence",
          "[inspect][server][asynchronous][shutdown-fence][owner-lifetime][reentrant]") {
    TemporaryDirectory temporary;
    InspectorDiscoveryPublisher publisher(temporary.path);
    InspectorDiscoveryReader reader(temporary.path);
    InspectorPolicyConfig policy;
    policy.profile = InspectorProfile::Observe;
    policy.available_capabilities = {
        InspectorCapability::SessionDescribe,
    };
    std::unique_ptr<InspectorServer> server;
    InspectorServerShutdownFence shutdown_fence;
    std::atomic<bool> wrapper_destroyed{false};
    std::atomic<bool> worker_self_wait_refused{false};
    InspectorSession session(
        {"session-async-self-destroy", "instance", "plugin", "1"},
        policy,
        [&](const auto& request) {
            server.reset();
            wrapper_destroyed.store(true, std::memory_order_release);
            worker_self_wait_refused.store(
                !shutdown_fence.wait_for(std::chrono::milliseconds(1)),
                std::memory_order_release);
            return make_response(request.id, R"({"completed":true})");
        });
    server = std::make_unique<InspectorServer>();
    shutdown_fence = server->shutdown_fence();
    const auto token = generate_inspector_secret();
    REQUIRE(token.has_value());
    InspectorDiscoveryRecord record;
    record.session_id = session.info().session_id;
    record.instance_id = session.info().instance_id;
    record.plugin_id = session.info().plugin_id;
    InspectorServerConfig config{
        &session, &publisher, record, *token};
    config.asynchronous_methods = {"Inspector.getInfo"};
    REQUIRE(start_test_inspector_server(*server, std::move(config)));
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));

    const auto response = client.request(
        "Inspector.getInfo", "{}", std::chrono::seconds(2));
    CHECK(wrapper_destroyed.load(std::memory_order_acquire));
    CHECK(worker_self_wait_refused.load(std::memory_order_acquire));
    CHECK_FALSE(server);
    REQUIRE(shutdown_fence.wait_for(std::chrono::seconds(1)));
    CHECK(shutdown_fence.ready());
    if (response.is_error)
        CHECK(response.error_code == "connection_closed");
}
