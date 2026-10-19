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
    catalog.set_routes({
        {"input:1", "Mic 1", pulp::host::AudioRouteKind::PhysicalInput, 1, true},
        {"track:piano", "Piano", pulp::host::AudioRouteKind::Track, 2, true},
        {"track:offline", "Offline", pulp::host::AudioRouteKind::Track, 2, false},
    });
    REQUIRE(catalog.connect("track:piano", 1));
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
    REQUIRE(catalog.connected_route(1).empty());
}
