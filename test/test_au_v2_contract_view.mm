// test_au_v2_contract_view.mm — the AU v2 Cocoa view factory for a processor
// that supplies an editor factory (au_v2_contract_view.mm).
//
// The view module is compiled into this test with test-unique class names. A
// fake Audio Unit answers only the private editor-context property, which is
// all the factory reads, and carries a ContractSessionRegistry the way the AU
// adapters do. The editor records every contract call.

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include "support/recording_contract_editor.hpp"

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <pulp/state/store.hpp>

#include <atomic>
#include <memory>
#include <thread>

namespace pulp::format::au {
EditorKeyEvent contract_key_event(unsigned short key_code, NSString* characters,
                                  NSEventModifierFlags flags, bool is_down, bool is_repeat);
}

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
        d.name = "Contract AU";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.au-contract-view";
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

// What an AU adapter instance holds for its editors, declared in the same
// order: the store, the processor, then the session registry.
struct FakeInstance {
    pulp::state::StateStore store;
    std::unique_ptr<ContractProcessor> processor;
    pulp::runtime::AliveToken alive;
    std::unique_ptr<pulp::format::ContractSessionRegistry> sessions =
        std::make_unique<pulp::format::ContractSessionRegistry>();

    explicit FakeInstance(bool has_factory)
        : processor(std::make_unique<ContractProcessor>(has_factory)) {
        processor->set_state_store(&store);
        processor->define_parameters(store);
    }
};
FakeInstance* g_instance = nullptr;

struct FakeAU {
    AudioComponentPlugInInterface iface{};
};

OSStatus fake_get_property(void*, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement,
                           void* out, UInt32* io_size) {
    if (id != pulp::format::au::kPulpEditorContextProperty || !out || !io_size ||
        *io_size < sizeof(pulp::format::au::PulpEditorContext) || g_instance == nullptr)
        return kAudioUnitErr_InvalidProperty;
    auto* ctx = static_cast<pulp::format::au::PulpEditorContext*>(out);
    ctx->processor = g_instance->processor.get();
    ctx->store = &g_instance->store;
    ctx->owner_alive = g_instance->alive.capture();
    ctx->contract_sessions = g_instance->sessions.get();
    *io_size = sizeof(*ctx);
    return noErr;
}
OSStatus fake_open(void*, AudioComponentInstance) {
    return noErr;
}
OSStatus fake_close(void* self) {
    delete static_cast<FakeAU*>(self);
    return noErr;
}
AudioComponentMethod fake_lookup(SInt16 selector) {
    if (selector == kAudioUnitGetPropertySelect)
        return reinterpret_cast<AudioComponentMethod>(&fake_get_property);
    return nullptr;
}
AudioComponentPlugInInterface* fake_factory(const AudioComponentDescription*) {
    auto* au = new FakeAU;
    au->iface.Open = fake_open;
    au->iface.Close = fake_close;
    au->iface.Lookup = fake_lookup;
    return &au->iface;
}

AudioUnit fake_audio_unit() {
    static AudioComponent comp = [] {
        AudioComponentDescription desc{kAudioUnitType_Effect, 'CtVw', 'PlTs', 0, 0};
        return AudioComponentRegister(&desc, CFSTR("Pulp: contract view test"), 1, fake_factory);
    }();
    AudioUnit au = nullptr;
    REQUIRE(comp != nullptr);
    REQUIRE(AudioComponentInstanceNew(comp, &au) == noErr);
    return au;
}

NSView* open_view(AudioUnit au) {
    Class factory_class = NSClassFromString(@"PulpAUContractViewFactory_Test");
    REQUIRE(factory_class != nil);
    id<AUCocoaUIBase> factory = [[factory_class alloc] init];
    NSView* view = [factory uiViewForAudioUnit:au withSize:NSMakeSize(480, 320)];
    [factory release];
    return view;
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

TEST_CASE("AU v2 advertises the contract view factory under its unique name",
          "[auv2][editor-host][au-contract]") {
    using namespace pulp::format::au;
    REQUIRE(g_cocoa_view_info_filler != nullptr);
    AudioUnitCocoaViewInfo info{};
    REQUIRE(g_cocoa_view_info_filler(&info));
    REQUIRE(info.mCocoaAUViewBundleLocation != nullptr);
    NSString* name = (__bridge NSString*)info.mCocoaAUViewClass[0];
    REQUIRE([name isEqualToString:@"PulpAUContractViewFactory_Test"]);
    Class cls = NSClassFromString(name);
    REQUIRE(cls != nil);
    REQUIRE([cls conformsToProtocol:@protocol(AUCocoaUIBase)]);
    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);
}

TEST_CASE("AU v2 opens, sizes and closes a contract editor, twice",
          "[auv2][editor-host][au-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    FakeInstance instance(true);
    g_instance = &instance;
    AudioUnit au = fake_audio_unit();
    NSWindow* window = make_window();

    @autoreleasepool {
        NSView* view = [open_view(au) retain];
        REQUIRE(view != nil);
        // Sized to the editor, which is attached into this container.
        REQUIRE(view.frame.size.width == 400);
        REQUIRE(view.frame.size.height == 300);
        REQUIRE(contract_log().saw("attach:cocoa_view"));
        REQUIRE(contract_log().parent_handle == (__bridge void*)view);
        REQUIRE(instance.sessions->size() == 1);
        // An in-process host reaches the open editor through the view's
        // session; any other view has none.
        auto* session = pulp::format::au::contract_view_session((__bridge void*)view);
        REQUIRE(session != nullptr);
        REQUIRE(session->editor() != nullptr);
        NSView* other = [[NSView alloc] initWithFrame:NSZeroRect];
        REQUIRE(pulp::format::au::contract_view_session((__bridge void*)other) == nullptr);
        [other release];
        REQUIRE(pulp::format::au::contract_view_session(nullptr) == nullptr);

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

        // An editor resize request moves the container's frame.
        REQUIRE(contract_log().context->request_resize({720, 540, 1.0}));
        REQUIRE(view.frame.size.width == 720);
        REQUIRE(view.frame.size.height == 540);
        REQUIRE_FALSE(contract_log().context->host_action({}));

        // Keys reaching the container: Space is offered and continues to the
        // host; a key the editor consumes stops here.
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
        [view release];
    }
    REQUIRE(contract_log().saw("detach"));
    REQUIRE_FALSE(contract_log().saw("invalidate"));
    REQUIRE(contract_log().live == 0);
    REQUIRE(instance.store.open_gesture_count() == 0);
    REQUIRE(instance.sessions->size() == 0);

    // Reopen: a fresh editor reads the value the first one set.
    contract_log().calls.clear();
    @autoreleasepool {
        NSView* view = [open_view(au) retain];
        REQUIRE(view != nil);
        REQUIRE(contract_log().context->params().get(kGain) == 0.75f);
        [view release];
    }
    REQUIRE(contract_log().live == 0);

    [window close];
    [window release];
    AudioComponentInstanceDispose(au);
    g_instance = nullptr;
}

TEST_CASE("an AU v2 contract editor ticks only while its view lives",
          "[auv2][editor-host][au-contract][idle]") {
    EditorEnvironment env;
    reset_contract_recording();
    FakeInstance instance(true);
    g_instance = &instance;
    AudioUnit au = fake_audio_unit();
    MainLoop loop;

    @autoreleasepool {
        NSView* view = [open_view(au) retain];
        REQUIRE(view != nil);
        REQUIRE(loop.run_until([] { return contract_log().idles >= 3; }));
        [view release];
    }
    const int at_release = contract_log().idles;
    loop.run_for(0.1);
    REQUIRE(contract_log().idles == at_release);

    AudioComponentInstanceDispose(au);
    g_instance = nullptr;
}

TEST_CASE("disposing the AU instance closes a contract editor its view still holds",
          "[auv2][editor-host][au-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    auto instance = std::make_unique<FakeInstance>(true);
    g_instance = instance.get();
    AudioUnit au = fake_audio_unit();
    MainLoop loop;

    @autoreleasepool {
        NSView* view = [open_view(au) retain];
        REQUIRE(view != nil);
        contract_log().context->params().begin(kGain);
        REQUIRE(instance->store.open_gesture_count() == 1);

        // The adapter's registry goes before its processor (member order).
        // The host still holds the view, so the editor detaches from it in
        // order rather than being invalidated.
        instance->sessions.reset();
        REQUIRE(contract_log().saw("detach"));
        REQUIRE_FALSE(contract_log().saw("invalidate"));
        REQUIRE(contract_log().live == 0);
        REQUIRE(instance->store.open_gesture_count() == 0);
        instance.reset();
        g_instance = nullptr;

        // The view outlives its instance: resizing, ticking and releasing it
        // touch nothing that is gone, and the editor is not detached twice.
        const auto calls = contract_log().calls.size();
        [view setFrameSize:NSMakeSize(500, 400)];
        loop.run_for(0.1);
        REQUIRE(contract_log().idles == 0);
        [view release];
        REQUIRE(contract_log().calls.size() == calls);
    }
    AudioComponentInstanceDispose(au);
}

TEST_CASE("disposing the AU instance off the main thread cuts the store at once and closes "
          "the editor on the main thread later",
          "[auv2][editor-host][au-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    auto instance = std::make_unique<FakeInstance>(true);
    g_instance = instance.get();
    AudioUnit au = fake_audio_unit();
    MainLoop loop;

    @autoreleasepool {
        NSView* view = [open_view(au) retain];
        REQUIRE(view != nil);
        auto& params = contract_log().context->params();
        params.begin(kGain);
        REQUIRE(instance->store.open_gesture_count() == 1);

        // The disposing thread does not wait for the main thread. If it did,
        // the timed wait below gives up, and the main loop then lets it finish
        // so it can be joined before the test fails.
        dispatch_semaphore_t disposed = dispatch_semaphore_create(0);
        std::thread disposer([&] {
            instance->sessions.reset();
            dispatch_semaphore_signal(disposed);
        });
        const bool finished =
            dispatch_semaphore_wait(
                disposed,
                dispatch_time(DISPATCH_TIME_NOW, 3 * static_cast<int64_t>(NSEC_PER_SEC))) == 0;
        if (!finished)
            loop.run_until([] { return contract_log().live == 0; });
        disposer.join();
        dispatch_release(disposed);
        REQUIRE(finished);

        // Done on the disposing thread: every gesture released and the port
        // cut from the store, so the store may go before the editor closes.
        REQUIRE(instance->store.open_gesture_count() == 0);
        REQUIRE(params.params().empty());
        params.begin(kGain);
        REQUIRE(instance->store.open_gesture_count() == 0);
        // The editor itself is still open: it closes on the main thread.
        REQUIRE_FALSE(contract_log().saw("detach"));
        REQUIRE(contract_log().live == 1);

        instance.reset();
        g_instance = nullptr;
        REQUIRE(loop.run_until([] { return contract_log().live == 0; }));
        REQUIRE(contract_log().saw("detach"));
        REQUIRE_FALSE(contract_log().saw("invalidate"));
        REQUIRE(contract_log().off_main_thread_calls == 0);
        const auto calls = contract_log().calls.size();
        [view release];
        REQUIRE(contract_log().calls.size() == calls);
    }
    AudioComponentInstanceDispose(au);
}

TEST_CASE("disposing the AU instance off the main thread never waits for a blocked main thread",
          "[auv2][editor-host][au-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    auto instance = std::make_unique<FakeInstance>(true);
    g_instance = instance.get();
    AudioUnit au = fake_audio_unit();
    MainLoop loop;

    @autoreleasepool {
        NSView* view = [open_view(au) retain];
        REQUIRE(view != nil);

        // A host whose main thread blocks until its worker has disposed of the
        // instance. Waiting for the main thread on that worker would hang both.
        dispatch_semaphore_t disposed = dispatch_semaphore_create(0);
        std::thread disposer([&] {
            instance->sessions.reset();
            dispatch_semaphore_signal(disposed);
        });
        const bool finished =
            dispatch_semaphore_wait(
                disposed,
                dispatch_time(DISPATCH_TIME_NOW, 3 * static_cast<int64_t>(NSEC_PER_SEC))) == 0;
        // Run the main loop either way, so a waiting worker can finish and be
        // joined before the verdict.
        loop.run_until([] { return contract_log().live == 0; });
        disposer.join();
        dispatch_release(disposed);
        CHECK(finished);

        REQUIRE(contract_log().saw("detach"));
        REQUIRE(contract_log().live == 0);
        REQUIRE(contract_log().off_main_thread_calls == 0);
        instance.reset();
        g_instance = nullptr;
        [view release];
    }
    AudioComponentInstanceDispose(au);
}

TEST_CASE("AU v2 serves no contract view without an editor factory or in CI",
          "[auv2][editor-host][au-contract]") {
    reset_contract_recording();
    AudioUnit au = fake_audio_unit();
    {
        EditorEnvironment env;
        FakeInstance instance(false);
        g_instance = &instance;
        @autoreleasepool {
            REQUIRE(open_view(au) == nil);
        }
    }
    {
        EditorEnvironment env;
        FakeInstance instance(true);
        g_instance = &instance;
        contract_config().factory_builds_editor = false;
        @autoreleasepool {
            REQUIRE(open_view(au) == nil);
        }
        contract_config() = {};
        contract_config().attach_succeeds = false;
        @autoreleasepool {
            REQUIRE(open_view(au) == nil);
        }
        REQUIRE(contract_log().live == 0);
        contract_config() = {};
        ::setenv("CI", "1", 1);
        @autoreleasepool {
            REQUIRE(open_view(au) == nil);
        }
    }
    g_instance = nullptr;
    AudioComponentInstanceDispose(au);
}

TEST_CASE("AppKit key events map to the contract's key events",
          "[auv2][editor-host][au-contract]") {
    using pulp::format::EditorKey;
    using pulp::format::au::contract_key_event;
    const auto space = contract_key_event(49, @" ", NSEventModifierFlagShift, true, false);
    REQUIRE(space.key == EditorKey::space);
    REQUIRE(space.character == U' ');
    REQUIRE(space.modifiers == pulp::format::editor_modifier_shift);

    struct Case {
        unsigned short code;
        EditorKey key;
    };
    const Case cases[] = {
        {36, EditorKey::enter},     {76, EditorKey::enter},      {48, EditorKey::tab},
        {51, EditorKey::backspace}, {53, EditorKey::escape},     {117, EditorKey::delete_forward},
        {123, EditorKey::left},     {124, EditorKey::right},     {125, EditorKey::down},
        {126, EditorKey::up},       {115, EditorKey::home},      {119, EditorKey::end},
        {116, EditorKey::page_up},  {121, EditorKey::page_down}, {0, EditorKey::none}};
    for (const auto& c : cases) {
        CAPTURE(c.code);
        REQUIRE(contract_key_event(c.code, @"", 0, true, false).key == c.key);
    }
    const auto chord = contract_key_event(0, @"a",
                                          NSEventModifierFlagCommand | NSEventModifierFlagOption |
                                              NSEventModifierFlagControl,
                                          false, false);
    REQUIRE(chord.character == U'a');
    REQUIRE_FALSE(chord.is_down);
    REQUIRE(chord.modifiers ==
            (pulp::format::editor_modifier_command | pulp::format::editor_modifier_alt |
             pulp::format::editor_modifier_control));
    // A function key's private-use code point is not text.
    const unichar f1 = 0xF704;
    REQUIRE(contract_key_event(122, [NSString stringWithCharacters:&f1 length:1], 0, true, false)
                .character == 0);
}
