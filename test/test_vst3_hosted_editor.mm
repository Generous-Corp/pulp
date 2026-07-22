// Vst3Slot's hosted-editor composition, against a REAL NSView parent.
//
// test_vst3_editor.cpp covers the free-function negotiation seam in isolation.
// What it cannot cover is the part that only exists once the slot is holding
// the IPlugFrame: IPlugView documents IPlugFrame::resizeView() as callable from
// inside attached(), which is how a plug-in reports the size it really wants,
// and honoring it needs the slot's view + container to be published before the
// attach call. That ordering is the whole fix, so it gets a test that drives
// the real Vst3Slot through make_vst3_slot() with a fake IEditController whose
// view calls back into the host mid-attach.
//
// macOS-only: create_editor_container is a stub everywhere else, so there is no
// real parent to hand a plug-in.

#include <catch2/catch_test_macros.hpp>

#include "plugin_slot_vst3_internal.hpp"

#include <public.sdk/source/common/pluginview.h>
#include <public.sdk/source/vst/vsteditcontroller.h>

#import <AppKit/AppKit.h>

#include <memory>

using namespace Steinberg;
using pulp::host::make_vst3_slot;
using pulp::host::PluginInfo;
using pulp::host::PluginSlot;

namespace {

// A plug-in view that can ask its host to resize it from inside attached(),
// the way a real self-sizing editor does.
class SelfSizingView : public CPluginView {
public:
    int32 initial_width = 400;
    int32 initial_height = 300;

    // When set, attached() calls IPlugFrame::resizeView with this size before
    // returning — the shape of any editor that only learns its real size once
    // it has a parent to measure against.
    bool resize_during_attach = false;
    int32 attach_width = 0;
    int32 attach_height = 0;

    bool refuse_attach = false;
    bool had_frame_during_attach = false;
    tresult resize_result = kResultFalse;

    int attach_calls = 0;
    int removed_calls = 0;
    int on_size_calls = 0;
    int32 committed_width = 0;
    int32 committed_height = 0;

    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1000; }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString) SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API canResize() SMTG_OVERRIDE { return kResultTrue; }

    tresult PLUGIN_API getSize(ViewRect* r) SMTG_OVERRIDE {
        if (!r) return kInvalidArgument;
        r->left = 0;
        r->top = 0;
        r->right = initial_width;
        r->bottom = initial_height;
        return kResultOk;
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) SMTG_OVERRIDE {
        ++attach_calls;
        if (refuse_attach) return kResultFalse;
        CPluginView::attached(parent, type);
        had_frame_during_attach = (plugFrame != nullptr);
        if (resize_during_attach && plugFrame) {
            ViewRect want{0, 0, attach_width, attach_height};
            resize_result = plugFrame->resizeView(this, &want);
        }
        return kResultOk;
    }

    tresult PLUGIN_API removed() SMTG_OVERRIDE {
        ++removed_calls;
        return CPluginView::removed();
    }

    tresult PLUGIN_API onSize(ViewRect* r) SMTG_OVERRIDE {
        ++on_size_calls;
        if (r) {
            committed_width = r->right - r->left;
            committed_height = r->bottom - r->top;
        }
        return kResultOk;
    }
};

// Combined plug-in: one object implementing IComponent and IEditController,
// counting the IPluginBase::terminate() calls it receives.
class CountingCombined : public Vst::EditController, public Vst::IComponent {
public:
    SelfSizingView* view = nullptr;
    int terminate_calls = 0;

    IPlugView* PLUGIN_API createView(FIDString) SMTG_OVERRIDE {
        return static_cast<IPlugView*>(view);
    }

    tresult PLUGIN_API initialize(FUnknown*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API terminate() SMTG_OVERRIDE {
        ++terminate_calls;
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API getControllerClassId(TUID) SMTG_OVERRIDE { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(Vst::IoMode) SMTG_OVERRIDE { return kNotImplemented; }
    int32 PLUGIN_API getBusCount(Vst::MediaType, Vst::BusDirection) SMTG_OVERRIDE { return 0; }
    tresult PLUGIN_API getBusInfo(Vst::MediaType, Vst::BusDirection, int32,
                                  Vst::BusInfo&) SMTG_OVERRIDE {
        return kResultFalse;
    }
    tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo&, Vst::RoutingInfo&) SMTG_OVERRIDE {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus(Vst::MediaType, Vst::BusDirection, int32,
                                   TBool) SMTG_OVERRIDE {
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool) SMTG_OVERRIDE { return kResultOk; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE {
        if (FUnknownPrivate::iidEqual(iid, Vst::IComponent::iid)) {
            *obj = static_cast<Vst::IComponent*>(this);
            addRef();
            return kResultTrue;
        }
        // Everything else, FUnknown included, resolves through the single
        // EditController half — so the two interface pointers are one object.
        return Vst::EditController::queryInterface(iid, obj);
    }
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1000; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1000; }
};

// A borrowed on-screen-less window whose contentView is a real NSView the
// plug-in can be parented into.
struct TestWindow {
    NSWindow* window = nil;

    TestWindow() {
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1200, 900)
                                             styleMask:NSWindowStyleMaskBorderless
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        [window setContentView:[[[NSView alloc]
            initWithFrame:NSMakeRect(0, 0, 1200, 900)] autorelease]];
    }
    ~TestWindow() { [window release]; }

    void* handle() const { return (__bridge void*) window; }
    NSView* content() const { return [window contentView]; }
};

std::unique_ptr<PluginSlot> slot_for(CountingCombined& plugin) {
    PluginInfo info;
    info.name = "FakeVst3";
    return make_vst3_slot(info, static_cast<Vst::IComponent*>(&plugin),
                          /*processor=*/nullptr,
                          static_cast<Vst::IEditController*>(&plugin));
}

}  // namespace

TEST_CASE("VST3 editor embeds into a real parent view",
          "[host][vst3][editor][hosted-editor]") {
    @autoreleasepool {
        SelfSizingView view;
        CountingCombined plugin;
        plugin.view = &view;
        TestWindow win;

        auto slot = slot_for(plugin);
        REQUIRE(slot);
        REQUIRE(slot->has_editor());

        auto editor = slot->create_hosted_editor(win.handle());
        REQUIRE(editor != nullptr);
        REQUIRE(editor->native_handle != nullptr);
        REQUIRE(editor->width == 400);
        REQUIRE(editor->height == 300);
        REQUIRE(view.attach_calls == 1);

        // The container is a real subview of the window's content view, sized
        // from the plug-in's own getSize.
        NSView* container = (__bridge NSView*) editor->native_handle;
        REQUIRE([container superview] == win.content());
        REQUIRE([container frame].size.width == 400.0);
        REQUIRE([container frame].size.height == 300.0);

        slot->destroy_hosted_editor(std::move(editor));
        REQUIRE(view.removed_calls == 1);
        REQUIRE([[win.content() subviews] count] == 0u);
    }
}

TEST_CASE("VST3 editor has a live host frame during attached()",
          "[host][vst3][editor][hosted-editor]") {
    @autoreleasepool {
        SelfSizingView view;
        CountingCombined plugin;
        plugin.view = &view;
        TestWindow win;

        auto slot = slot_for(plugin);
        auto editor = slot->create_hosted_editor(win.handle());
        REQUIRE(editor != nullptr);

        // Without setFrame before attached(), a plug-in that reports its real
        // size from inside attached() has nothing to report it to — and some
        // refuse to attach at all.
        REQUIRE(view.had_frame_during_attach);

        slot->destroy_hosted_editor(std::move(editor));
    }
}

TEST_CASE("VST3 editor honors a resize requested from inside attached()",
          "[host][vst3][editor][hosted-editor]") {
    @autoreleasepool {
        SelfSizingView view;
        view.resize_during_attach = true;
        view.attach_width = 960;
        view.attach_height = 640;
        CountingCombined plugin;
        plugin.view = &view;
        TestWindow win;

        auto slot = slot_for(plugin);
        auto editor = slot->create_hosted_editor(win.handle());
        REQUIRE(editor != nullptr);

        REQUIRE(view.resize_result == kResultTrue);
        // Same callstack: the host resizes the platform view, then commits with
        // onSize.
        REQUIRE(view.on_size_calls == 1);
        REQUIRE(view.committed_width == 960);
        REQUIRE(view.committed_height == 640);

        NSView* container = (__bridge NSView*) editor->native_handle;
        REQUIRE([container frame].size.width == 960.0);
        REQUIRE([container frame].size.height == 640.0);

        // The reported size is what the editor ENDED UP at, not the size
        // queried before the view existed — a host that attaches at the stale
        // 400x300 clips the editor.
        REQUIRE(editor->width == 960);
        REQUIRE(editor->height == 640);

        slot->destroy_hosted_editor(std::move(editor));
    }
}

TEST_CASE("VST3 editor resize request can be vetoed by the embedder",
          "[host][vst3][editor][hosted-editor]") {
    @autoreleasepool {
        SelfSizingView view;
        view.resize_during_attach = true;
        view.attach_width = 4000;
        view.attach_height = 4000;
        CountingCombined plugin;
        plugin.view = &view;
        TestWindow win;

        auto slot = slot_for(plugin);
        int handler_calls = 0;
        slot->set_editor_resize_request_handler([&](uint32_t, uint32_t) {
            ++handler_calls;
            return false;  // the embedder has no room for this
        });

        auto editor = slot->create_hosted_editor(win.handle());
        REQUIRE(editor != nullptr);

        REQUIRE(handler_calls == 1);
        REQUIRE(view.resize_result == kResultFalse);
        // A vetoed request must change nothing: no commit, no container resize.
        REQUIRE(view.on_size_calls == 0);
        NSView* container = (__bridge NSView*) editor->native_handle;
        REQUIRE([container frame].size.width == 400.0);
        REQUIRE(editor->width == 400);

        slot->destroy_hosted_editor(std::move(editor));
    }
}

TEST_CASE("VST3 editor request reaches an installed handler with the new size",
          "[host][vst3][editor][hosted-editor]") {
    @autoreleasepool {
        SelfSizingView view;
        view.resize_during_attach = true;
        view.attach_width = 512;
        view.attach_height = 288;
        CountingCombined plugin;
        plugin.view = &view;
        TestWindow win;

        auto slot = slot_for(plugin);
        uint32_t seen_w = 0, seen_h = 0;
        slot->set_editor_resize_request_handler([&](uint32_t w, uint32_t h) {
            seen_w = w;
            seen_h = h;
            return true;
        });

        auto editor = slot->create_hosted_editor(win.handle());
        REQUIRE(editor != nullptr);
        REQUIRE(seen_w == 512);
        REQUIRE(seen_h == 288);
        REQUIRE(view.resize_result == kResultTrue);

        slot->destroy_hosted_editor(std::move(editor));
    }
}

TEST_CASE("VST3 editor create fails cleanly when the plug-in refuses to attach",
          "[host][vst3][editor][hosted-editor]") {
    @autoreleasepool {
        SelfSizingView view;
        view.refuse_attach = true;
        CountingCombined plugin;
        plugin.view = &view;
        TestWindow win;

        auto slot = slot_for(plugin);
        REQUIRE(slot->create_hosted_editor(win.handle()) == nullptr);

        // Nothing left parented, and the slot did not keep the failed view —
        // a second attempt must not report "already open".
        REQUIRE([[win.content() subviews] count] == 0u);
        view.refuse_attach = false;
        auto editor = slot->create_hosted_editor(win.handle());
        REQUIRE(editor != nullptr);
        slot->destroy_hosted_editor(std::move(editor));
    }
}

TEST_CASE("VST3 combined plug-in is terminated exactly once",
          "[host][vst3][hosted-editor][connection]") {
    @autoreleasepool {
        SelfSizingView view;
        CountingCombined plugin;
        plugin.view = &view;

        { auto slot = slot_for(plugin); }  // destroyed here

        // Both halves are one object. Classifying it as "separated" — which a
        // cast-and-compare on FUnknown* does — terminates it twice.
        REQUIRE(plugin.terminate_calls == 1);
    }
}
