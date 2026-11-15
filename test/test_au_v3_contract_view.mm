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
#include <vector>

using pulp::test_support::contract_config;
using pulp::test_support::contract_log;
using pulp::test_support::EditorEnvironment;
using pulp::test_support::MainLoop;
using pulp::test_support::RecordingContractFactory;
using pulp::test_support::reset_contract_recording;

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
};

PULP_AUV3_CONTRACT_VIEW_CLASS* open_view(FakeInstance& instance, ResizeLog* log = nullptr) {
    return [PULP_AUV3_CONTRACT_VIEW_CLASS openViewForProcessor:instance.processor.get()
                                                         store:&instance.store
                                                      registry:instance.sessions.get()
                                                 resizeRequest:^BOOL(CGSize size) {
                                                   if (log == nullptr)
                                                       return YES;
                                                   log->requests.push_back(size);
                                                   return log->follow;
                                                 }];
}

NSWindow* make_window() {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 800, 600)
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

    @autoreleasepool {
        PULP_AUV3_CONTRACT_VIEW_CLASS* view = [open_view(instance, &resizes) retain];
        REQUIRE(view != nil);
        // Sized to the editor, which is attached into this view.
        REQUIRE(view.frame.size.width == 400);
        REQUIRE(view.frame.size.height == 300);
        REQUIRE(contract_log().saw("attach:cocoa_view"));
        REQUIRE(contract_log().parent_handle == (__bridge void*)view);
        REQUIRE(instance.sessions->size() == 1);

        [window.contentView addSubview:view];
        REQUIRE(contract_log().saw("visible:on"));

        // The host resizes the view; an undersized frame is refused by the
        // editor, which keeps its size.
        [view setFrameSize:NSMakeSize(640, 480)];
        REQUIRE(contract_log().saw("set_size:640x480"));
        const auto calls = contract_log().calls.size();
        [view setFrameSize:NSMakeSize(100, 100)];
        REQUIRE(contract_log().calls.size() == calls);

        // The window's backing scale reaches the editor's size.
        [view viewDidChangeBackingProperties];
        REQUIRE(contract_log().last_scale == window.backingScaleFactor);

        // An editor resize request goes to the controller's block, and the
        // view follows once the host accepts. The editor is not told about the
        // size it asked for.
        const auto before = contract_log().calls.size();
        REQUIRE(contract_log().context->request_resize({720, 540, 1.0}));
        REQUIRE(resizes.requests.size() == 1);
        REQUIRE(resizes.requests[0].width == 720);
        REQUIRE(resizes.requests[0].height == 540);
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
