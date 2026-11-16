// test_au_v3_contract_view.mm - the AU v3 contract view
// (au_v3_contract_view.mm) for a processor that supplies an editor factory.
//
// The view module is compiled into this test with a test-unique class name.
// The fake instance holds what an AU v3 audio unit holds for its editors, in
// the same order: the store, the processor, then the session registry. The
// editor records every contract call.

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

#include "../core/format/src/au_v3_contract_view.h"
#include "support/recording_contract_editor.hpp"

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <memory>
#include <utility>
#include <vector>

using pulp::test_support::contract_config;
using pulp::test_support::contract_log;
using pulp::test_support::EditorEnvironment;
using pulp::test_support::MainLoop;
using pulp::test_support::RecordingContractFactory;
using pulp::test_support::reset_contract_recording;

// A window whose backing scale a test changes.
@interface PulpAUv3TestWindow : NSWindow
@property(nonatomic) CGFloat scale;
@end
@implementation PulpAUv3TestWindow
- (CGFloat)backingScaleFactor {
    return _scale > 0 ? _scale : 1.0;
}
@end

namespace {

constexpr pulp::state::ParamID kGain = 1;

class ContractProcessor final : public pulp::format::Processor {
  public:
    explicit ContractProcessor(bool has_factory) : has_factory_(has_factory) {}
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Contract AU v3";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.au-v3-contract-view";
        d.version = "1.0.0";
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo info;
        info.id = kGain;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(info);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
    pulp::format::EditorFactory* editor_factory() override {
        return has_factory_ ? &factory_ : nullptr;
    }

  private:
    bool has_factory_;
    RecordingContractFactory factory_;
};

struct FakeInstance {
    pulp::state::StateStore store;
    std::unique_ptr<ContractProcessor> processor;
    std::unique_ptr<pulp::format::ContractSessionRegistry> sessions =
        std::make_unique<pulp::format::ContractSessionRegistry>();

    explicit FakeInstance(bool has_factory)
        : processor(std::make_unique<ContractProcessor>(has_factory)) {
        processor->set_state_store(&store);
        processor->define_parameters(store);
    }
};

/// The sizes the view asked the host to resize to; the host follows or not.
struct ResizeLog {
    std::vector<CGSize> requests;
    BOOL follow = YES;
    // The host's container for the view; it follows an accepted request.
    NSView* container = nil;
};

PULP_AUV3_CONTRACT_VIEW_CLASS* open_view(FakeInstance& instance, ResizeLog* log = nullptr) {
    return [PULP_AUV3_CONTRACT_VIEW_CLASS
        openViewForProcessor:instance.processor.get()
                       store:&instance.store
                    registry:instance.sessions.get()
               resizeRequest:^BOOL(CGSize size) {
                 if (log == nullptr)
                     return YES;
                 log->requests.push_back(size);
                 if (log->follow && log->container != nil)
                     [log->container setFrameSize:NSSizeFromCGSize(size)];
                 return log->follow;
               }];
}

NSWindow* make_window() {
    NSWindow* window =
        [[PulpAUv3TestWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 800, 600)
                                              styleMask:NSWindowStyleMaskBorderless
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    window.releasedWhenClosed = NO;
    return window;
}

NSEvent* key_event(NSEventType type, unsigned short code, NSString* chars) {
    return [NSEvent keyEventWithType:type
                            location:NSZeroPoint
                       modifierFlags:0
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:chars
         charactersIgnoringModifiers:chars
                           isARepeat:NO
                             keyCode:code];
}

} // namespace

TEST_CASE("AU v3 opens, sizes and closes a contract editor, twice",
          "[auv3][editor-host][au-v3-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    FakeInstance instance(true);
    ResizeLog resizes;
    NSWindow* window = make_window();
    resizes.container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];

    @autoreleasepool {
        PULP_AUV3_CONTRACT_VIEW_CLASS* view = [open_view(instance, &resizes) retain];
        REQUIRE(view != nil);
        // Sized to the editor, which is attached into this view.
        REQUIRE(view.frame.size.width == 400);
        REQUIRE(view.frame.size.height == 300);
        REQUIRE(contract_log().saw("attach:cocoa_view"));
        REQUIRE(contract_log().parent_handle == (__bridge void*)view);
        REQUIRE(instance.sessions->size() == 1);

        [resizes.container addSubview:view];
        [window.contentView addSubview:resizes.container];
        REQUIRE(contract_log().saw("visible:on"));

        // The host resizes the view: the editor follows.
        [view setFrameSize:NSMakeSize(640, 480)];
        REQUIRE(contract_log().saw("set_size:640x480"));
        REQUIRE(resizes.requests.empty());

        // A host size outside the editor's limits is taken at the nearest size
        // the editor accepts, and that size is published to the host, which
        // follows: the view ends inside the limits instead of at 100x100.
        [view setFrameSize:NSMakeSize(100, 100)];
        REQUIRE(contract_log().saw("set_size:200x150"));
        // Never from inside the host's resize call: the ask comes once the
        // host's sizes stop changing.
        REQUIRE(resizes.requests.empty());
        MainLoop loop;
        REQUIRE(loop.run_until([&] { return resizes.requests.size() == 1; }));
        REQUIRE(resizes.requests[0].width == 200);
        REQUIRE(resizes.requests[0].height == 150);
        REQUIRE(view.frame.size.width == 200);
        REQUIRE(view.frame.size.height == 150);

        // A backing scale change reaches the editor's size. The scale the view
        // pushed on joining the window was 1.
        REQUIRE(contract_log().last_scale == 1.0);
        static_cast<PulpAUv3TestWindow*>(window).scale = 3.0;
        [view viewDidChangeBackingProperties];
        REQUIRE(contract_log().last_scale == 3.0);

        // An editor resize request goes to the controller's block, and the
        // view follows once the host accepts. The editor is not told about the
        // size it asked for.
        const auto before = contract_log().calls.size();
        REQUIRE(contract_log().context->request_resize({720, 540, 1.0}));
        REQUIRE(resizes.requests.size() == 2);
        REQUIRE(resizes.requests[1].width == 720);
        REQUIRE(resizes.requests[1].height == 540);
        REQUIRE(view.frame.size.width == 720);
        REQUIRE(view.frame.size.height == 540);
        REQUIRE(contract_log().calls.size() == before);

        // Space is offered and continues to the host; any other key is taken.
        [view keyDown:key_event(NSEventTypeKeyDown, 49, @" ")];
        REQUIRE(contract_log().saw("offer:space"));
        [view keyDown:key_event(NSEventTypeKeyDown, 0, @"a")];
        REQUIRE(contract_log().saw("offer:char"));

        // A drag through the port, then the host closes the editor mid-drag.
        auto& port = contract_log().context->params();
        port.begin(kGain);
        port.set(kGain, 0.75f);
        REQUIRE(instance.store.open_gesture_count() == 1);
        [resizes.container removeFromSuperview];
        [view removeFromSuperview];
        REQUIRE(contract_log().saw("visible:off"));
        [view closeEditor];
        REQUIRE(contract_log().saw("detach"));
        REQUIRE(instance.store.open_gesture_count() == 0);
        REQUIRE([view session] == nullptr);
        [view release];
    }
    REQUIRE_FALSE(contract_log().saw("invalidate"));
    REQUIRE(contract_log().live == 0);
    REQUIRE(instance.sessions->size() == 0);

    // Reopen: a fresh editor reads the value the first one set.
    contract_log().calls.clear();
    @autoreleasepool {
        NSView* view = [open_view(instance) retain];
        REQUIRE(view != nil);
        REQUIRE(contract_log().context->params().get(kGain) == 0.75f);
        [view release];
    }
    REQUIRE(contract_log().saw("detach"));
    REQUIRE(contract_log().live == 0);
    [window close];
    [window release];
}

TEST_CASE("an AU v3 editor resize the host does not follow is refused",
          "[auv3][editor-host][au-v3-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    FakeInstance instance(true);
    ResizeLog resizes;
    resizes.follow = NO;

    @autoreleasepool {
        NSView* view = [open_view(instance, &resizes) retain];
        REQUIRE(view != nil);
        REQUIRE_FALSE(contract_log().context->request_resize({720, 540, 1.0}));
        REQUIRE(resizes.requests.size() == 1);
        REQUIRE(view.frame.size.width == 400);
        REQUIRE(view.frame.size.height == 300);
        // Never a size the editor cannot lay out.
        REQUIRE_FALSE(contract_log().context->request_resize({0, 0, 1.0}));
        REQUIRE(resizes.requests.size() == 1);
        [view release];
    }
    REQUIRE(contract_log().live == 0);
}

namespace {
// Ten host sizes outside the editor's limits, 16 ms apart, as a live drag
// delivers them. Returns the requests seen during the burst and after it.
std::pair<size_t, size_t> drag_burst(NSInteger settle_ms) {
    FakeInstance instance(true);
    ResizeLog resizes;
    resizes.container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    MainLoop loop;
    size_t during = 0, after = 0;
    @autoreleasepool {
        PULP_AUV3_CONTRACT_VIEW_CLASS* view = [open_view(instance, &resizes) retain];
        view.askBackDelayMs = settle_ms;
        [resizes.container addSubview:view];
        for (int i = 0; i < 10; ++i) {
            [view setFrameSize:NSMakeSize(100 + i, 100 + i)];
            loop.run_for(0.016);
        }
        during = resizes.requests.size();
        loop.run_for(0.4);
        after = resizes.requests.size() - during;
        [view release];
    }
    [resizes.container release];
    return {during, after};
}
} // namespace

TEST_CASE("AU v3 asks the host for the editor's limits only after a drag settles",
          "[auv3][editor-host][au-v3-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto settled = drag_burst(120);
    REQUIRE(settled.first == 0);
    REQUIRE(settled.second == 1);
    // Control: with no settle time every size asks, so the burst is not quiet
    // for a reason other than the delay.
    reset_contract_recording();
    const auto immediate = drag_burst(0);
    REQUIRE(immediate.first > 0);
}

TEST_CASE("an accepted AU v3 resize request never leaves the view different from its parent",
          "[auv3][editor-host][au-v3-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    FakeInstance instance(true);
    ResizeLog resizes; // accepts every request, but the parent does not change
    NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];

    @autoreleasepool {
        PULP_AUV3_CONTRACT_VIEW_CLASS* view = [open_view(instance, &resizes) retain];
        REQUIRE(view != nil);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [parent addSubview:view];
        REQUIRE(contract_log().context->request_resize({720, 540, 1.0}));
        // The host has not followed, so the view still fills its parent: it
        // neither overflows nor waits for a size the parent will not give.
        REQUIRE(view.frame.size.width == 400);
        REQUIRE(view.frame.size.height == 300);
        // The host follows: the parent grows and the flexible view fills it
        // exactly, with no second step from the request.
        [parent setFrameSize:NSMakeSize(720, 540)];
        REQUIRE(view.frame.size.width == 720);
        REQUIRE(view.frame.size.height == 540);
        [view release];
    }
    [parent release];
}

TEST_CASE("an AU v3 contract editor ticks only while its view lives",
          "[auv3][editor-host][au-v3-contract][idle]") {
    EditorEnvironment env;
    reset_contract_recording();
    FakeInstance instance(true);
    MainLoop loop;

    @autoreleasepool {
        NSView* view = [open_view(instance) retain];
        REQUIRE(view != nil);
        REQUIRE(loop.run_until([] { return contract_log().idles >= 3; }));
        [view release];
    }
    const int at_release = contract_log().idles;
    loop.run_for(0.1);
    REQUIRE(contract_log().idles == at_release);
}

TEST_CASE("closing the AU v3 instance closes a contract editor its view still holds",
          "[auv3][editor-host][au-v3-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    auto instance = std::make_unique<FakeInstance>(true);
    MainLoop loop;

    @autoreleasepool {
        PULP_AUV3_CONTRACT_VIEW_CLASS* view = [open_view(*instance) retain];
        REQUIRE(view != nil);
        contract_log().context->params().begin(kGain);
        REQUIRE(instance->store.open_gesture_count() == 1);

        // The audio unit's registry goes before its processor. The host still
        // holds the view, so the editor detaches in order, never invalidated.
        instance->sessions.reset();
        REQUIRE(contract_log().saw("detach"));
        REQUIRE_FALSE(contract_log().saw("invalidate"));
        REQUIRE(contract_log().live == 0);
        REQUIRE(instance->store.open_gesture_count() == 0);
        instance.reset();

        // The view outlives its instance: resizing, ticking, closing and
        // releasing it touch nothing that is gone.
        const auto calls = contract_log().calls.size();
        [view setFrameSize:NSMakeSize(500, 400)];
        loop.run_for(0.1);
        REQUIRE(contract_log().idles == 0);
        [view closeEditor];
        [view release];
        REQUIRE(contract_log().calls.size() == calls);
    }
}

TEST_CASE("AU v3 serves no contract view without a factory, an editor or a parent",
          "[auv3][editor-host][au-v3-contract]") {
    reset_contract_recording();
    {
        EditorEnvironment env;
        FakeInstance instance(false);
        @autoreleasepool {
            REQUIRE(open_view(instance) == nil);
        }
    }
    {
        EditorEnvironment env;
        FakeInstance instance(true);
        contract_config().factory_builds_editor = false;
        @autoreleasepool {
            REQUIRE(open_view(instance) == nil);
        }
        contract_config() = {};
        contract_config().attach_succeeds = false;
        @autoreleasepool {
            REQUIRE(open_view(instance) == nil);
        }
        REQUIRE(contract_log().live == 0);
        REQUIRE(instance.sessions->size() == 0);
    }
    @autoreleasepool {
        REQUIRE([PULP_AUV3_CONTRACT_VIEW_CLASS openViewForProcessor:nullptr
                                                              store:nullptr
                                                           registry:nullptr
                                                      resizeRequest:nil] == nil);
    }
}
