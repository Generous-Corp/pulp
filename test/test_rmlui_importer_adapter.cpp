#include <catch2/catch_test_macros.hpp>
#include <pulp/view/rmlui_importer_adapter.hpp>

#include <memory>
#include <stdexcept>

using namespace pulp::view::rmlui;

namespace {
struct Lua final : LuaHost {
    HostStatus initialise() override {
        return HostStatus::ok;
    }
    HostStatus dispatch(std::string_view) override {
        return HostStatus::ok;
    }
};
struct Elements final : ElementHost {
    HostStatus register_element(ElementRegistration) override {
        return HostStatus::ok;
    }
};
struct Product final : ProductHost {
    HostStatus dispatch(ProductEvent) override {
        return HostStatus::ok;
    }
};
struct ThrowingProduct final : ProductHost {
    HostStatus dispatch(ProductEvent) override {
        throw std::runtime_error("product callback failure");
    }
};
} // namespace

TEST_CASE("importer adapter fails closed without public capabilities") {
    ImporterAdapter adapter;
    REQUIRE(adapter.activate() == HostStatus::missing_capability);
    REQUIRE_FALSE(adapter.receipt().host_bridge_active);
    REQUIRE(adapter.dispatch_product({ProductEventKind::action, "PresetNext", 0.0}) ==
            HostStatus::not_active);
}

TEST_CASE("importer adapter emits a typed capability receipt") {
    ImporterAdapter adapter;
    REQUIRE(adapter.bind_lua(std::make_shared<Lua>()) == HostStatus::ok);
    REQUIRE(adapter.bind_elements(std::make_shared<Elements>()) == HostStatus::ok);
    REQUIRE(adapter.bind_product(std::make_shared<Product>()) == HostStatus::ok);
    REQUIRE(adapter.activate() == HostStatus::ok);
    REQUIRE(adapter.receipt().host_bridge_active);
    REQUIRE(adapter.receipt().lua_initialised);
    REQUIRE(adapter.receipt().product_callbacks);
    REQUIRE(adapter.register_element({"knob", [](std::string_view) { return HostStatus::ok; }}) ==
            HostStatus::ok);
    REQUIRE(adapter.receipt().elements_registered);
    adapter.close();
    REQUIRE_FALSE(adapter.receipt().host_bridge_active);
}

TEST_CASE("importer adapter clears its receipt when a provider quarantines the bridge") {
    ImporterAdapter adapter;
    REQUIRE(adapter.bind_lua(std::make_shared<Lua>()) == HostStatus::ok);
    REQUIRE(adapter.bind_elements(std::make_shared<Elements>()) == HostStatus::ok);
    REQUIRE(adapter.bind_product(std::make_shared<ThrowingProduct>()) == HostStatus::ok);
    REQUIRE(adapter.activate() == HostStatus::ok);
    REQUIRE(adapter.receipt().host_bridge_active);
    REQUIRE(adapter.receipt().lua_initialised);
    REQUIRE(adapter.receipt().product_callbacks);

    REQUIRE(adapter.dispatch_product({ProductEventKind::action, "PresetNext", 0.0}) ==
            HostStatus::rejected);
    REQUIRE_FALSE(adapter.receipt().host_bridge_active);
    REQUIRE_FALSE(adapter.receipt().lua_initialised);
    REQUIRE_FALSE(adapter.receipt().elements_registered);
    REQUIRE_FALSE(adapter.receipt().product_callbacks);
}
