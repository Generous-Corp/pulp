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

// register_api() drives a table whose entries carry their required capability as
// DATA, so the gate for every effectful group is one field rather than a
// hand-written `if` per call site. Sweep the whole gated set: one representative
// JS symbol per gated group, proven absent under the empty set and present under
// exactly its own capability. A group that loses (or gains) a gate in the table
// fails here rather than shipping an ungated native surface.
//
//   setImageSource   → Filesystem (register_widget_assets_api)
//   showOpenDialog   → Filesystem (register_platform_services_dialog_api)
//   __loadAssetSync__→ Filesystem (register_asset_loading_api)
//   loadFont         → Filesystem (register_font_assets_api)
//   setWidgetSchema  → Storage    (register_widget_schema_api)
//   storageGetItem   → Storage    (register_storage_key_value_api)
//   setAICli         → Ai         (register_platform_services_ai_api)
//   readClipboard    → Clipboard  (register_platform_services_clipboard_api)
//   exec             → Exec       (register_platform_services_exec_api)
TEST_CASE("every gated API group is absent without its capability and present with it",
          "[view][bridge][capabilities]") {
    struct GatedSymbol {
        ReloadCapability gate;
        const char* js_name;
    };
    const GatedSymbol gated[] = {
        {ReloadCapability::Filesystem, "setImageSource"},
        {ReloadCapability::Filesystem, "showOpenDialog"},
        {ReloadCapability::Filesystem, "__loadAssetSync__"},
        {ReloadCapability::Filesystem, "loadFont"},
        {ReloadCapability::Storage,    "setWidgetSchema"},
        {ReloadCapability::Storage,    "storageGetItem"},
        {ReloadCapability::Ai,         "setAICli"},
        {ReloadCapability::Clipboard,  "readClipboard"},
        {ReloadCapability::Exec,       "exec"},
    };

    const CapabilitySet none;
    for (const auto& entry : gated) {
        INFO("gated symbol: " << entry.js_name);
        // Absent under the empty (most restrictive) set…
        REQUIRE(typeof_global(none, entry.js_name) == "undefined");

        // …present once its OWN capability is granted…
        CapabilitySet own;
        own.grant(entry.gate);
        REQUIRE(typeof_global(own, entry.js_name) == "function");

        // …and still absent under an unrelated capability.
        const auto unrelated = entry.gate == ReloadCapability::Network
                                   ? ReloadCapability::Exec
                                   : ReloadCapability::Network;
        CapabilitySet other;
        other.grant(unrelated);
        REQUIRE(typeof_global(other, entry.js_name) == "undefined");
    }
}

// Pure-UI groups carry no effect and are registered whatever the granted set is.
// This is the other half of the table's contract: an entry with no gate must not
// acquire one by accident.
TEST_CASE("ungated API groups are present under the empty capability set",
          "[view][bridge][capabilities]") {
    const CapabilitySet none;
    REQUIRE(typeof_global(none, "createKnob") == "function");   // widget factory
    REQUIRE(typeof_global(none, "setBackground") == "function"); // visual style
    REQUIRE(typeof_global(none, "setTheme") == "function");      // theme
    REQUIRE(typeof_global(none, "removeWidget") == "function");  // metadata/removal
}
