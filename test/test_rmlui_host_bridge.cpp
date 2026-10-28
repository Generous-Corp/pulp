#include <catch2/catch_test_macros.hpp>
#include <pulp/view/rmlui_host_bridge.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace pulp::view::rmlui;

namespace {
struct Lua final : LuaHost {
    HostStatus initialise() override {
        ++initialise_count;
        if (throw_on_initialise) throw std::runtime_error("init");
        return result;
    }
    HostStatus dispatch(std::string_view callback) override {
        if (throw_on_dispatch) throw std::runtime_error("dispatch");
        callbacks.emplace_back(callback);
        return result;
    }
    HostStatus result = HostStatus::ok;
    int initialise_count = 0;
    bool throw_on_initialise = false;
    bool throw_on_dispatch = false;
    std::vector<std::string> callbacks;
};
struct Elements final : ElementHost {
    HostStatus register_element(ElementRegistration registration) override {
        names.emplace_back(registration.name);
        registrations.push_back(std::move(registration));
        if (throw_on_register) throw std::runtime_error("register");
        return result;
    }
    HostStatus result = HostStatus::ok;
    bool throw_on_register = false;
    std::vector<std::string> names;
    std::vector<ElementRegistration> registrations;
};
struct Product final : ProductHost {
    HostStatus dispatch(ProductEvent event) override {
        if (throw_on_dispatch) throw std::runtime_error("product");
        events.push_back(std::move(event));
        return result;
    }
    HostStatus result = HostStatus::ok;
    bool throw_on_dispatch = false;
    std::vector<ProductEvent> events;
};
} // namespace

TEST_CASE("RmlUi host bridge requires all providers and stages registration") {
    HostBridge bridge;
    auto lua = std::make_shared<Lua>();
    auto elements = std::make_shared<Elements>();
    auto product = std::make_shared<Product>();
    REQUIRE(bridge.activate() == HostStatus::missing_capability);
    REQUIRE(bridge.bind_lua(lua) == HostStatus::ok);
    REQUIRE(bridge.bind_elements(elements) == HostStatus::ok);
    REQUIRE(bridge.register_element({"knob", [](std::string_view) { return HostStatus::ok; }}) == HostStatus::not_active);
    REQUIRE(bridge.bind_product(product) == HostStatus::ok);
    REQUIRE(bridge.activate() == HostStatus::ok);
    REQUIRE(lua->initialise_count == 1);
    REQUIRE(bridge.register_element({"knob", [](std::string_view) { return HostStatus::ok; }}) == HostStatus::ok);
    REQUIRE(elements->names == std::vector<std::string>{"knob"});
    REQUIRE(elements->registrations.front().dispatch("before-close") == HostStatus::ok);
    bridge.close();
    REQUIRE(elements->registrations.front().dispatch("after-close") == HostStatus::not_active);
}

TEST_CASE("RmlUi host bridge owns providers and event identifiers") {
    HostBridge bridge;
    auto lua = std::make_shared<Lua>();
    auto elements = std::make_shared<Elements>();
    auto product = std::make_shared<Product>();
    bridge.bind_lua(lua); bridge.bind_elements(elements); bridge.bind_product(product);
    REQUIRE(bridge.dispatch_lua("onload") == HostStatus::not_active);
    REQUIRE(bridge.activate() == HostStatus::ok);
    std::string callback = "onload";
    REQUIRE(bridge.dispatch_lua(callback) == HostStatus::ok);
    std::string id = "Cutoff";
    REQUIRE(bridge.dispatch_product({ProductEventKind::parameter, id, 42.0}) == HostStatus::ok);
    id = "changed";
    REQUIRE(product->events.front().id == "Cutoff");
    REQUIRE(lua->callbacks == std::vector<std::string>{"onload"});
    bridge.close();
    REQUIRE_FALSE(bridge.active());
    REQUIRE(bridge.dispatch_product({ProductEventKind::action, "PresetNext", 0.0}) == HostStatus::not_active);
}

TEST_CASE("RmlUi host bridge propagates refusals and catches provider exceptions") {
    HostBridge bridge;
    auto lua = std::make_shared<Lua>();
    auto elements = std::make_shared<Elements>();
    auto product = std::make_shared<Product>();
    lua->throw_on_initialise = true;
    bridge.bind_lua(lua); bridge.bind_elements(elements); bridge.bind_product(product);
    REQUIRE(bridge.activate() == HostStatus::rejected);
    REQUIRE_FALSE(bridge.active());
    lua->throw_on_initialise = false;
    REQUIRE(bridge.bind_lua(lua) == HostStatus::ok);
    REQUIRE(bridge.bind_elements(elements) == HostStatus::ok);
    REQUIRE(bridge.bind_product(product) == HostStatus::ok);
    REQUIRE(bridge.activate() == HostStatus::ok);
    REQUIRE(bridge.dispatch_product({static_cast<ProductEventKind>(99), "bad", 0.0}) ==
            HostStatus::rejected);
    lua->throw_on_dispatch = true;
    REQUIRE(bridge.dispatch_lua("onload") == HostStatus::rejected);
    REQUIRE_FALSE(bridge.active());
    // A provider fault closes the bridge and releases all ownership; it cannot
    // be resumed with a stale callback graph.
    REQUIRE(bridge.dispatch_product({ProductEventKind::action, "PresetNext", 0.0}) ==
            HostStatus::not_active);
    REQUIRE(bridge.bind_lua(lua) == HostStatus::ok);
    REQUIRE(bridge.bind_elements(elements) == HostStatus::ok);
    REQUIRE(bridge.bind_product(product) == HostStatus::ok);
    REQUIRE(bridge.activate() == HostStatus::ok);
    product->throw_on_dispatch = true;
    REQUIRE(bridge.dispatch_product({ProductEventKind::action, "PresetNext", 0.0}) == HostStatus::rejected);
    REQUIRE_FALSE(bridge.active());
}

TEST_CASE("RmlUi host bridge rejects null providers and is noncopyable") {
    HostBridge bridge;
    REQUIRE(bridge.bind_lua(nullptr) == HostStatus::rejected);
    REQUIRE(bridge.bind_elements(nullptr) == HostStatus::rejected);
    REQUIRE(bridge.bind_product(nullptr) == HostStatus::rejected);
    static_assert(!std::is_copy_constructible_v<HostBridge>);
    static_assert(!std::is_move_constructible_v<HostBridge>);
}

TEST_CASE("RmlUi host bridge close releases provider ownership") {
    HostBridge bridge;
    auto lua = std::make_shared<Lua>();
    auto elements = std::make_shared<Elements>();
    auto product = std::make_shared<Product>();
    std::weak_ptr<Lua> weak_lua = lua;
    REQUIRE(bridge.bind_lua(lua) == HostStatus::ok);
    REQUIRE(bridge.bind_elements(elements) == HostStatus::ok);
    REQUIRE(bridge.bind_product(product) == HostStatus::ok);
    lua.reset();
    REQUIRE_FALSE(weak_lua.expired());
    bridge.close();
    REQUIRE(weak_lua.expired());
}

TEST_CASE("RmlUi host bridge rejects retained callbacks after reactivation") {
    HostBridge bridge;
    auto lua = std::make_shared<Lua>();
    auto elements = std::make_shared<Elements>();
    auto product = std::make_shared<Product>();
    bridge.bind_lua(lua);
    bridge.bind_elements(elements);
    bridge.bind_product(product);
    REQUIRE(bridge.activate() == HostStatus::ok);
    REQUIRE(bridge.register_element({"knob", [](std::string_view) { return HostStatus::ok; }}) ==
            HostStatus::ok);
    auto retained = elements->registrations.front().dispatch;

    bridge.close();
    REQUIRE(bridge.bind_lua(lua) == HostStatus::ok);
    REQUIRE(bridge.bind_elements(elements) == HostStatus::ok);
    REQUIRE(bridge.bind_product(product) == HostStatus::ok);
    REQUIRE(bridge.activate() == HostStatus::ok);
    REQUIRE(retained("stale") == HostStatus::not_active);
}

TEST_CASE("RmlUi host bridge quarantines partial element registration") {
    HostBridge bridge;
    auto lua = std::make_shared<Lua>();
    auto elements = std::make_shared<Elements>();
    auto product = std::make_shared<Product>();
    bridge.bind_lua(lua);
    bridge.bind_elements(elements);
    bridge.bind_product(product);
    REQUIRE(bridge.activate() == HostStatus::ok);
    elements->throw_on_register = true;
    REQUIRE(bridge.register_element({"knob", [](std::string_view) { return HostStatus::ok; }}) ==
            HostStatus::rejected);
    REQUIRE_FALSE(bridge.active());
    REQUIRE(bridge.dispatch_product({ProductEventKind::action, "PresetNext", 0.0}) ==
            HostStatus::not_active);
    REQUIRE(elements->registrations.front().dispatch("stale") == HostStatus::not_active);
}
