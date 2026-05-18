#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/json_rpc.hpp>
#include <pulp/runtime/memory_message_channel.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace pulp::runtime;
using namespace std::chrono_literals;

namespace {

bool wait_until(std::function<bool()> predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

class FailingSendChannel final : public MessageChannel {
public:
    bool send(const std::uint8_t*, std::size_t) override { return false; }
    bool send_text(std::string_view) override { return false; }
    void on_message(MessageCallback callback) override { on_message_ = std::move(callback); }
    void on_closed(ChannelClosedCallback) override {}
    void on_error(ChannelErrorCallback) override {}
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }

    void deliver_text(std::string_view text) {
        Message message{
            MessageKind::Text,
            std::vector<std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(text.data()),
                reinterpret_cast<const std::uint8_t*>(text.data() + text.size()))};
        if (on_message_) on_message_(message);
    }

private:
    bool open_ = true;
    MessageCallback on_message_;
};

} // namespace

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
    REQUIRE(wait_until([&] { return done.load(); }));
    REQUIRE_FALSE(err.has_value());
    REQUIRE(response_payload == "5");
}

TEST_CASE("JsonRpcError factories expose spec codes and messages",
          "[json_rpc][coverage][phase3]") {
    const auto parse = JsonRpcError::parse_error();
    const auto invalid = JsonRpcError::invalid_request();
    const auto params = JsonRpcError::invalid_params();
    const auto internal = JsonRpcError::internal_error();

    REQUIRE(parse.code == -32700);
    REQUIRE(parse.message == "Parse error");
    REQUIRE(invalid.code == -32600);
    REQUIRE(invalid.message == "Invalid Request");
    REQUIRE(params.code == -32602);
    REQUIRE(params.message == "Invalid params");
    REQUIRE(internal.code == -32603);
    REQUIRE(internal.message == "Internal error");
    REQUIRE(internal.data_json.empty());
}

TEST_CASE("JsonRpcResult factories separate success payloads from failures",
          "[json_rpc][coverage][phase3]") {
    const auto ok = JsonRpcResult::ok(R"({"ready":true})");
    REQUIRE(ok.result_json == R"({"ready":true})");
    REQUIRE_FALSE(ok.error.has_value());

    const auto failed = JsonRpcResult::fail({-32001, "server busy", R"({"retry":true})"});
    REQUIRE(failed.result_json.empty());
    REQUIRE(failed.error.has_value());
    REQUIRE(failed.error->code == -32001);
    REQUIRE(failed.error->message == "server busy");
    REQUIRE(failed.error->data_json == R"({"retry":true})");
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

    REQUIRE(wait_until([&] { return done.load(); }));
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

    REQUIRE(wait_until([&] { return notify_count.load() == 2; }));
    REQUIRE(last_params == "[\"again\"]");
}

TEST_CASE("JsonRpcPeer delivers empty notification params when omitted",
          "[json_rpc][coverage][phase3]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer server(*pair.second);

    std::string captured = "unset";
    server.on_notification("bare", [&](std::string_view params) {
        captured = std::string(params);
    });

    REQUIRE(pair.first->send_text(
        R"json({"jsonrpc":"2.0","method":"bare"})json"));
    REQUIRE(captured.empty());
}

TEST_CASE("JsonRpcPeer reports scalar parse errors and ignores batches", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string reply;
    pair.second->on_message([&](const Message& message) {
        reply.assign(message.as_text());
    });

    JsonRpcPeer client(*pair.first);

    pair.second->send_text("42");
    REQUIRE(reply.find("-32700") != std::string::npos);
    REQUIRE(reply.find("\"id\":null") != std::string::npos);

    reply.clear();
    pair.second->send_text(R"json([{"jsonrpc":"2.0","method":"ignored"}])json");
    REQUIRE(reply.empty());

    reply.clear();
    pair.second->send_text("   ");
    REQUIRE(reply.empty());
}

TEST_CASE("JsonRpcPeer unregisters methods and notifications", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);
    JsonRpcPeer server(*pair.second);

    server.register_method("gone", [](std::string_view) {
        return JsonRpcResult::ok("1");
    });
    server.register_method("gone", nullptr);

    std::atomic<bool> done{false};
    std::optional<JsonRpcError> err;
    REQUIRE(client.send_request("gone", "[]", [&](const JsonRpcResult& response) {
        err = response.error;
        done.store(true);
    }));

    REQUIRE(wait_until([&] { return done.load(); }));
    REQUIRE(err.has_value());
    REQUIRE(err->code == -32601);

    std::atomic<int> notifications{0};
    server.on_notification("tick", [&](std::string_view) {
        notifications.fetch_add(1);
    });
    REQUIRE(client.notify("tick", "[]"));
    REQUIRE(wait_until([&] { return notifications.load() == 1; }));

    server.on_notification("tick", nullptr);
    REQUIRE(client.notify("tick", "[]"));
    std::this_thread::sleep_for(10ms);
    REQUIRE(notifications.load() == 1);
}

TEST_CASE("JsonRpcPeer preserves string request ids and omits missing params",
          "[json_rpc][coverage][phase3]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string reply;
    pair.first->on_message([&](const Message& message) {
        reply.assign(message.as_text());
    });

    JsonRpcPeer server(*pair.second);
    std::string captured_params = "unset";
    server.register_method("no_params", [&](std::string_view params) {
        captured_params = std::string(params);
        return JsonRpcResult::ok(R"({"accepted":true})");
    });

    REQUIRE(pair.first->send_text(
        R"json({"jsonrpc":"2.0","id":"abc-123","method":"no_params"})json"));

    REQUIRE(reply.find(R"("id":"abc-123")") != std::string::npos);
    REQUIRE(reply.find(R"("accepted":true)") != std::string::npos);
    REQUIRE(captured_params.empty());
}

TEST_CASE("JsonRpcPeer serializes handler errors and exceptions", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);
    JsonRpcPeer server(*pair.second);

    server.register_method("custom_error", [](std::string_view) {
        return JsonRpcResult::fail({1234, "custom failure", R"json({"detail":true})json"});
    });
    server.register_method("throws", [](std::string_view) -> JsonRpcResult {
        throw std::runtime_error("boom");
    });
    server.register_method("empty", [](std::string_view) {
        return JsonRpcResult::ok("");
    });

    std::atomic<int> responses{0};
    std::optional<JsonRpcError> custom_error;
    std::optional<JsonRpcError> thrown_error;
    std::string empty_result = "unset";

    REQUIRE(client.send_request("custom_error", "[]", [&](const JsonRpcResult& response) {
        custom_error = response.error;
        responses.fetch_add(1);
    }));
    REQUIRE(client.send_request("throws", "[]", [&](const JsonRpcResult& response) {
        thrown_error = response.error;
        responses.fetch_add(1);
    }));
    REQUIRE(client.send_request("empty", "[]", [&](const JsonRpcResult& response) {
        empty_result = response.result_json;
        responses.fetch_add(1);
    }));

    REQUIRE(wait_until([&] { return responses.load() == 3; }));
    REQUIRE(custom_error.has_value());
    REQUIRE(custom_error->code == 1234);
    REQUIRE(custom_error->message == "custom failure");
    REQUIRE(custom_error->data_json.find("\"detail\"") != std::string::npos);
    REQUIRE(custom_error->data_json.find("true") != std::string::npos);
    REQUIRE(thrown_error.has_value());
    REQUIRE(thrown_error->code == -32603);
    REQUIRE(empty_result == "null");
}

TEST_CASE("JsonRpcPeer ignores responses that cannot match pending requests", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string outbound_request;
    pair.second->on_message([&](const Message& message) {
        outbound_request.assign(message.as_text());
    });

    JsonRpcPeer client(*pair.first);
    std::atomic<int> callbacks{0};
    std::string result;

    REQUIRE(client.send_request("manual", "[]", [&](const JsonRpcResult& response) {
        result = response.result_json;
        callbacks.fetch_add(1);
    }));
    REQUIRE(outbound_request.find("\"id\":1") != std::string::npos);

    pair.second->send_text(R"json({"jsonrpc":"2.0","id":"1","result":99})json");
    pair.second->send_text(R"json({"jsonrpc":"2.0","result":100})json");
    pair.second->send_text(R"json({"jsonrpc":"2.0","id":99,"result":101})json");
    std::this_thread::sleep_for(10ms);
    REQUIRE(callbacks.load() == 0);

    pair.second->send_text(R"json({"jsonrpc":"2.0","id":1,"result":102})json");
    REQUIRE(wait_until([&] { return callbacks.load() == 1; }));
    REQUIRE(result == "102");
}

TEST_CASE("JsonRpcPeer reports closed and failed sends", "[json_rpc]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);
    pair.second->close();

    bool callback_called = false;
    REQUIRE_FALSE(client.send_request("closed", "[]", [&](const JsonRpcResult&) {
        callback_called = true;
    }));
    REQUIRE_FALSE(client.notify("closed", "[]"));
    REQUIRE_FALSE(callback_called);

    FailingSendChannel channel;
    JsonRpcPeer failing_peer(channel);
    REQUIRE_FALSE(failing_peer.send_request("failed", "[]", [&](const JsonRpcResult&) {
        callback_called = true;
    }));
    REQUIRE_FALSE(failing_peer.notify("failed_notify", "[]"));
    channel.deliver_text(R"json({"jsonrpc":"2.0","id":1,"result":true})json");
    REQUIRE_FALSE(callback_called);
}

TEST_CASE("JsonRpcPeer rejects sends after direct channel close",
          "[json_rpc][coverage][phase3-batch742]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);

    pair.first->close();

    bool callback_called = false;
    REQUIRE_FALSE(client.send_request("closed", "[]", [&](const JsonRpcResult&) {
        callback_called = true;
    }));
    REQUIRE_FALSE(client.notify("closed", "[]"));
    REQUIRE_FALSE(callback_called);
}

TEST_CASE("JsonRpcPeer destructor detaches its message callback",
          "[json_rpc][coverage][phase3-batch742]") {
    auto pair = MemoryMessageChannel::make_pair();
    std::string reply;
    pair.second->on_message([&](const Message& message) {
        reply.assign(message.as_text());
    });

    {
        JsonRpcPeer client(*pair.first);
    }

    REQUIRE(pair.second->send_text("{not-json"));
    REQUIRE(reply.empty());
}

TEST_CASE("JsonRpcPeer omits params for empty request payloads",
          "[json_rpc][coverage][issue-641]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string outbound_request;
    pair.second->on_message([&](const Message& message) {
        outbound_request.assign(message.as_text());
    });

    JsonRpcPeer client(*pair.first);
    std::atomic<int> callbacks{0};
    std::string result;
    REQUIRE(client.send_request("noParams", "", [&](const JsonRpcResult& response) {
        result = response.result_json;
        callbacks.fetch_add(1);
    }));

    REQUIRE(outbound_request.find(R"("method":"noParams")") != std::string::npos);
    REQUIRE(outbound_request.find(R"("params")") == std::string::npos);

    pair.second->send_text(R"json({"jsonrpc":"2.0","id":1,"result":{"ok":true}})json");
    REQUIRE(wait_until([&] { return callbacks.load() == 1; }));
    REQUIRE(result.find(R"("ok")") != std::string::npos);
    REQUIRE(result.find("true") != std::string::npos);
}

TEST_CASE("JsonRpcPeer preserves string request ids in responses",
          "[json_rpc][coverage][issue-641]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string reply;
    pair.first->on_message([&](const Message& message) {
        reply.assign(message.as_text());
    });

    JsonRpcPeer server(*pair.second);
    server.register_method("echo", [](std::string_view params) {
        return JsonRpcResult::ok(std::string(params));
    });

    pair.first->send_text(
        R"json({"jsonrpc":"2.0","id":"abc-123","method":"echo","params":{"value":7}})json");

    REQUIRE(reply.find(R"("id":"abc-123")") != std::string::npos);
    REQUIRE(reply.find(R"("value")") != std::string::npos);
    REQUIRE(reply.find("7") != std::string::npos);
}

TEST_CASE("JsonRpcPeer dispatches incoming error responses with data",
          "[json_rpc][coverage][issue-641]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string outbound_request;
    pair.second->on_message([&](const Message& message) {
        outbound_request.assign(message.as_text());
    });

    JsonRpcPeer client(*pair.first);
    std::atomic<bool> done{false};
    std::optional<JsonRpcError> error;

    REQUIRE(client.send_request("willFail", "[]", [&](const JsonRpcResult& response) {
        error = response.error;
        done.store(true);
    }));
    REQUIRE(outbound_request.find(R"("id":1)") != std::string::npos);

    pair.second->send_text(
        R"json({"jsonrpc":"2.0","id":1,"error":{"code":-32099,"message":"custom","data":{"retry":false}}})json");

    REQUIRE(wait_until([&] { return done.load(); }));
    REQUIRE(error.has_value());
    REQUIRE(error->code == -32099);
    REQUIRE(error->message == "custom");
    REQUIRE(error->data_json.find(R"("retry")") != std::string::npos);
    REQUIRE(error->data_json.find("false") != std::string::npos);
}

TEST_CASE("JsonRpcPeer replies to null id requests and notification gaps",
          "[json_rpc][coverage][phase3]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::vector<std::string> replies;
    pair.first->on_message([&](const Message& message) {
        replies.emplace_back(message.as_text());
    });

    JsonRpcPeer server(*pair.second);
    server.register_method("null_id", [](std::string_view params) {
        REQUIRE(params == "[1]");
        return JsonRpcResult::ok(R"({"ok":true})");
    });

    REQUIRE(pair.first->send_text(
        R"json({"jsonrpc":"2.0","id":null,"method":"null_id","params":[1]})json"));
    REQUIRE(replies.size() == 1);
    REQUIRE(replies.back().find(R"("id":null)") != std::string::npos);
    REQUIRE(replies.back().find(R"("ok")") != std::string::npos);

    REQUIRE(pair.first->send_text(
        R"json({"jsonrpc":"2.0","method":"missing_notification","params":{"ignored":true}})json"));
    REQUIRE(replies.size() == 1);
}

TEST_CASE("JsonRpcPeer unregisters a replacement handler cleanly",
          "[json_rpc][coverage][phase3]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);
    JsonRpcPeer server(*pair.second);

    server.register_method("replaceable", [](std::string_view) {
        return JsonRpcResult::ok("1");
    });
    server.register_method("replaceable", [](std::string_view) {
        return JsonRpcResult::ok("2");
    });

    std::atomic<int> callbacks{0};
    std::string first_result;
    REQUIRE(client.send_request("replaceable", "[]", [&](const JsonRpcResult& response) {
        first_result = response.result_json;
        callbacks.fetch_add(1);
    }));
    REQUIRE(wait_until([&] { return callbacks.load() == 1; }));
    REQUIRE(first_result == "2");

    server.register_method("replaceable", nullptr);
    std::optional<JsonRpcError> error;
    REQUIRE(client.send_request("replaceable", "[]", [&](const JsonRpcResult& response) {
        error = response.error;
        callbacks.fetch_add(1);
    }));
    REQUIRE(wait_until([&] { return callbacks.load() == 2; }));
    REQUIRE(error.has_value());
    REQUIRE(error->code == -32601);
}

TEST_CASE("JsonRpcPeer ignores malformed response payloads without firing callbacks",
          "[json_rpc][coverage][phase3]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string outbound_request;
    pair.second->on_message([&](const Message& message) {
        outbound_request.assign(message.as_text());
    });

    JsonRpcPeer client(*pair.first);
    std::atomic<int> callbacks{0};
    REQUIRE(client.send_request("manual", "", [&](const JsonRpcResult&) {
        callbacks.fetch_add(1);
    }));
    REQUIRE(outbound_request.find(R"("id":1)") != std::string::npos);

    pair.second->send_text("{not-json");
    pair.second->send_text(R"json({"jsonrpc":"2.0","id":1})json");
    std::this_thread::sleep_for(10ms);
    REQUIRE(callbacks.load() == 0);

    pair.second->send_text(R"json({"jsonrpc":"2.0","id":1,"result":true})json");
    REQUIRE(wait_until([&] { return callbacks.load() == 1; }));
}

TEST_CASE("JsonRpcPeer forwards scalar and object params unchanged",
          "[json_rpc][coverage][phase3-large]") {
    auto pair = MemoryMessageChannel::make_pair();
    JsonRpcPeer client(*pair.first);
    JsonRpcPeer server(*pair.second);

    std::vector<std::string> params_seen;
    server.register_method("capture", [&](std::string_view params) {
        params_seen.emplace_back(params);
        return JsonRpcResult::ok(R"({"ok":true})");
    });

    std::atomic<int> callbacks{0};
    REQUIRE(client.send_request("capture", R"({"nested":{"value":7}})",
        [&](const JsonRpcResult& response) {
            REQUIRE_FALSE(response.error.has_value());
            callbacks.fetch_add(1);
        }));
    REQUIRE(client.send_request("capture", R"("scalar")",
        [&](const JsonRpcResult& response) {
            REQUIRE_FALSE(response.error.has_value());
            callbacks.fetch_add(1);
        }));

    REQUIRE(wait_until([&] { return callbacks.load() == 2; }));
    REQUIRE(params_seen.size() == 2);
    REQUIRE(params_seen[0].find(R"("nested")") != std::string::npos);
    REQUIRE(params_seen[0].find("7") != std::string::npos);
    REQUIRE(params_seen[1] == R"("scalar")");
}

TEST_CASE("JsonRpcPeer ignores inert objects and unknown notifications without replies",
          "[json_rpc][coverage][phase3-large]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::vector<std::string> replies;
    pair.first->on_message([&](const Message& message) {
        replies.emplace_back(message.as_text());
    });

    JsonRpcPeer peer(*pair.second);
    REQUIRE(pair.first->send_text(R"json({"jsonrpc":"2.0","id":4})json"));
    REQUIRE(pair.first->send_text(R"json({"jsonrpc":"2.0","params":[1,2,3]})json"));
    REQUIRE(pair.first->send_text(R"json({"jsonrpc":"2.0","method":"unhandled"})json"));

    std::this_thread::sleep_for(10ms);
    REQUIRE(replies.empty());
}

TEST_CASE("JsonRpcPeer destructor releases the channel callback",
          "[json_rpc][coverage][phase3]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string reply;
    pair.first->on_message([&](const Message& message) {
        reply.assign(message.as_text());
    });

    {
        JsonRpcPeer server(*pair.second);
        server.register_method("echo", [](std::string_view params) {
            return JsonRpcResult::ok(std::string(params));
        });

        REQUIRE(pair.first->send_text(
            R"json({"jsonrpc":"2.0","id":1,"method":"echo","params":{"first":true}})json"));
        REQUIRE(reply.find(R"("first")") != std::string::npos);
    }

    reply.clear();
    REQUIRE(pair.first->send_text(
        R"json({"jsonrpc":"2.0","id":2,"method":"echo","params":{"stale":true}})json"));
    REQUIRE(reply.empty());

    std::string raw_message;
    pair.second->on_message([&](const Message& message) {
        raw_message.assign(message.as_text());
    });
    REQUIRE(pair.first->send_text("raw after peer"));
    REQUIRE(raw_message == "raw after peer");
}

TEST_CASE("JsonRpcPeer consumes pending callbacks after the first response",
          "[json_rpc][coverage][phase3]") {
    auto pair = MemoryMessageChannel::make_pair();

    std::string outbound_request;
    pair.second->on_message([&](const Message& message) {
        outbound_request.assign(message.as_text());
    });

    JsonRpcPeer client(*pair.first);
    std::atomic<int> callbacks{0};
    std::string last_result;

    REQUIRE(client.send_request("once", "[]", [&](const JsonRpcResult& response) {
        last_result = response.result_json;
        callbacks.fetch_add(1);
    }));
    REQUIRE(outbound_request.find(R"("id":1)") != std::string::npos);

    pair.second->send_text(R"json({"jsonrpc":"2.0","id":1,"result":"first"})json");
    REQUIRE(wait_until([&] { return callbacks.load() == 1; }));
    REQUIRE(last_result == R"("first")");

    pair.second->send_text(R"json({"jsonrpc":"2.0","id":1,"result":"duplicate"})json");
    std::this_thread::sleep_for(10ms);
    REQUIRE(callbacks.load() == 1);
    REQUIRE(last_result == R"("first")");
}

TEST_CASE("JsonRpcPeer destructor clears the channel message callback",
          "[json_rpc][coverage]") {
    auto pair = MemoryMessageChannel::make_pair();

    {
        JsonRpcPeer peer(*pair.first);
    }

    std::string reply;
    pair.second->on_message([&](const Message& message) {
        reply.assign(message.as_text());
    });

    REQUIRE(pair.second->send_text("{not-json"));
    REQUIRE(reply.empty());
}
