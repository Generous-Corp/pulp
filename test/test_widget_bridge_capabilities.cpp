// Capability-scoped WidgetBridge. Proves the effectful register_*_api groups are
// gated by the granted CapabilitySet: an ungranted group's JS function is ABSENT
// (typeof === "undefined"), a granted one is present, and the gates are INDEPENDENT
// (granting Filesystem does not grant Exec). Also proves the default constructor
// (all()) preserves the full surface — the full-featured posture for trusted UIs.
//
// Names checked are eager (registered in register_api at construction):
//   exec           → ReloadCapability::Exec       (register_platform_services_exec_api)
//   setImageSource → ReloadCapability::Filesystem (register_widget_assets_api)
// runtime_import (__pulpRuntimeImport__) installs lazily, so it is covered by the
// early-return guard in install_runtime_import_handlers, not a typeof check here.
#include <catch2/catch_test_macros.hpp>

#include <pulp/view/reload_capabilities.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/state/store.hpp>

#include <string>

using namespace pulp::view;
using pulp::state::StateStore;

namespace {
// typeof a global in a freshly-constructed bridge scoped to `caps`.
std::string typeof_global(CapabilitySet caps, const char* name) {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store, nullptr, caps);
    return engine.evaluate(std::string("typeof ") + name).toString();
}
}  // namespace

TEST_CASE("empty capabilities → effectful JS functions are absent",
          "[view][bridge][capabilities]") {
    CapabilitySet none;  // consumer default = most restrictive
    REQUIRE(typeof_global(none, "exec") == "undefined");
    REQUIRE(typeof_global(none, "setImageSource") == "undefined");
}

TEST_CASE("all() capabilities → effectful JS functions are present (dev-unenforced)",
          "[view][bridge][capabilities]") {
    auto all = CapabilitySet::all();
    REQUIRE(typeof_global(all, "exec") == "function");
    REQUIRE(typeof_global(all, "setImageSource") == "function");
}

TEST_CASE("capability gates are independent — Filesystem does not grant Exec",
          "[view][bridge][capabilities]") {
    CapabilitySet fs;
    fs.grant(ReloadCapability::Filesystem);
    REQUIRE(typeof_global(fs, "setImageSource") == "function");  // granted
    REQUIRE(typeof_global(fs, "exec") == "undefined");           // NOT granted

    CapabilitySet ex;
    ex.grant(ReloadCapability::Exec);
    REQUIRE(typeof_global(ex, "exec") == "function");            // granted
    REQUIRE(typeof_global(ex, "setImageSource") == "undefined"); // NOT granted
}
