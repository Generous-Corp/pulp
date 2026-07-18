// Host-side VST3 editor (IPlugView) negotiation seam
// (pulp::host::detail::vst3_editor.hpp).
//
// Vst3Slot is anonymous-namespace and its create_hosted_editor path needs a
// real native window (the AppKit container), so the negotiation logic is
// exercised here through the free-function seam with a fake IEditController /
// IPlugView. The container itself is covered by the CLAP path and the #12
// real-DAW smoke; this test is the headless proof of the VST3 call sequence.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/detail/vst3_editor.hpp>

#include <public.sdk/source/common/pluginview.h>
#include <public.sdk/source/vst/vsteditcontroller.h>

#include <cstdint>

using namespace Steinberg;
using pulp::host::detail::vst3_attach_editor_view;
using pulp::host::detail::vst3_create_editor_view;
using pulp::host::detail::vst3_detach_editor_view;
using pulp::host::detail::vst3_release_editor_view;
using pulp::host::detail::vst3_resize_editor_view;

namespace {

// Fake IPlugView. Reference counting is neutered (addRef/release are no-ops)
// so the test owns lifetime by scope and can inspect the recorded calls after
// the seam has "released" the view.
class FakeView : public CPluginView {
public:
    bool support_platform = true;
    int32 width = 800;
    int32 height = 600;
    bool resizable = true;

    void* attached_parent = nullptr;
    int attach_calls = 0;
    int removed_calls = 0;
    int on_size_calls = 0;
    int check_constraint_calls = 0;

    // When set, checkSizeConstraint snaps any request to this size.
    bool constrain = false;
    int32 constrain_w = 0;
    int32 constrain_h = 0;

    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1000; }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString /*type*/) SMTG_OVERRIDE {
        return support_platform ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API getSize(ViewRect* r) SMTG_OVERRIDE {
        if (!r) return kInvalidArgument;
        r->left = 0;
        r->top = 0;
        r->right = width;
        r->bottom = height;
        return kResultOk;
    }
    tresult PLUGIN_API attached(void* parent, FIDString /*type*/) SMTG_OVERRIDE {
        attached_parent = parent;
        ++attach_calls;
        return kResultOk;
    }
    tresult PLUGIN_API removed() SMTG_OVERRIDE {
        attached_parent = nullptr;
        ++removed_calls;
        return kResultOk;
    }
    tresult PLUGIN_API canResize() SMTG_OVERRIDE {
        return resizable ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* r) SMTG_OVERRIDE {
        ++check_constraint_calls;
        if (constrain && r) {
            r->right = r->left + constrain_w;
            r->bottom = r->top + constrain_h;
        }
        return kResultOk;
    }
    tresult PLUGIN_API onSize(ViewRect* r) SMTG_OVERRIDE {
        ++on_size_calls;
        (void)r;
        return kResultOk;
    }
};

class FakeController : public Vst::EditController {
public:
    FakeView* view = nullptr;  // returned by createView; nullptr => no editor
    int create_view_calls = 0;

    IPlugView* PLUGIN_API createView(FIDString /*name*/) SMTG_OVERRIDE {
        ++create_view_calls;
        return view;
    }
};

}  // namespace

TEST_CASE("VST3 create_editor_view returns the view with its size",
          "[host][vst3][editor][issue-9]") {
    FakeView view;
    view.width = 640;
    view.height = 480;
    view.resizable = true;
    FakeController ctrl;
    ctrl.view = &view;

    uint32_t w = 0, h = 0;
    bool resizable = false;
    IPlugView* got = vst3_create_editor_view(&ctrl, &w, &h, &resizable);

    REQUIRE(got == static_cast<IPlugView*>(&view));
    REQUIRE(ctrl.create_view_calls == 1);
    REQUIRE(w == 640);
    REQUIRE(h == 480);
    REQUIRE(resizable);
}

TEST_CASE("VST3 create_editor_view returns null when the plug-in has no editor",
          "[host][vst3][editor][issue-9]") {
    FakeController ctrl;  // ctrl.view stays null
    uint32_t w = 1, h = 1;
    REQUIRE(vst3_create_editor_view(&ctrl, &w, &h, nullptr) == nullptr);
    REQUIRE(ctrl.create_view_calls == 1);
}

TEST_CASE("VST3 create_editor_view returns null when the platform is unsupported",
          "[host][vst3][editor][issue-9]") {
    FakeView view;
    view.support_platform = false;
    FakeController ctrl;
    ctrl.view = &view;
    uint32_t w = 1, h = 1;
    REQUIRE(vst3_create_editor_view(&ctrl, &w, &h, nullptr) == nullptr);
}

TEST_CASE("VST3 create_editor_view rejects a zero-area view",
          "[host][vst3][editor][issue-9]") {
    FakeView view;
    view.width = 0;
    view.height = 300;
    FakeController ctrl;
    ctrl.view = &view;
    uint32_t w = 1, h = 1;
    REQUIRE(vst3_create_editor_view(&ctrl, &w, &h, nullptr) == nullptr);
}

TEST_CASE("VST3 attach forwards the container to the plug-in view",
          "[host][vst3][editor][issue-9]") {
    FakeView view;
    int dummy_container = 0;
    void* container = &dummy_container;

    REQUIRE(vst3_attach_editor_view(&view, container));
    REQUIRE(view.attach_calls == 1);
    REQUIRE(view.attached_parent == container);

    // Null inputs are rejected without touching the view.
    REQUIRE_FALSE(vst3_attach_editor_view(&view, nullptr));
    REQUIRE_FALSE(vst3_attach_editor_view(nullptr, container));
    REQUIRE(view.attach_calls == 1);

    vst3_detach_editor_view(&view);
    REQUIRE(view.removed_calls == 1);
}

TEST_CASE("VST3 resize negotiates through the plug-in's size constraint",
          "[host][vst3][editor][issue-9]") {
    FakeView view;
    view.constrain = true;
    view.constrain_w = 500;  // the plug-in snaps any request to 500x400
    view.constrain_h = 400;

    uint32_t w = 1234;
    uint32_t h = 5678;
    REQUIRE(vst3_resize_editor_view(&view, &w, &h));
    REQUIRE(view.check_constraint_calls == 1);
    REQUIRE(view.on_size_calls == 1);
    REQUIRE(w == 500);
    REQUIRE(h == 400);
}
