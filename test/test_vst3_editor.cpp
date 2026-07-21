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
using pulp::host::detail::vst3_commit_requested_size;
using pulp::host::detail::vst3_create_editor_view;
using pulp::host::detail::vst3_detach_editor_view;
using pulp::host::detail::vst3_release_editor_view;
using pulp::host::detail::vst3_resize_editor_view;

namespace {

// A host frame the seam can install. Only identity matters here — the resize
// path itself is exercised through vst3_commit_requested_size.
class FakeFrame : public IPlugFrame {
public:
    tresult PLUGIN_API resizeView(IPlugView*, ViewRect*) SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API queryInterface(const TUID, void** obj) SMTG_OVERRIDE {
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1000; }
};

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
    int release_calls = 0;

    // The frame currently installed, and whether one was already installed the
    // first time the seam asked the view anything. IPlugView allows a plug-in
    // to call back into its frame from inside attached(), so a frame that
    // arrives late is a frame the plug-in cannot use.
    IPlugFrame* frame = nullptr;
    bool had_frame_on_first_query = false;

    ViewRect last_on_size{};

    // When set, checkSizeConstraint snaps any request to this size.
    bool constrain = false;
    int32 constrain_w = 0;
    int32 constrain_h = 0;
    // When set, onSize refuses.
    bool refuse_on_size = false;

    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE {
        ++release_calls;
        return 1000;
    }

    tresult PLUGIN_API setFrame(IPlugFrame* f) SMTG_OVERRIDE {
        frame = f;
        return kResultOk;
    }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString /*type*/) SMTG_OVERRIDE {
        had_frame_on_first_query = (frame != nullptr);
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
        if (r) last_on_size = *r;
        return refuse_on_size ? kResultFalse : kResultOk;
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
    FakeFrame frame;

    uint32_t w = 0, h = 0;
    bool resizable = false;
    IPlugView* got = vst3_create_editor_view(&ctrl, &frame, &w, &h, &resizable);

    REQUIRE(got == static_cast<IPlugView*>(&view));
    REQUIRE(ctrl.create_view_calls == 1);
    REQUIRE(w == 640);
    REQUIRE(h == 480);
    REQUIRE(resizable);
}

TEST_CASE("VST3 create_editor_view installs the host frame before it asks anything",
          "[host][vst3][editor]") {
    FakeView view;
    FakeController ctrl;
    ctrl.view = &view;
    FakeFrame frame;

    uint32_t w = 0, h = 0;
    REQUIRE(vst3_create_editor_view(&ctrl, &frame, &w, &h, nullptr)
            == static_cast<IPlugView*>(&view));
    // A plug-in may call IPlugFrame::resizeView from inside attached(), so the
    // frame has to be live before the seam touches the view at all.
    REQUIRE(view.had_frame_on_first_query);
    REQUIRE(view.frame == static_cast<IPlugFrame*>(&frame));
}

TEST_CASE("VST3 create_editor_view clears the frame on every failure path",
          "[host][vst3][editor]") {
    FakeFrame frame;
    uint32_t w = 1, h = 1;

    SECTION("unsupported platform type") {
        FakeView view;
        view.support_platform = false;
        FakeController ctrl;
        ctrl.view = &view;
        REQUIRE(vst3_create_editor_view(&ctrl, &frame, &w, &h, nullptr) == nullptr);
        REQUIRE(view.frame == nullptr);
        REQUIRE(view.release_calls == 1);
    }

    SECTION("zero-area view") {
        FakeView view;
        view.width = 0;
        view.height = 300;
        FakeController ctrl;
        ctrl.view = &view;
        REQUIRE(vst3_create_editor_view(&ctrl, &frame, &w, &h, nullptr) == nullptr);
        REQUIRE(view.frame == nullptr);
        REQUIRE(view.release_calls == 1);
    }
}

TEST_CASE("VST3 release_editor_view drops the host frame before the last reference",
          "[host][vst3][editor]") {
    FakeView view;
    FakeFrame frame;
    view.setFrame(&frame);

    vst3_release_editor_view(&view);

    // A plug-in that outlives the host's frame must not be left holding a
    // pointer to it.
    REQUIRE(view.frame == nullptr);
    REQUIRE(view.release_calls == 1);
}

TEST_CASE("VST3 create_editor_view returns null when the plug-in has no editor",
          "[host][vst3][editor][issue-9]") {
    FakeController ctrl;  // ctrl.view stays null
    FakeFrame frame;
    uint32_t w = 1, h = 1;
    REQUIRE(vst3_create_editor_view(&ctrl, &frame, &w, &h, nullptr) == nullptr);
    REQUIRE(ctrl.create_view_calls == 1);
}

TEST_CASE("VST3 create_editor_view tolerates a null host frame",
          "[host][vst3][editor]") {
    FakeView view;
    FakeController ctrl;
    ctrl.view = &view;
    uint32_t w = 0, h = 0;
    REQUIRE(vst3_create_editor_view(&ctrl, nullptr, &w, &h, nullptr)
            == static_cast<IPlugView*>(&view));
    REQUIRE(view.frame == nullptr);
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

TEST_CASE("VST3 commit_requested_size closes a plug-in-driven resize with onSize",
          "[host][vst3][editor]") {
    FakeView view;
    ViewRect requested{0, 0, 1024, 768};

    REQUIRE(vst3_commit_requested_size(&view, requested));
    REQUIRE(view.on_size_calls == 1);
    // The plug-in asked for this size; the host commits exactly it, and does
    // NOT re-run checkSizeConstraint against the plug-in's own request.
    REQUIRE(view.last_on_size.right - view.last_on_size.left == 1024);
    REQUIRE(view.last_on_size.bottom - view.last_on_size.top == 768);
    REQUIRE(view.check_constraint_calls == 0);

    view.refuse_on_size = true;
    REQUIRE_FALSE(vst3_commit_requested_size(&view, requested));
    REQUIRE_FALSE(vst3_commit_requested_size(nullptr, requested));
}
