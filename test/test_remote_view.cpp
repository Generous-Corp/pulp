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
#include <thread>

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

    bridge.detach_remote(session);
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
