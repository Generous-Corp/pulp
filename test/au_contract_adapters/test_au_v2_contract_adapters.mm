// test_au_v2_contract_adapters.mm — every AU v2 adapter type (effect,
// instrument, MIDI processor) hands its contract session registry to the
// contract view, so disposing the unit closes an editor its view still holds
// in order (detached, never invalidated, since the view is still alive).
//
// Each adapter is built from its real entry macro (one per translation unit),
// registered with the Audio Component manager for this process only, and
// instantiated. The contract view module is compiled in with test-unique class
// names.

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include "contract_processor.hpp"

#include <pulp/format/au_v2_common.hpp>

extern "C" {
void* PulpTestContractFxFactory(const AudioComponentDescription*);
void* PulpTestContractSynthFactory(const AudioComponentDescription*);
void* PulpTestContractMidiFactory(const AudioComponentDescription*);
}

using pulp::test_support::contract_log;
using pulp::test_support::EditorEnvironment;
using pulp::test_support::reset_contract_recording;

namespace {

AudioComponent register_adapter(OSType type, OSType subtype,
                                AudioComponentFactoryFunction factory) {
    AudioComponentDescription desc{type, subtype, 'PlTa', 0, 0};
    return AudioComponentRegister(&desc, CFSTR("Pulp: contract adapter test"), 1, factory);
}

void check_adapter(AudioComponent comp) {
    REQUIRE(comp != nullptr);
    AudioUnit au = nullptr;
    REQUIRE(AudioComponentInstanceNew(comp, &au) == noErr);

    // The adapter hands out its session registry with the editor context.
    pulp::format::au::PulpEditorContext ctx{};
    UInt32 size = sizeof(ctx);
    REQUIRE(AudioUnitGetProperty(au, pulp::format::au::kPulpEditorContextProperty,
                                 kAudioUnitScope_Global, 0, &ctx, &size) == noErr);
    REQUIRE(ctx.processor != nullptr);
    REQUIRE(ctx.contract_sessions != nullptr);

    // It advertises the contract view, and the view opens.
    AudioUnitCocoaViewInfo info{};
    size = sizeof(info);
    REQUIRE(AudioUnitGetProperty(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &info,
                                 &size) == noErr);
    NSString* class_name = (__bridge NSString*)info.mCocoaAUViewClass[0];
    REQUIRE([class_name isEqualToString:@"PulpAUContractViewFactory_Adapters"]);
    Class factory_class = NSClassFromString(class_name);
    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);

    NSView* view = nil;
    @autoreleasepool {
        id<AUCocoaUIBase> factory = [[factory_class alloc] init];
        view = [[factory uiViewForAudioUnit:au withSize:NSMakeSize(400, 300)] retain];
        [factory release];
    }
    REQUIRE(view != nil);
    REQUIRE(contract_log().live == 1);
    contract_log().context->params().begin(pulp::test_fixtures::kContractGain);
    REQUIRE(ctx.store->open_gesture_count() == 1);

    // Disposed while the view lives: the adapter closes the editor in order
    // (gestures released, detached) before its processor goes.
    AudioComponentInstanceDispose(au);
    REQUIRE(contract_log().saw("detach"));
    REQUIRE_FALSE(contract_log().saw("invalidate"));
    REQUIRE(contract_log().live == 0);
    @autoreleasepool {
        [view release];
    }
}

} // namespace

TEST_CASE("the AU v2 effect adapter closes its contract editor on dispose",
          "[auv2][editor-host][au-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    check_adapter(register_adapter(
        kAudioUnitType_Effect, 'CtFx',
        reinterpret_cast<AudioComponentFactoryFunction>(PulpTestContractFxFactory)));
}

TEST_CASE("the AU v2 instrument adapter closes its contract editor on dispose",
          "[auv2][editor-host][au-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    check_adapter(register_adapter(
        kAudioUnitType_MusicDevice, 'CtSy',
        reinterpret_cast<AudioComponentFactoryFunction>(PulpTestContractSynthFactory)));
}

TEST_CASE("the AU v2 MIDI processor adapter closes its contract editor on dispose",
          "[auv2][editor-host][au-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    check_adapter(register_adapter(
        kAudioUnitType_MIDIProcessor, 'CtMi',
        reinterpret_cast<AudioComponentFactoryFunction>(PulpTestContractMidiFactory)));
}
