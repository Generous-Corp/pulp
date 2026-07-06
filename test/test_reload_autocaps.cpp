// Automatic capability inference from a scripted UI's JS. A UI that only draws maps
// to no capabilities; calling an effectful bridge function declares the capability
// that gates it; whole-token matching avoids false hits on longer identifiers.
#include <catch2/catch_test_macros.hpp>

#include <pulp/view/reload_autocaps.hpp>

#include <string>
#include <vector>

using namespace pulp::view;

TEST_CASE("autocaps: a pure-UI script needs no capabilities", "[reload][autocaps]") {
    const auto pure = infer_capabilities_from_js(
        "createKnob('gain',0,0,48,48); ctx.fillRect(0,0,10,10); el.style.color='red';");
    REQUIRE(pure.empty());
}

TEST_CASE("autocaps: effectful calls declare their capability", "[reload][autocaps]") {
    REQUIRE(infer_capabilities_from_js("pulp.exec('ls')").has(ReloadCapability::Exec));
    REQUIRE(infer_capabilities_from_js("readClipboard()").has(ReloadCapability::Clipboard));
    REQUIRE(infer_capabilities_from_js("img.setImageSource('a.png')").has(ReloadCapability::Filesystem));
    REQUIRE(infer_capabilities_from_js("storageSetItem('k','v')").has(ReloadCapability::Storage));
    REQUIRE(infer_capabilities_from_js("getAICli()").has(ReloadCapability::Ai));
    REQUIRE(infer_capabilities_from_js("__pulpRuntimeImport__(html,src)").has(ReloadCapability::RuntimeImport));
}

TEST_CASE("autocaps: whole-token match ignores longer identifiers", "[reload][autocaps]") {
    // 'exec' must not match 'executeQuery' / 'myExec'; 'loadFont' must not match
    // 'loadFontMetrics'.
    const auto a = infer_capabilities_from_js("function executeQuery(){} const myExecutor=1;");
    REQUIRE_FALSE(a.has(ReloadCapability::Exec));
    const auto b = infer_capabilities_from_js("loadFontMetrics()");
    REQUIRE_FALSE(b.has(ReloadCapability::Filesystem));
    // But a real bounded call still matches.
    REQUIRE(infer_capabilities_from_js("loadFont('x')").has(ReloadCapability::Filesystem));
}

TEST_CASE("autocaps: capability NAMES come out sorted + deduped for the manifest",
          "[reload][autocaps]") {
    const auto names = infer_capability_names_from_js(
        "storageSetItem('k','v'); pulp.exec('x'); setImageSource('a'); loadStylePreset('p');");
    // exec, filesystem (setImageSource), storage (storageSetItem + loadStylePreset once)
    REQUIRE(names == std::vector<std::string>{"exec", "filesystem", "storage"});
}
