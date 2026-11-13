// test_pulp_vellum_param_bridge.cpp: the Pulp ParamPort / VisualPort to
// Vellum ParamModel / ValueFeed bridge, headless, against a real StateStore and
// StateStoreParamPort with HostParameterEditBridge installed the way a format
// adapter installs it. The host log records what a DAW would hear.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/host_parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/vellum/mapping.hpp>
#include <pulp/vellum/param_port_model.hpp>
#include <pulp/vellum/visual_port_feed.hpp>

#include <vellum/value/param_model.hpp>
#include <vellum/value/value_feed.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using pulp::format::HostParameterEditBridge;
using pulp::format::StateStoreParamPort;
using pulp::state::ParamID;
using pulp::state::StateStore;
using pulp::vellum::ParamPortModel;
using vellum::value::ParamHandle;

namespace {

constexpr ParamID kGain = 1; // linear 0..1
constexpr ParamID kFreq = 3; // skewed 20..20000 Hz
constexpr ParamID kMode = 4; // five-value enum
constexpr ParamID kDrag = 5; // linear, written by the editor

pulp::state::ParamInfo make_info(ParamID id, const char* name, pulp::state::ParamRange range) {
    pulp::state::ParamInfo info;
    info.id = id;
    info.name = name;
    info.range = range;
    return info;
}

struct HostLog {
    std::vector<std::string> entries;

    HostParameterEditBridge::Callbacks callbacks() {
        return {
            [this](ParamID id) { entries.push_back("B" + std::to_string(id)); },
            [this](ParamID, float, float) { ++values; },
            [this](ParamID id) { entries.push_back("E" + std::to_string(id)); },
        };
    }
    int values = 0;
    int count(const std::string& entry) const {
        int n = 0;
        for (const auto& e : entries)
            n += e == entry ? 1 : 0;
        return n;
    }
};

struct Fixture {
    StateStore store;
    HostLog host;
    std::unique_ptr<HostParameterEditBridge> bridge;
    std::unique_ptr<StateStoreParamPort> port;
    std::unique_ptr<ParamPortModel> model;

    Fixture() {
        store.add_parameter(make_info(kGain, "Gain", {0.0f, 1.0f, 0.5f}));
        auto freq = make_info(kFreq, "Freq", {20.0f, 20000.0f, 1000.0f, 0.0f, 0.3f});
        freq.unit = "Hz";
        store.add_parameter(freq);
        auto mode = make_info(kMode, "Mode", {0.0f, 4.0f, 0.0f, 1.0f});
        mode.kind = pulp::state::ParamKind::Enum;
        mode.value_labels = {"A", "B", "C", "D", "E"};
        store.add_parameter(mode);
        store.add_parameter(make_info(kDrag, "Drag", {0.0f, 1.0f, 0.0f}));
        bridge = std::make_unique<HostParameterEditBridge>(store, host.callbacks());
        port = std::make_unique<StateStoreParamPort>(store);
        ParamPortModel::Options options;
        options.modulation = [this](ParamID id) { return store.displayed_modulation(id); };
        model = std::make_unique<ParamPortModel>(*port, std::move(options));
    }

    void tick() {
        store.pump_listeners();
        store.reconcile_restore_listeners();
    }
    const pulp::state::ParamInfo& info(ParamID id) const {
        return *port->info(id);
    }
};

} // namespace

TEST_CASE("host automation reaches the model as the normalized value on skewed and stepped ranges",
          "[pulp-vellum][round-trip]") {
    Fixture f;
    const auto freq = f.model->resolve("Freq");
    const auto mode = f.model->resolve("Mode");
    std::vector<std::pair<ParamHandle, double>> heard;
    auto sub = f.model->subscribe([&](ParamHandle h, double n) { heard.emplace_back(h, n); });

    std::thread audio([&] {
        f.store.set_value_rt(kFreq, 2500.0f);
        f.store.set_value_rt(kMode, 3.0f);
    });
    audio.join();
    f.tick();

    REQUIRE(heard.size() == 2);
    const double want_freq = f.info(kFreq).range.normalize(2500.0f);
    REQUIRE(heard[0].first == freq);
    REQUIRE(std::abs(heard[0].second - want_freq) < 1e-6);
    REQUIRE(std::abs(f.model->normalized(freq) - want_freq) < 1e-6);
    // The skewed range is not linear: a linear mapping would read 0.124.
    REQUIRE(want_freq > 0.3);
    REQUIRE(heard[1].first == mode);
    REQUIRE(heard[1].second == 0.75);
    REQUIRE(f.model->plain(mode, heard[1].second) == 3.0);
}

TEST_CASE("an editor gesture reaches the host as one begin, the values and one end",
          "[pulp-vellum][round-trip][gesture]") {
    Fixture f;
    const auto freq = f.model->resolve("Freq");

    f.model->begin(freq);
    f.model->set_normalized(freq, 0.37);
    f.model->end(freq);

    REQUIRE(std::abs(f.store.get_value(kFreq) - f.info(kFreq).range.denormalize(0.37f)) < 1e-3f);
    REQUIRE(f.host.count("B3") == 1);
    REQUIRE(f.host.count("E3") == 1);
    REQUIRE(f.host.values >= 1);
    REQUIRE(f.store.open_gesture_count() == 0);
}

TEST_CASE("a long drag is one host gesture", "[pulp-vellum][gesture]") {
    Fixture f;
    const auto drag = f.model->resolve("Drag");

    f.model->begin(drag);
    f.model->begin(drag); // nested: still one
    for (int i = 1; i <= 50; ++i)
        f.model->set_normalized(drag, i / 50.0);
    f.model->end(drag);
    REQUIRE(f.host.count("E5") == 0); // the outer gesture is still open
    f.model->end(drag);
    f.model->end(drag); // unbalanced: ignored

    REQUIRE(f.host.count("B5") == 1);
    REQUIRE(f.host.count("E5") == 1);
    REQUIRE(f.store.get_value(kDrag) == 1.0f);
}

TEST_CASE("a set outside a gesture is a one-shot the port brackets", "[pulp-vellum][gesture]") {
    Fixture f;
    f.model->set_normalized(f.model->resolve("Gain"), 0.25);
    REQUIRE(f.host.count("B1") == 1);
    REQUIRE(f.host.count("E1") == 1);
    REQUIRE(f.store.get_value(kGain) == 0.25f);
}

TEST_CASE("two editors dragging one parameter open the host gesture once",
          "[pulp-vellum][gesture]") {
    Fixture f;
    StateStoreParamPort second_port(f.store);
    ParamPortModel second(second_port);
    const auto a = f.model->resolve("Drag");
    const auto b = second.resolve("Drag");

    f.model->begin(a);
    second.begin(b);
    f.model->end(a);
    REQUIRE(f.host.count("E5") == 0); // the other editor still holds it
    second.end(b);

    REQUIRE(f.host.count("B5") == 1);
    REQUIRE(f.host.count("E5") == 1);
}

TEST_CASE("destroying a model mid-drag ends its gesture", "[pulp-vellum][gesture]") {
    Fixture f;
    f.model->begin(f.model->resolve("Drag"));
    REQUIRE(f.host.count("B5") == 1);
    f.model.reset();
    REQUIRE(f.host.count("E5") == 1);
    REQUIRE(f.store.open_gesture_count() == 0);
}

TEST_CASE("a state restore reaches the model after the tick pump and is not an edit",
          "[pulp-vellum][restore]") {
    Fixture f;
    const auto freq = f.model->resolve("Freq");
    f.store.set_value(kFreq, 5000.0f);
    const auto saved = f.store.serialize();
    f.store.set_value(kFreq, 100.0f);
    f.tick();

    std::vector<double> heard;
    auto sub = f.model->subscribe([&](ParamHandle h, double n) {
        if (h == freq)
            heard.push_back(n);
    });
    f.host.entries.clear();
    f.host.values = 0;

    bool restored = false;
    std::thread host_thread([&] { restored = f.store.deserialize(saved); });
    host_thread.join();
    REQUIRE(restored);
    REQUIRE(heard.empty()); // restore is silent until the UI tick
    f.tick();

    const double want = f.info(kFreq).range.normalize(5000.0f);
    REQUIRE(!heard.empty());
    REQUIRE(std::abs(heard.back() - want) < 1e-6);
    REQUIRE(std::abs(f.model->normalized(freq) - want) < 1e-6);
    REQUIRE(f.host.entries.empty());
    REQUIRE(f.host.values == 0);
}

TEST_CASE("a restore during a drag leaves the gesture open", "[pulp-vellum][restore][gesture]") {
    Fixture f;
    const auto drag = f.model->resolve("Drag");
    f.store.set_value(kDrag, 0.9f);
    const auto saved = f.store.serialize();

    f.model->begin(drag);
    f.model->set_normalized(drag, 0.2);
    REQUIRE(f.store.deserialize(saved));
    f.tick();
    REQUIRE(std::abs(f.model->normalized(drag) - 0.9) < 1e-6);
    f.model->set_normalized(drag, 0.4);
    f.model->end(drag);

    REQUIRE(f.host.count("B5") == 1);
    REQUIRE(f.host.count("E5") == 1);
}

TEST_CASE("a subscriber that writes back what it heard does not create a host edit",
          "[pulp-vellum][echo]") {
    Fixture f;
    const auto gain = f.model->resolve("Gain");
    int heard = 0;
    auto sub = f.model->subscribe([&](ParamHandle h, double n) {
        ++heard;
        f.model->set_normalized(h, n); // a widget whose set_value raises on_change
    });

    f.store.set_value_rt(kGain, 0.8f);
    const int values_before_tick = f.host.values; // the automation write itself
    f.tick();

    REQUIRE(heard == 1);
    REQUIRE(f.host.entries.empty());
    REQUIRE(f.host.values == values_before_tick);
    REQUIRE(std::abs(f.model->normalized(gain) - 0.8) < 1e-6);
}

TEST_CASE("audio-thread writes never leave the UI behind the store", "[pulp-vellum][hammer]") {
    Fixture f;
    const auto freq = f.model->resolve("Freq");
    const auto drag = f.model->resolve("Drag");
    double last_heard = -1.0;
    auto sub = f.model->subscribe([&](ParamHandle h, double n) {
        if (h == freq)
            last_heard = n;
    });

    std::atomic<bool> done{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::thread audio([&] {
        for (int i = 0; i < 200000; ++i)
            f.store.set_value_rt(kFreq, 20.0f + static_cast<float>(i % 19980));
        done.store(true, std::memory_order_release);
    });
    int steps = 0;
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        f.model->begin(drag);
        f.model->set_normalized(drag, (steps++ % 100) / 100.0);
        f.model->end(drag);
        f.tick();
    }
    audio.join();
    REQUIRE(done.load());
    f.tick();

    const double want = f.model->normalized(freq);
    REQUIRE(std::abs(want - f.info(kFreq).range.normalize(f.store.get_value(kFreq))) < 1e-6);
    REQUIRE(std::abs(last_heard - want) < 1e-6);
    REQUIRE(f.host.count("B5") == f.host.count("E5"));
    REQUIRE(f.store.open_gesture_count() == 0);
}

TEST_CASE("discrete parameters agree with Vellum's value-index arithmetic",
          "[pulp-vellum][discrete]") {
    for (int count = 2; count <= 128; ++count) {
        pulp::state::ParamInfo info;
        info.kind = pulp::state::ParamKind::Enum;
        info.range = {0.0f, static_cast<float>(count - 1), 0.0f, 1.0f};
        REQUIRE(pulp::vellum::value_count(info) == count);
        for (int i = 0; i < count; ++i) {
            const double n = pulp::vellum::to_normalized(info, static_cast<float>(i));
            REQUIRE(vellum::value::normalized_to_value_index(n, count) == i);
            REQUIRE(pulp::vellum::to_plain(info, vellum::value::value_index_to_normalized(
                                                     i, count)) == static_cast<float>(i));
        }
    }
}

TEST_CASE("the mapping helpers need no model and round-trip the ends", "[pulp-vellum][mapping]") {
    const auto info = make_info(kFreq, "Freq", {20.0f, 20000.0f, 1000.0f, 0.0f, 0.3f});
    REQUIRE(pulp::vellum::to_normalized(info, 20.0f) == 0.0);
    REQUIRE(pulp::vellum::to_normalized(info, 20000.0f) == 1.0);
    REQUIRE(pulp::vellum::to_plain(info, 0.0) == 20.0f);
    REQUIRE(pulp::vellum::to_plain(info, 1.0) == 20000.0f);
    REQUIRE(pulp::vellum::to_plain(info, 7.0) == 20000.0f); // clamps
    for (double n : {0.1, 0.5, 0.9}) {
        const float plain = pulp::vellum::to_plain(info, n);
        REQUIRE(std::abs(pulp::vellum::to_normalized(info, plain) - n) < 1e-4);
    }
}

TEST_CASE("keys resolve by name and by decimal id, and duplicates are refused",
          "[pulp-vellum][keys]") {
    Fixture f;
    REQUIRE(f.model->resolve("Gain").valid());
    REQUIRE(f.model->resolve("1") == f.model->resolve("Gain"));
    REQUIRE(!f.model->resolve("gain").valid());
    REQUIRE(!f.model->resolve("").valid());
    REQUIRE(!f.model->resolve("99").valid());
    REQUIRE(f.model->param_id(f.model->resolve("Mode")) == kMode);

    StateStore store;
    store.add_parameter(make_info(1, "Same", {}));
    store.add_parameter(make_info(2, "Same", {}));
    StateStoreParamPort port(store);
    REQUIRE_THROWS_AS(ParamPortModel(port), std::invalid_argument);

    // A custom resolver is an allowlist: a parameter it does not name is out.
    ParamPortModel::Options options;
    options.resolver = [](std::string_view key) -> std::optional<ParamID> {
        return key == "level" ? std::optional<ParamID>(2) : std::nullopt;
    };
    ParamPortModel gated(port, std::move(options)); // duplicate names are fine here
    REQUIRE(gated.resolve("level").valid());
    REQUIRE(!gated.resolve("Same").valid());
    REQUIRE(gated.param_id(gated.resolve("level")) == 2);
}

TEST_CASE("info, text and parsing go through the parameter's own description",
          "[pulp-vellum][text]") {
    Fixture f;
    const auto freq = f.model->resolve("Freq");
    const auto mode = f.model->resolve("Mode");
    const auto info = f.model->info(freq);
    REQUIRE(info.name == "Freq");
    REQUIRE(info.unit == "Hz");
    REQUIRE(info.value_count == 0);
    REQUIRE(std::abs(info.default_normalized - f.info(kFreq).range.normalize(1000.0f)) < 1e-6);
    REQUIRE(f.model->info(mode).value_count == 5);
    REQUIRE(f.model->info({}).name.empty());

    REQUIRE(f.model->display_text(mode, 0.5) == "C");
    REQUIRE(f.model->parse_text(mode, "D") == std::optional<double>(0.75));
    REQUIRE(!f.model->parse_text(mode, "Z").has_value());
    REQUIRE(f.model->display_text(freq, 1.0) == "20000.00 Hz");
    for (double n : {0.0, 0.3, 0.8, 1.0}) {
        const auto back = f.model->parse_text(freq, f.model->display_text(freq, n));
        REQUIRE(back.has_value());
        REQUIRE(std::abs(*back - n) < 1e-3);
    }
    REQUIRE(f.model->parse_text(freq, "20000 Hz") == std::optional<double>(1.0));
    REQUIRE(f.model->parse_text(freq, "999999") == std::optional<double>(1.0));
    for (const char* bad : {"", "  ", "abc", "10 dB", "nan", "inf", "1 2"})
        REQUIRE(!f.model->parse_text(freq, bad).has_value());
    REQUIRE(!f.model->parse_text({}, "1").has_value());
}

TEST_CASE("modulation readout follows what the processor publishes", "[pulp-vellum][modulation]") {
    Fixture f;
    const auto freq = f.model->resolve("Freq");
    REQUIRE(!f.model->modulated_normalized(freq).has_value());

    std::thread audio([&] { f.store.set_display_modulation(kFreq, 5000.0f); });
    audio.join();
    const auto shown = f.model->modulated_normalized(freq);
    REQUIRE(shown.has_value());
    REQUIRE(std::abs(*shown - f.info(kFreq).range.normalize(5000.0f)) < 1e-6);
    // The base value is untouched and differs from what is shown.
    REQUIRE(std::abs(f.model->normalized(freq) - *shown) > 0.1);

    f.store.clear_display_modulation(kFreq);
    REQUIRE(!f.model->modulated_normalized(freq).has_value());

    // Without a reader nothing is modulated.
    ParamPortModel plain_model(*f.port);
    f.store.set_display_modulation(kFreq, 5000.0f);
    REQUIRE(!plain_model.modulated_normalized(plain_model.resolve("Freq")).has_value());
}

TEST_CASE("a disconnected port makes the model inert and safe", "[pulp-vellum][disconnect]") {
    Fixture f;
    const auto gain = f.model->resolve("Gain");
    f.port->disconnect();

    f.model->begin(gain);
    f.model->set_normalized(gain, 0.9);
    f.model->end(gain);
    f.tick();

    REQUIRE(f.store.get_value(kGain) == 0.5f);
    REQUIRE(f.host.entries.empty());
    REQUIRE(f.store.open_gesture_count() == 0);
    f.model.reset(); // destroying it after the port is gone from the store is safe
}

TEST_CASE("subscriptions are removable and may outlive the model", "[pulp-vellum][subscribe]") {
    Fixture f;
    int heard = 0;
    auto sub = f.model->subscribe([&](ParamHandle, double) { ++heard; });
    f.store.set_value_rt(kGain, 0.7f);
    f.tick();
    REQUIRE(heard == 1);
    sub.reset();
    f.store.set_value_rt(kGain, 0.2f);
    f.tick();
    REQUIRE(heard == 1);

    auto late = f.model->subscribe([&](ParamHandle, double) { ++heard; });
    f.model.reset();
    late.reset(); // the model is gone; cancelling is a no-op
    REQUIRE(heard == 1);
}

namespace {

// A VisualPort with one channel of each shape and recognizable payloads.
class FakeVisualPort final : public pulp::format::VisualPort {
  public:
    std::size_t channel_count() const noexcept override {
        return 4;
    }
    pulp::format::VisualChannelInfo channel(std::size_t i) const noexcept override {
        static constexpr pulp::format::VisualChannelShape shapes[] = {
            pulp::format::VisualChannelShape::scalar, pulp::format::VisualChannelShape::meter,
            pulp::format::VisualChannelShape::vector, pulp::format::VisualChannelShape::events};
        static constexpr const char* names[] = {"gr", "level", "scope", "hits"};
        return {names[i], "dB", shapes[i], -1.0f};
    }
    bool read_scalar(std::size_t i, float& v) noexcept override {
        if (i != 0)
            return false;
        v = 0.5f;
        return true;
    }
    std::size_t read_meter(std::size_t i, std::span<float> rms,
                           std::span<float> peak) noexcept override {
        if (i != 1)
            return 0;
        if (!rms.empty())
            rms[0] = 0.1f;
        if (!peak.empty())
            peak[0] = 0.9f;
        return 2;
    }
    std::size_t read_vector(std::size_t i, std::span<float> s) noexcept override {
        if (i != 2 || s.empty())
            return 0;
        s[0] = 0.25f;
        return 1;
    }
    std::size_t read_events(std::size_t i,
                            std::span<pulp::format::VisualEvent> e) noexcept override {
        if (i != 3 || e.size() < 2)
            return 0;
        e[0] = {4, 1.0f};
        e[1] = {9, 0.0f};
        return 2;
    }
    std::uint32_t publish_count(std::size_t i) const noexcept override {
        return static_cast<std::uint32_t>(10 + i);
    }
};

} // namespace

TEST_CASE("the visual feed maps channels and reads one to one", "[pulp-vellum][feed]") {
    FakeVisualPort port;
    pulp::vellum::VisualPortFeed feed(port);
    using vellum::value::ChannelShape;

    REQUIRE(feed.channel_count() == 4);
    REQUIRE(feed.channel(0).shape == ChannelShape::scalar);
    REQUIRE(feed.channel(1).shape == ChannelShape::meter);
    REQUIRE(feed.channel(2).shape == ChannelShape::vector);
    REQUIRE(feed.channel(3).shape == ChannelShape::events);
    REQUIRE(feed.channel(2).name == "scope");
    REQUIRE(feed.channel(0).neutral == -1.0f);
    REQUIRE(feed.find("hits", ChannelShape::events) == std::optional<std::size_t>(3));
    REQUIRE(!feed.find("hits", ChannelShape::scalar).has_value());

    float scalar = 0.0f;
    REQUIRE(feed.read_scalar(0, scalar));
    REQUIRE(scalar == 0.5f);
    float rms[1]{};
    float peak[1]{};
    REQUIRE(feed.read_meter(1, rms, peak) == 2);
    REQUIRE(peak[0] == 0.9f);
    float samples[4]{};
    REQUIRE(feed.read_vector(2, samples) == 1);
    REQUIRE(samples[0] == 0.25f);
    vellum::value::ChannelEvent events[4];
    REQUIRE(feed.read_events(3, events) == 2);
    REQUIRE(events[0].frame_index == 4);
    REQUIRE(events[1].frame_index == 9);
    REQUIRE(feed.publish_count(3) == 13);

    // Out-of-range indices are refused rather than forwarded.
    REQUIRE(!feed.read_scalar(9, scalar));
    REQUIRE(feed.read_meter(9, rms, peak) == 0);
    REQUIRE(feed.publish_count(9) == 0);
    REQUIRE(feed.channel(9).name.empty());
}
