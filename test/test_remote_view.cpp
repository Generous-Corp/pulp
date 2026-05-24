#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/format/remote_view_session.hpp>
#include <pulp/runtime/memory_message_channel.hpp>
#include <pulp/runtime/json_rpc.hpp>
#include <pulp/state/store.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pulp;

namespace {

// Minimal processor with one parameter to exercise the remote-view
// protocol. Primary view comes from AutoUi default.
class StubProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {"RemoteViewStub", "Acme", "com.acme.rv", "1.0.0",
                format::PluginCategory::Effect};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {}
    format::ViewSize view_size() const override {
        return {640, 480, 320, 240, 1280, 960};
    }
};

class EscapedMetadataProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Remote \"Quoted\"\nStub",
            .manufacturer = "Acme",
            .bundle_id = "com.acme.rv.escaped",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
        };
    }

    void define_parameters(state::StateStore& s) override {
        s.add_parameter({
            .id = 7,
            .name = "Gain \"A\"\nLine",
            .unit = "dB",
            .range = {-60.0f, 12.0f, 0.0f},
        });
    }

    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {}
};

class MultiParamProcessor : public StubProcessor {
public:
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 12.0f, 0.0f},
        });
        s.add_parameter({
            .id = 2,
            .name = "Mix",
            .unit = "%",
            .range = {0.0f, 100.0f, 50.0f},
        });
    }
};

class LifecycleProcessor : public StubProcessor {
public:
    std::unique_ptr<view::View> create_view() override {
        return std::make_unique<view::View>();
    }

    void on_view_opened(view::View& view) override {
        last_view = &view;
        ++opened;
    }

    void on_view_closed(view::View& view) override {
        last_view = &view;
        ++closed;
    }

    void on_view_resized(view::View& view, uint32_t w, uint32_t h) override {
        last_view = &view;
        last_width = w;
        last_height = h;
        ++resized;
    }

    view::View* last_view = nullptr;
    uint32_t last_width = 0;
    uint32_t last_height = 0;
    int opened = 0;
    int closed = 0;
    int resized = 0;
};

template <typename Pred>
bool wait_for(Pred pred, int ms = 500) {
    for (int i = 0; i < ms && !pred(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace

TEST_CASE("RemoteViewSession - handshake sends view.hello + view.metadata", "[remote_view]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();

    // Remote side: record every notification the bridge emits.
    std::vector<std::string> notifications;
    runtime::JsonRpcPeer remote_peer(*remote_chan);
    remote_peer.on_notification("view.hello",
        [&](std::string_view) { notifications.push_back("view.hello"); });
    std::string metadata_payload;
    remote_peer.on_notification("view.metadata",
        [&](std::string_view params) {
            notifications.push_back("view.metadata");
            metadata_payload = std::string{params};
        });

    auto* session = bridge.attach_remote_channel(std::move(host_chan), "loopback");
    REQUIRE(session != nullptr);

    REQUIRE(wait_for([&]{ return notifications.size() >= 2; }));
    REQUIRE(notifications[0] == "view.hello");
    REQUIRE(notifications[1] == "view.metadata");
    INFO("metadata payload: " << metadata_payload);
    REQUIRE(metadata_payload.find("RemoteViewStub") != std::string::npos);
    REQUIRE(metadata_payload.find("640") != std::string::npos);

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - metadata escapes names as valid JSON",
          "[remote_view][coverage][phase3]") {
    EscapedMetadataProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    std::string metadata_payload;
    remote_peer.on_notification("view.metadata",
        [&](std::string_view params) {
            metadata_payload = std::string{params};
        });

    auto* session = bridge.attach_remote_channel(std::move(host_chan), "loopback");
    REQUIRE(session != nullptr);

    REQUIRE(wait_for([&]{ return !metadata_payload.empty(); }));
    auto root = choc::json::parse(metadata_payload);
    REQUIRE(root.isObject());
    REQUIRE(root["title"].getString() == std::string("Remote \"Quoted\"\nStub"));
    REQUIRE(root["params"].isArray());
    REQUIRE(root["params"].size() == 1);
    REQUIRE(root["params"][0]["id"].getInt64() == 7);
    REQUIRE(root["params"][0]["name"].getString()
            == std::string("Gain \"A\"\nLine"));

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - metadata includes exact size hints and multiple params",
          "[remote_view][coverage][phase3]") {
    MultiParamProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    std::string metadata_payload;
    remote_peer.on_notification("view.metadata",
        [&](std::string_view params) {
            metadata_payload = std::string{params};
        });

    auto* session = bridge.attach_remote_channel(std::move(host_chan), "loopback");
    REQUIRE(session != nullptr);
    const bool session_label_ok =
        session->role() == format::ViewRole::Remote && session->url() == "loopback";
    REQUIRE(session_label_ok);

    REQUIRE(wait_for([&]{ return !metadata_payload.empty(); }));
    auto root = choc::json::parse(metadata_payload);
    const bool size_hints_ok =
        root["size_hints"]["preferred_width"].getInt64() == 640
        && root["size_hints"]["max_width"].getInt64() == 1280;
    REQUIRE(size_hints_ok);
    REQUIRE(root["params"].size() == 2);
    const bool params_ok =
        root["params"][0]["id"].getInt64() == 1
        && root["params"][1]["id"].getInt64() == 2
        && root["params"][1]["name"].getString() == std::string("Mix");
    REQUIRE(params_ok);

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - remote sets param, host StateStore reflects it", "[remote_view]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    // Drive a parameter change from the remote.
    REQUIRE(remote_peer.notify("view.param_set",
        R"({"id":1,"normalized":0.75})"));

    REQUIRE(wait_for([&]{ return store.get_normalized(1) > 0.7f; }));
    REQUIRE(store.get_normalized(1) == Catch::Approx(0.75f));

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - remote param_set accepts numeric JSON variants",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    REQUIRE(remote_peer.notify("view.param_set", R"({"id":1.0,"normalized":1})"));
    REQUIRE(wait_for([&]{ return store.get_normalized(1) > 0.99f; }));
    REQUIRE(store.get_normalized(1) == Catch::Approx(1.0f));

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - host set_parameter notifies remote", "[remote_view]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    std::atomic<int> param_changed_count{0};
    std::string last_payload;
    remote_peer.on_notification("view.param_changed",
        [&](std::string_view params) {
            last_payload = std::string{params};
            ++param_changed_count;
        });

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    REQUIRE(session->set_parameter(1, 0.42f));
    REQUIRE(wait_for([&]{ return param_changed_count.load() >= 1; }));
    INFO("last_payload=" << last_payload);
    REQUIRE(last_payload.find("\"id\"") != std::string::npos);
    REQUIRE(last_payload.find("1") != std::string::npos);
    REQUIRE(store.get_normalized(1) == Catch::Approx(0.42f));

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - closed peer channel disables outbound operations",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    remote_chan->close();
    REQUIRE(wait_for([&]{ return !session->is_open(); }));
    REQUIRE_FALSE(session->set_parameter(1, 0.5f));
    REQUIRE_FALSE(session->get_parameter(1).has_value());
    REQUIRE_FALSE(session->send_input(R"({"kind":"click"})"));

    REQUIRE(bridge.detach_remote(session));
}

TEST_CASE("RemoteViewSession - send_input forwards payload to remote",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    std::atomic<int> input_count{0};
    std::string last_payload;
    remote_peer.on_notification("view.input",
        [&](std::string_view params) {
            last_payload = std::string{params};
            ++input_count;
        });

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    REQUIRE(session->send_input(R"({"kind":"pointer_down","x":12,"y":34})"));
    REQUIRE(wait_for([&]{ return input_count.load() == 1; }));
    INFO("last_payload=" << last_payload);
    REQUIRE(last_payload.find("\"pointer_down\"") != std::string::npos);
    REQUIRE(last_payload.find("\"x\"") != std::string::npos);
    REQUIRE(last_payload.find("12") != std::string::npos);
    REQUIRE(last_payload.find("\"y\"") != std::string::npos);
    REQUIRE(last_payload.find("34") != std::string::npos);

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - remote view.param_get serves state values and errors",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);
    store.set_normalized(1, 0.375f);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    std::string result_json;
    std::atomic<bool> got_valid{false};
    remote_peer.send_request("view.param_get", R"({"id":1})",
        [&](const runtime::JsonRpcResult& result) {
            result_json = result.result_json;
            got_valid = true;
        });

    REQUIRE(wait_for([&]{ return got_valid.load(); }));
    auto result = choc::json::parse(result_json);
    REQUIRE(result["normalized"].getFloat64() == Catch::Approx(0.375));

    std::atomic<bool> got_invalid{false};
    std::optional<runtime::JsonRpcError> invalid_error;
    remote_peer.send_request("view.param_get", R"({"id":"bad"})",
        [&](const runtime::JsonRpcResult& result) {
            invalid_error = result.error;
            got_invalid = true;
        });
    REQUIRE(wait_for([&]{ return got_invalid.load(); }));
    REQUIRE(invalid_error.has_value());
    REQUIRE(invalid_error->code == -32602);

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - host get_parameter reads remote result", "[remote_view]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    std::string request_payload;
    remote_peer.register_method("view.param_get",
        [&](std::string_view params) -> runtime::JsonRpcResult {
            request_payload = std::string{params};
            return runtime::JsonRpcResult::ok(R"({"normalized":0.625})");
        });

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    auto value = session->get_parameter(1);
    REQUIRE(value.has_value());
    REQUIRE(*value == Catch::Approx(0.625f));
    REQUIRE(request_payload.find("\"id\"") != std::string::npos);
    REQUIRE(request_payload.find("1") != std::string::npos);

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - host get_parameter ignores invalid remote results",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    remote_peer.register_method("view.param_get",
        [](std::string_view) -> runtime::JsonRpcResult {
            return runtime::JsonRpcResult::ok(R"({"value":0.5})");
        });

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    REQUIRE_FALSE(session->get_parameter(1).has_value());

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - host get_parameter ignores remote errors",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    remote_peer.register_method("view.param_get",
        [](std::string_view) -> runtime::JsonRpcResult {
            return runtime::JsonRpcResult::fail(runtime::JsonRpcError::invalid_params());
        });

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    REQUIRE_FALSE(session->get_parameter(1).has_value());

    bridge.detach_remote(session);
}

TEST_CASE("RemoteViewSession - malformed remote param sets are ignored", "[remote_view]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);

    store.set_normalized(1, 0.25f);
    REQUIRE(store.get_normalized(1) == Catch::Approx(0.25f));

    REQUIRE(remote_peer.notify("view.param_set", "not-json"));
    REQUIRE(remote_peer.notify("view.param_set", R"({"id":1})"));
    REQUIRE(remote_peer.notify("view.param_set", R"({"id":"1","normalized":0.9})"));
    REQUIRE(remote_peer.notify("view.param_set", R"({"id":1,"normalized":"0.9"})"));
    REQUIRE(store.get_normalized(1) == Catch::Approx(0.25f));

    bridge.detach_remote(session);
}

TEST_CASE("ViewBridge - attach_remote_channel rejects null channel",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    REQUIRE(bridge.attach_remote_channel(nullptr, "null") == nullptr);
    REQUIRE(bridge.last_error().find("null channel") != std::string::npos);
}

TEST_CASE("ViewBridge - attach_remote_channel reports handshake send failure",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    remote_chan->close();

    auto* session = bridge.attach_remote_channel(std::move(host_chan), "closed-peer");
    REQUIRE(session == nullptr);
    REQUIRE(bridge.last_error().find("view.hello") != std::string::npos);
}

TEST_CASE("RemoteViewSession - remote close shuts guards", "[remote_view]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);
    REQUIRE(session->is_open());

    REQUIRE(remote_peer.notify("view.close", R"({"reason":"remote_detach"})"));
    REQUIRE(wait_for([&]{ return !session->is_open(); }));

    REQUIRE_FALSE(session->set_parameter(1, 0.5f));
    REQUIRE_FALSE(session->get_parameter(1).has_value());
    REQUIRE_FALSE(session->send_input(R"({"kind":"click"})"));

    session->close();
    REQUIRE_FALSE(session->is_open());
    REQUIRE(bridge.detach_remote(session));
}

TEST_CASE("RemoteViewSession - close detaches and closes underlying channel", "[remote_view]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    std::atomic<bool> got_close{false};
    remote_peer.on_notification("view.close",
        [&](std::string_view) { got_close = true; });

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);
    REQUIRE(session->is_open());

    REQUIRE(bridge.detach_remote(session));
    REQUIRE(wait_for([&]{ return got_close.load(); }));
}

TEST_CASE("ViewBridge - custom view lifecycle and secondary view indexing",
          "[remote_view][coverage][phase3]") {
    LifecycleProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store, {.role = format::ViewRole::Inspector});
    REQUIRE(bridge.role() == format::ViewRole::Inspector);
    REQUIRE(bridge.release_view() == nullptr);

    std::string error = "unchanged";
    REQUIRE(bridge.open(&error));
    REQUIRE(bridge.open());
    REQUIRE(bridge.is_open());
    REQUIRE_FALSE(bridge.uses_script_ui());
    const bool primary_view_ok =
        bridge.width() == 640
        && bridge.size_hints().max_width == 1280
        && bridge.view_count() == 1
        && bridge.view_at(0) == bridge.view()
        && bridge.role_at(0) == format::ViewRole::Inspector;
    REQUIRE(primary_view_ok);

    bridge.resize(700, 500);
    REQUIRE(p.resized == 0);
    REQUIRE(bridge.width() == 700);

    bridge.notify_attached();
    bridge.notify_attached();
    REQUIRE(p.opened == 1);

    bridge.resize(720, 540);
    REQUIRE(p.resized == 1);
    REQUIRE(p.last_width == 720);

    REQUIRE(bridge.attach_secondary_view(nullptr, format::ViewRole::Remote) == nullptr);
    auto secondary = std::make_unique<view::View>();
    auto* secondary_raw = secondary.get();
    REQUIRE(bridge.attach_secondary_view(std::move(secondary), format::ViewRole::Remote)
            == secondary_raw);
    REQUIRE(bridge.view_count() == 2);
    REQUIRE(bridge.view_at(1) == secondary_raw);
    REQUIRE(bridge.role_at(1) == format::ViewRole::Remote);
    REQUIRE(bridge.role_at(2) == format::ViewRole::Editor);
    REQUIRE_FALSE(bridge.detach_secondary_view(bridge.view()));
    REQUIRE(bridge.detach_secondary_view(secondary_raw));
    REQUIRE_FALSE(bridge.detach_secondary_view(secondary_raw));

    auto released = bridge.release_view();
    REQUIRE(released != nullptr);
    REQUIRE(bridge.release_view() == nullptr);

    bridge.close();
    REQUIRE(p.closed == 1);
    REQUIRE_FALSE(bridge.is_open());
    bridge.close();
    REQUIRE(p.closed == 1);
}

TEST_CASE("ViewBridge - detach_remote is false for null and stale sessions",
          "[remote_view][coverage][phase3]") {
    StubProcessor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::ViewBridge bridge(p, store);
    REQUIRE(bridge.open());
    REQUIRE_FALSE(bridge.detach_remote(nullptr));

    auto [host_chan, remote_chan] = runtime::MemoryMessageChannel::make_pair();
    runtime::JsonRpcPeer remote_peer(*remote_chan);

    auto* session = bridge.attach_remote_channel(std::move(host_chan));
    REQUIRE(session != nullptr);
    REQUIRE(bridge.detach_remote(session));
    REQUIRE_FALSE(bridge.detach_remote(session));
}
