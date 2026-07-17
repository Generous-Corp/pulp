// AAX custom-editor construction against the real Avid SDK types.
//
// The editor's logic (gesture routing, sizing) is covered SDK-free by
// test_aax_editor.cpp. This suite drives the thin AAX_CEffectGUI shell itself,
// so it only builds when the developer-supplied SDK is configured
// (PULP_ENABLE_AAX=ON + a valid PULP_AAX_SDK_DIR). It asserts the shell the
// host actually instantiates: create_effect_gui() must hand back a live,
// ACF-queryable AAX_IEffectGUI, because a null or mistyped return is exactly
// what leaves Pro Tools on its auto-generated parameter strip.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/aax_effect_gui.hpp>

#include <AAX_IEffectGUI.h>
#include <AAX_UIDs.h>

#include <acfunknown.h>

TEST_CASE("AAX effect-GUI factory returns a live editor object", "[aax][view-bridge]") {
    IACFUnknown* unknown = pulp::format::aax::create_effect_gui();
    REQUIRE(unknown != nullptr);

    // The host reaches the editor through ACF, not the C++ type: the object must
    // answer the AAX_IEffectGUI interface or Pro Tools cannot drive it.
    IACFUnknown* gui = nullptr;
    const ACFRESULT result =
        unknown->QueryInterface(IID_IAAXEffectGUIV2, reinterpret_cast<void**>(&gui));
    CHECK(ACFSUCCEEDED(result));
    CHECK(gui != nullptr);
    if (gui) {
        gui->Release();
    }

    unknown->Release();
}
