#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/json_rpc.hpp>
#include <pulp/runtime/memory_message_channel.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

using namespace pulp::runtime;
using namespace std::chrono_literals;

TEST_CASE("JsonRpcPeer request/response over an in-memory channel", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);
    JsonRpcPeer server(*pair.second);

    server.register_method("add", [](std::string_view params) {
        // params is expected to be a JSON array like [2,3]
        // For the Phase 4 smoke test we just echo back a sum of two ints
        // via a trivial regex-free parse.
        int a = 0, b = 0;
        std::sscanf(std::string(params).c_str(), "[%d,%d]", &a, &b);
        return JsonRpcResult::ok(std::to_string(a + b));
    });

    std::atomic<bool> done{false};
    std::string response_payload;
    std::optional<JsonRpcError> err;

    REQUIRE(client.send_request("add", "[2,3]", [&](const JsonRpcResult& r) {
        response_payload = r.result_json;
        err = r.error;
        done.store(true);
    }));

    // Memory channel dispatches synchronously, so the response should
    // already be settled by the time send_request returns. Still do a
    // short wait to be safe under sanitizer runs.
    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(done.load());
    REQUIRE_FALSE(err.has_value());
    REQUIRE(response_payload == "5");
}

TEST_CASE("JsonRpcPeer returns method_not_found for unknown methods", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);
    JsonRpcPeer server(*pair.second);

    std::atomic<bool> done{false};
    std::optional<JsonRpcError> err;

    REQUIRE(client.send_request("nope", "[]", [&](const JsonRpcResult& r) {
        err = r.error;
        done.store(true);
    }));

    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(done.load());
    REQUIRE(err.has_value());
    REQUIRE(err->code == -32601);
}

TEST_CASE("JsonRpcPeer emits parse error for malformed input", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();
    // Raw receiver — no JsonRpcPeer on the right side, so we can observe
    // the error envelope our side emits when fed garbage.
    std::string reply;
    pair.second->on_message([&](const Message& m) {
        reply.assign(m.as_text());
    });
    JsonRpcPeer client(*pair.first);

    // Feed the client peer's channel malformed JSON directly.
    const char* garbage = "{not valid json";
    pair.second->send_text(garbage);

    REQUIRE(reply.find("-32700") != std::string::npos);
    REQUIRE(reply.find("\"id\":null") != std::string::npos);
}

TEST_CASE("JsonRpcPeer fires notification handler and expects no response", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);
    JsonRpcPeer server(*pair.second);

    std::atomic<int> notify_count{0};
    std::string last_params;
    server.on_notification("ping", [&](std::string_view params) {
        last_params = std::string(params);
        notify_count.fetch_add(1);
    });

    REQUIRE(client.notify("ping", "[\"hi\"]"));
    REQUIRE(client.notify("ping", "[\"again\"]"));

    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (notify_count.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(notify_count.load() == 2);
    REQUIRE(last_params == "[\"again\"]");
}
