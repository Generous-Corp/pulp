#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/format/remote_view_session.hpp>
#include <pulp/runtime/memory_message_channel.hpp>
#include <pulp/runtime/json_rpc.hpp>
#include <pulp/state/store.hpp>

#include <atomic>
#include <chrono>
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
