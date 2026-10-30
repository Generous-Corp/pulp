#include <catch2/catch_test_macros.hpp>

#include <pulp/host/audio_route_catalog.hpp>

#include <array>
#include <utility>

namespace {

class FixtureCatalog final : public pulp::host::AudioRouteCatalog {
  public:
    std::vector<pulp::host::AudioRouteInfo> enumerate() const override {
        return routes;
    }

    void set_change_callback(ChangeCallback callback) override {
        callback_ = std::move(callback);
    }

    bool connect(std::string_view stable_id, std::size_t bus_index) override {
        if (bus_index > 1)
            return false;
        for (const auto& route : routes) {
            if (route.stable_id == stable_id && route.available) {
                selected[bus_index] = route.stable_id;
                return true;
            }
        }
        return false;
    }

    std::string connected_route(std::size_t bus_index) const override {
        return bus_index < selected.size() ? selected[bus_index] : std::string{};
    }

    void notify() {
        if (callback_)
            callback_();
    }

    std::vector<pulp::host::AudioRouteInfo> routes{
        {"input:1", "Mic 1", pulp::host::AudioRouteKind::PhysicalInput, 1, true},
        {"track:piano", "Piano", pulp::host::AudioRouteKind::Track, 2, true},
        {"bus:side", "Side Bus", pulp::host::AudioRouteKind::Bus, 2, false},
    };

  private:
    ChangeCallback callback_;
    std::array<std::string, 2> selected{};
};

} // namespace

TEST_CASE("AudioRouteCatalog exposes host-owned source identity and selection") {
    FixtureCatalog catalog;
    bool changed = false;
    catalog.set_change_callback([&] { changed = true; });

    const auto routes = catalog.enumerate();
    REQUIRE(routes.size() == 3);
    REQUIRE(routes[0].kind == pulp::host::AudioRouteKind::PhysicalInput);
    REQUIRE(routes[1].display_name == "Piano");
    REQUIRE(catalog.connect("bus:side", 0) == false);
    REQUIRE(catalog.connect("track:piano", 0) == true);
    REQUIRE(catalog.connected_route(0) == "track:piano");
    REQUIRE(catalog.connect("track:piano", 2) == false);

    catalog.notify();
    REQUIRE(changed);
}

TEST_CASE("MutableAudioRouteCatalog provides checkmarked host selection state") {
    pulp::host::MutableAudioRouteCatalog catalog;
    int changes = 0;
    catalog.set_change_callback([&] { ++changes; });
    std::vector<std::string> connected;
    catalog.set_connect_callback(
        [&](const pulp::host::AudioRouteInfo& route, std::size_t bus_index) {
            connected.push_back(route.stable_id + ":" + std::to_string(bus_index));
            return route.stable_id != "input:1";
        });
    catalog.set_routes({
        {"input:1", "Mic 1", pulp::host::AudioRouteKind::PhysicalInput, 1, true},
        {"track:piano", "Piano", pulp::host::AudioRouteKind::Track, 2, true},
        {"track:offline", "Offline", pulp::host::AudioRouteKind::Track, 2, false},
    });
    REQUIRE(catalog.connect("track:piano", 1));
    REQUIRE(catalog.connected_route(1) == "track:piano");
    REQUIRE(catalog.selected_route_ids() == std::vector<std::string>{"", "track:piano"});
    REQUIRE(connected == std::vector<std::string>{"track:piano:1"});
    REQUIRE_FALSE(catalog.connect("input:1", 1));
    REQUIRE(catalog.connected_route(1) == "track:piano");
    REQUIRE_FALSE(catalog.connect("track:offline", 1));
    REQUIRE_FALSE(catalog.connect("missing", 1));
    REQUIRE(changes == 2);

    catalog.set_routes({
        {"track:piano", "Piano", pulp::host::AudioRouteKind::Track, 2, true},
        {"input:1", "Mic 1", pulp::host::AudioRouteKind::PhysicalInput, 1, true},
    });
    REQUIRE(catalog.connected_route(1) == "track:piano");

    catalog.set_routes({
        {"track:drums", "Drums", pulp::host::AudioRouteKind::Track, 2, true},
    });
    // A temporary project/device refresh keeps the stable identity so the UI
    // can render the source as unavailable and reconnect it when it returns.
    REQUIRE(catalog.connected_route(1) == "track:piano");
    REQUIRE(catalog.selected_route_ids() == std::vector<std::string>{"", "track:piano"});

    catalog.set_routes({
        {"track:piano", "Piano", pulp::host::AudioRouteKind::Track, 2, true},
    });
    REQUIRE(catalog.connected_route(1) == "track:piano");
}

TEST_CASE("MutableAudioRouteCatalog restores only available stable route IDs") {
    pulp::host::MutableAudioRouteCatalog catalog;
    catalog.set_routes({
        {"input:1", "Mic 1", pulp::host::AudioRouteKind::PhysicalInput, 1, true},
        {"track:piano", "Piano", pulp::host::AudioRouteKind::Track, 2, true},
        {"track:offline", "Offline", pulp::host::AudioRouteKind::Track, 2, false},
    });

    std::vector<std::string> connected;
    catalog.set_connect_callback(
        [&](const pulp::host::AudioRouteInfo& route, std::size_t bus_index) {
            connected.push_back(route.stable_id + ":" + std::to_string(bus_index));
            return true;
        });
    const std::vector<std::string> saved{"input:1", "track:piano", "track:offline", "missing"};
    catalog.restore_selected_route_ids(saved);
    REQUIRE(catalog.selected_route_ids() == std::vector<std::string>{"input:1", "track:piano"});
    REQUIRE(connected == std::vector<std::string>{"input:1:0", "track:piano:1"});
}

TEST_CASE("MutableAudioRouteCatalog keeps independent left and right selections") {
    pulp::host::MutableAudioRouteCatalog catalog;
    catalog.set_routes({
        {"track:piano", "Piano", pulp::host::AudioRouteKind::Track, 2, true},
        {"bus:reverb", "Reverb", pulp::host::AudioRouteKind::Bus, 2, true},
    });

    REQUIRE(catalog.connect_channel("track:piano", 0, 0));
    REQUIRE(catalog.connect_channel("bus:reverb", 0, 1));
    REQUIRE(catalog.connected_route_channel(0, 0) == "track:piano");
    REQUIRE(catalog.connected_route_channel(0, 1) == "bus:reverb");
    REQUIRE(catalog.connected_route(0) == "track:piano");

    REQUIRE_FALSE(catalog.connect_channel("track:piano", 0, 2));
    REQUIRE(catalog.disconnect(0));
    REQUIRE(catalog.connected_route_channel(0, 0).empty());
    REQUIRE(catalog.connected_route_channel(0, 1).empty());
}

TEST_CASE("MutableAudioRouteCatalog binds independent channels and clears right-only state") {
    pulp::host::MutableAudioRouteCatalog catalog;
    catalog.set_routes({
        {"stereo", "Stereo", pulp::host::AudioRouteKind::Track, 2, true},
        {"mono", "Mono", pulp::host::AudioRouteKind::Track, 1, true},
    });
    std::vector<std::string> bindings;
    catalog.set_connect_channel_callback(
        [&](const auto& route, std::size_t bus, std::size_t channel) {
            bindings.push_back(route.stable_id + ":" + std::to_string(bus) + ":" +
                               std::to_string(channel));
            return true;
        });

    REQUIRE(catalog.connect_channel("stereo", 0, 1));
    REQUIRE(bindings == std::vector<std::string>{"stereo:0:1"});
    REQUIRE_FALSE(catalog.connect_channel("mono", 0, 1));
    REQUIRE(catalog.disconnect(0));
    REQUIRE(catalog.connected_route_channel(0, 1).empty());
}

TEST_CASE("MutableAudioRouteCatalog rejects independent binding through legacy callback") {
    pulp::host::MutableAudioRouteCatalog catalog;
    catalog.set_routes({{"stereo", "Stereo", pulp::host::AudioRouteKind::Track, 2, true}});
    catalog.set_connect_callback([](const auto&, std::size_t) { return true; });

    REQUIRE_FALSE(catalog.connect_channel("stereo", 0, 1));
}

TEST_CASE("MutableAudioRouteCatalog whole-bus transitions match graph bindings") {
    pulp::host::MutableAudioRouteCatalog catalog;
    catalog.set_routes({
        {"left", "Left", pulp::host::AudioRouteKind::Track, 2, true},
        {"right", "Right", pulp::host::AudioRouteKind::Track, 2, true},
        {"bus", "Bus", pulp::host::AudioRouteKind::Bus, 2, true},
    });
    std::array<std::string, 2> graph;
    catalog.set_connect_channel_callback(
        [&](const auto& route, std::size_t, std::size_t channel) {
            graph[channel] = route.stable_id;
            return true;
        });
    REQUIRE(catalog.connect_channel("left", 0, 0));
    REQUIRE(catalog.connect_channel("right", 0, 1));
    REQUIRE_FALSE(catalog.connect("bus", 0));
    REQUIRE(graph[1] == "right");
    REQUIRE(catalog.connected_route_channel(0, 1) == graph[1]);

    bool reject = true;
    catalog.set_connect_callback([&](const auto& route, std::size_t) {
        if (reject)
            return false;
        graph = {route.stable_id, {}};
        return true;
    });
    REQUIRE_FALSE(catalog.connect("bus", 0));
    REQUIRE(catalog.connected_route_channel(0, 1) == graph[1]);
    reject = false;
    REQUIRE(catalog.connect("bus", 0));
    REQUIRE(graph[0] == "bus");
    REQUIRE(graph[1].empty());
    REQUIRE(catalog.connected_route_channel(0, 0) == graph[0]);
    REQUIRE(catalog.connected_route_channel(0, 1) == graph[1]);
}
