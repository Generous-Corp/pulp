// test_au_v3_view_controller.mm - the macOS AU v3 view controller opening a
// contract editor for a processor that supplies an editor factory.
//
// A real PulpAudioUnit is built over a processor from the registry, and the
// controller (the principal class an extension loads) is bound to it exactly
// as the extension does. The editor records every contract call, so each test
// asserts what the controller's view hierarchy, preferredContentSize and
// teardown became in the contract. A processor without a factory keeps the
// ViewBridge path.

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>

#import "../core/format/src/au_audio_unit.h"
#include "support/recording_contract_editor.hpp"

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/state/store.hpp>

using pulp::test_support::contract_log;
using pulp::test_support::EditorEnvironment;
using pulp::test_support::RecordingContractFactory;
using pulp::test_support::reset_contract_recording;

namespace {

constexpr pulp::state::ParamID kGain = 1;
bool g_has_factory = true;

class ContractProcessor final : public pulp::format::Processor {
  public:
    ContractProcessor() : has_factory_(g_has_factory) {}
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Contract AU v3 controller";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.au-v3-view-controller";
        d.version = "1.0.0";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
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

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<ContractProcessor>();
}

struct ScopedFactoryRegistration {
    explicit ScopedFactoryRegistration(pulp::format::ProcessorFactory factory)
        : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(factory);
    }
    ~ScopedFactoryRegistration() {
        pulp::format::register_plugin(previous);
    }
    pulp::format::ProcessorFactory previous;
};

PulpAudioUnit* make_unit() {
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Effect;
    desc.componentSubType = 'TstE';
    desc.componentManufacturer = 'Plup';
    NSError* error = nil;
    PulpAudioUnit* unit = [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                                      options:0
                                                                        error:&error];
    REQUIRE(unit != nil);
    return unit;
}

/// The extension's principal class, bound to `unit` the way the extension does.
NSViewController* make_controller(PulpAudioUnit* unit) {
    Class cls = NSClassFromString(@"PulpAUMacViewController");
    REQUIRE(cls != nil);
    NSViewController* controller = [[cls alloc] init];
    (void)controller.view; // loadView + viewDidLoad
    [controller setValue:unit forKey:@"audioUnit"];
    return controller;
}

NSView* contract_view_of(NSViewController* controller) {
    for (NSView* v in controller.view.subviews)
        if ([NSStringFromClass([v class]) isEqualToString:@"PulpAUv3ContractView"])
            return v;
    return nil;
}

NSWindow* make_window() {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 800, 600)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    return window;
}

} // namespace

TEST_CASE("the AU v3 view controller opens, resizes and closes a contract editor",
          "[auv3][editor-host][au-v3-contract][controller]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_has_factory = true;
    ScopedFactoryRegistration registration(create_processor);
    NSWindow* window = make_window();

    PulpAudioUnit* unit = nil;
    NSViewController* controller = nil;
    // Messages to the controller autorelease it into the pool they run in, so
    // everything that touches it drains before the final release.
    @autoreleasepool {
        unit = make_unit();
        controller = make_controller(unit);
        NSView* view = contract_view_of(controller);
        REQUIRE(view != nil);

        // Attached into the contract view, which fills the controller's view
        // at the editor's size, published as preferredContentSize.
        REQUIRE(contract_log().saw("attach:cocoa_view"));
        REQUIRE(contract_log().parent_handle == (__bridge void*)view);
        REQUIRE(controller.preferredContentSize.width == 400);
        REQUIRE(controller.preferredContentSize.height == 300);
        REQUIRE(controller.view.frame.size.width == 400);
        REQUIRE(controller.view.frame.size.height == 300);
        REQUIRE(view.frame.size.width == 400);
        REQUIRE(view.frame.size.height == 300);
        REQUIRE([unit pulpContractSessions]->size() == 1);

        [window.contentView addSubview:controller.view];
        REQUIRE(contract_log().saw("visible:on"));

        // The host resizes the controller's view; the editor follows.
        [controller.view setFrameSize:NSMakeSize(640, 480)];
        REQUIRE(view.frame.size.width == 640);
        REQUIRE(contract_log().saw("set_size:640x480"));

        // The editor asks for a size: preferredContentSize and the controller's
        // view move, and the editor is not echoed its own request.
        const auto calls = contract_log().calls.size();
        REQUIRE(contract_log().context->request_resize({720, 540, 1.0}));
        REQUIRE(controller.preferredContentSize.width == 720);
        REQUIRE(controller.preferredContentSize.height == 540);
        REQUIRE(controller.view.frame.size.width == 720);
        REQUIRE(view.frame.size.width == 720);
        REQUIRE(contract_log().calls.size() == calls);

        contract_log().context->params().begin(kGain);
        REQUIRE([unit pulpStore]->open_gesture_count() == 1);

        [controller.view removeFromSuperview];
    }
    // Binding the controller to another audio unit rebuilds its editor: the
    // contract editor closes in order, here for a processor with no factory.
    g_has_factory = false;
    PulpAudioUnit* other = nil;
    @autoreleasepool {
        other = make_unit();
        [controller setValue:other forKey:@"audioUnit"];
        REQUIRE(contract_view_of(controller) == nil);
    }
    g_has_factory = true;
    REQUIRE(contract_log().saw("detach"));
    REQUIRE_FALSE(contract_log().saw("invalidate"));
    REQUIRE(contract_log().live == 0);
    REQUIRE([unit pulpStore]->open_gesture_count() == 0);
    REQUIRE([unit pulpContractSessions]->size() == 0);
    [unit release];
    [other release];
    [controller release];
    [window close];
    [window release];
}

TEST_CASE("an audio unit released by its controller closes the contract editor in order",
          "[auv3][editor-host][au-v3-contract][controller]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_has_factory = true;
    ScopedFactoryRegistration registration(create_processor);

    PulpAudioUnit* unit = nil;
    NSViewController* controller = nil;
    @autoreleasepool {
        unit = make_unit();
        controller = make_controller(unit);
        REQUIRE(contract_view_of(controller) != nil);
        contract_log().context->params().begin(kGain);
        REQUIRE([unit pulpStore]->open_gesture_count() == 1);
    }
    // The controller retains the unit, so the host's release leaves it up.
    [unit release];
    REQUIRE(contract_log().live == 1);

    // The controller lets go of the unit while its view is still on screen: the
    // unit's registry closes the editor in order, never invalidating it, and
    // the view that remains touches nothing that is gone.
    @autoreleasepool {
        [controller setValue:nil forKey:@"audioUnit"];
        REQUIRE(contract_log().saw("detach"));
        REQUIRE_FALSE(contract_log().saw("invalidate"));
        REQUIRE(contract_log().live == 0);
        const auto calls = contract_log().calls.size();
        [controller.view setFrameSize:NSMakeSize(500, 400)];
        REQUIRE(contract_log().calls.size() == calls);
    }
    [controller release];
}

TEST_CASE("a processor without an editor factory gets no contract view from the controller",
          "[auv3][editor-host][au-v3-contract][controller]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_has_factory = false;
    ScopedFactoryRegistration registration(create_processor);

    @autoreleasepool {
        PulpAudioUnit* unit = make_unit();
        NSViewController* controller = make_controller(unit);
        REQUIRE(contract_view_of(controller) == nil);
        REQUIRE(contract_log().live == 0);
        REQUIRE([unit pulpContractSessions]->size() == 0);
        [controller release];
        [unit release];
    }
    g_has_factory = true;
}
