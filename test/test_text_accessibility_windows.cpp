// Windows UIA TextAccessibilityNode backend tests (font v2 Slice 2.6).
//
// Compile-gated on _WIN32. The Pulp CI matrix doesn't currently exercise
// the Windows lane on every PR (macOS is the only required gate), so
// this file is structured so it:
//   - compiles into pulp-test-text-accessibility-windows on every platform,
//     reporting SUCCEED("...stub on non-Windows...") so the test name shows
//     up consistently in ctest output everywhere; AND
//   - runs the real COM-bridge assertions only on Windows.
//
// When a Windows CI lane is added, the gated TEST_CASEs validate the
// register / unregister / role-mapping bridge end-to-end through the
// IRawElementProviderSimple COM surface.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/text_accessibility.hpp>

#include <string>
#include <vector>

using namespace pulp::view;

#ifndef _WIN32

TEST_CASE("TextAccessibilityNode windows backend: skipped (non-Windows host)",
          "[view][text-a11y][windows][issue-2255]") {
    // Cross-platform sanity: the default backend identifier must NOT be
    // "windows-uia" off Windows. If a future build accidentally pulls
    // the Windows TU on the wrong platform this assertion catches it.
    REQUIRE(accessibility_backend_name() != "windows-uia");
    SUCCEED("Windows UIA backend test skipped — not a Windows host");
}

#else  // _WIN32

#include <pulp/platform/win32_sane.hpp>
#include <UIAutomation.h>

// Forward declaration mirroring the extern "C" hook in
// platform/win/text_accessibility_windows.cpp. The provider type is
// opaque to the test — we only need an IRawElementProviderSimple* to
// drive the COM surface assertions.
extern "C" IRawElementProviderSimple*
pulp_text_accessibility_lookup_windows(const char* id_utf8);

namespace {

void clear_registry() {
    for (const auto& node : snapshot_accessibility_nodes()) {
        unregister_text_accessibility_node(node.id);
    }
    REQUIRE(snapshot_accessibility_nodes().empty());
}

}  // namespace

TEST_CASE("TextAccessibilityNode windows backend reports 'windows-uia'",
          "[view][text-a11y][windows][issue-2255]") {
    REQUIRE(accessibility_backend_name() == "windows-uia");
}

TEST_CASE("TextAccessibilityNode windows: register Label vends UIA provider with text control type",
          "[view][text-a11y][windows][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "win-label-1";
    node.text = "Hello UIA";
    node.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(node);

    auto* prov = pulp_text_accessibility_lookup_windows("win-label-1");
    REQUIRE(prov != nullptr);

    // ProviderOptions must report a server-side provider.
    ProviderOptions opts = static_cast<ProviderOptions>(0);
    REQUIRE(prov->get_ProviderOptions(&opts) == S_OK);
    REQUIRE((opts & ProviderOptions_ServerSideProvider) ==
            ProviderOptions_ServerSideProvider);

    // ControlType property must map Label → UIA_TextControlTypeId.
    VARIANT v;
    VariantInit(&v);
    REQUIRE(prov->GetPropertyValue(UIA_ControlTypePropertyId, &v) == S_OK);
    REQUIRE(v.vt == VT_I4);
    REQUIRE(v.lVal == UIA_TextControlTypeId);
    VariantClear(&v);

    clear_registry();
}

TEST_CASE("TextAccessibilityNode windows: role mapping covers all five enumerators",
          "[view][text-a11y][windows][issue-2255]") {
    clear_registry();

    struct RoleCase {
        TextAccessibilityRole role;
        long expected_uia_id;
        const char* id;
    };
    const RoleCase cases[] = {
        {TextAccessibilityRole::Label,      UIA_TextControlTypeId,   "case-label"},
        {TextAccessibilityRole::Button,     UIA_ButtonControlTypeId, "case-button"},
        {TextAccessibilityRole::TextEditor, UIA_EditControlTypeId,   "case-editor"},
        {TextAccessibilityRole::Heading,    UIA_HeaderControlTypeId, "case-heading"},
        {TextAccessibilityRole::Other,      UIA_CustomControlTypeId, "case-other"},
    };

    for (const auto& c : cases) {
        TextAccessibilityNode n;
        n.id = c.id;
        n.text = c.id;
        n.role = c.role;
        register_text_accessibility_node(n);

        auto* prov = pulp_text_accessibility_lookup_windows(c.id);
        REQUIRE(prov != nullptr);

        VARIANT v;
        VariantInit(&v);
        REQUIRE(prov->GetPropertyValue(UIA_ControlTypePropertyId, &v) == S_OK);
        REQUIRE(v.vt == VT_I4);
        REQUIRE(v.lVal == c.expected_uia_id);
        VariantClear(&v);
    }

    clear_registry();
}

TEST_CASE("TextAccessibilityNode windows: Name property returns the registered text",
          "[view][text-a11y][windows][issue-2255]") {
    clear_registry();

    TextAccessibilityNode n;
    n.id = "win-name";
    n.text = "ScreenReaderMe";
    n.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(n);

    auto* prov = pulp_text_accessibility_lookup_windows("win-name");
    REQUIRE(prov != nullptr);

    VARIANT v;
    VariantInit(&v);
    REQUIRE(prov->GetPropertyValue(UIA_NamePropertyId, &v) == S_OK);
    REQUIRE(v.vt == VT_BSTR);
    REQUIRE(v.bstrVal != nullptr);
    // BSTR is wide-char UTF-16; compare via SysStringLen + char-by-char.
    REQUIRE(SysStringLen(v.bstrVal) == 14);  // "ScreenReaderMe"
    VariantClear(&v);

    clear_registry();
}

TEST_CASE("TextAccessibilityNode windows: unregister drops the provider lookup",
          "[view][text-a11y][windows][issue-2255]") {
    clear_registry();

    TextAccessibilityNode n;
    n.id = "win-drop";
    n.text = "bye";
    register_text_accessibility_node(n);
    REQUIRE(pulp_text_accessibility_lookup_windows("win-drop") != nullptr);

    unregister_text_accessibility_node("win-drop");
    REQUIRE(pulp_text_accessibility_lookup_windows("win-drop") == nullptr);
}

TEST_CASE("TextAccessibilityNode windows: same-id re-register preserves provider identity",
          "[view][text-a11y][windows][issue-2255]") {
    clear_registry();

    TextAccessibilityNode first;
    first.id = "win-stable";
    first.text = "v1";
    first.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(first);
    auto* p1 = pulp_text_accessibility_lookup_windows("win-stable");
    REQUIRE(p1 != nullptr);

    TextAccessibilityNode second;
    second.id = "win-stable";
    second.text = "v2";
    second.role = TextAccessibilityRole::Heading;
    register_text_accessibility_node(second);
    auto* p2 = pulp_text_accessibility_lookup_windows("win-stable");
    REQUIRE(p2 != nullptr);
    // Assistive tech caches provider pointers — the contract is that a
    // re-register with the same id keeps the same COM object so cached
    // pointers stay valid.
    REQUIRE(p1 == p2);

    // ControlType reflects the updated role.
    VARIANT v;
    VariantInit(&v);
    REQUIRE(p2->GetPropertyValue(UIA_ControlTypePropertyId, &v) == S_OK);
    REQUIRE(v.vt == VT_I4);
    REQUIRE(v.lVal == UIA_HeaderControlTypeId);
    VariantClear(&v);

    clear_registry();
}

#endif  // _WIN32
