// Hosting Apple's own Audio Units through the AUAudioUnit slot, out of process.
//
// These are real plugin binaries that every Mac carries, so the tests run
// without installing anything. PluginSlot::load() with
// PluginFormat::AudioUnitV3 instantiates them with
// kAudioComponentInstantiation_LoadOutOfProcess: the plugin runs in the
// system's Audio Unit hosting service and this process talks to it through the
// AUAudioUnit proxy. What the probe fixture checks from inside a plugin
// (transport, offline mode, parameter edits, latency changes) runs against the
// same slot in test_host_probe_bundles.cpp.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/native_handle_visitor.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/plugin_slot_auv3.hpp>
#include <pulp/host/signal_graph.hpp>

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::PluginSlot;
namespace fmt = pulp::format;

namespace {

constexpr int kFrames = 256;
constexpr double kRate = 48000.0;

std::unique_ptr<PluginSlot> load(const char* id, bool out_of_process = true) {
    PluginInfo info;
    info.format = PluginFormat::AudioUnitV3;
    info.unique_id = id;
    pulp::host::AuV3LoadOptions options;
    options.out_of_process = out_of_process;
    auto slot =
        out_of_process ? PluginSlot::load(info) : pulp::host::load_auv3_plugin(info, options);
    REQUIRE(slot);
    REQUIRE(slot->is_loaded());
    return slot;
}

/// The AUAudioUnit the slot hosts, through the typed native-handle visitor.
AUAudioUnit* audio_unit(const PluginSlot& slot) {
    struct Visitor : pulp::host::NativeHandleVisitor {
        AUAudioUnit* unit = nil;
        void visit_audio_unit_v3(const PluginSlot&,
                                 const pulp::host::AudioUnitV3NativeHandle& ext) override {
            unit = (__bridge AUAudioUnit*)ext.audio_unit;
        }
    } v;
    slot.accept(v);
    return v.unit;
}

/// True when the system reports the unit as hosted in another process. Older
/// systems cannot say; the test then trusts the instantiation option.
bool runs_out_of_process(const PluginSlot& slot) {
    AUAudioUnit* unit = audio_unit(slot);
    REQUIRE(unit != nil);
    if ([unit respondsToSelector:@selector(isLoadedInProcess)])
        return ![unit isLoadedInProcess];
    return true;
}

std::uint32_t param_named(const PluginSlot& slot, const std::string& fragment) {
    for (const auto& p : slot.parameters())
        if (p.name.find(fragment) != std::string::npos)
            return p.id;
    FAIL("no parameter named like " << fragment);
    return 0;
}

struct Stereo {
    std::vector<float> in_l = std::vector<float>(kFrames, 0.0f);
    std::vector<float> in_r = std::vector<float>(kFrames, 0.0f);
    std::vector<float> out_l = std::vector<float>(kFrames, 0.0f);
    std::vector<float> out_r = std::vector<float>(kFrames, 0.0f);
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::host::ParameterEventQueue params;

    void impulses() {
        std::fill(in_l.begin(), in_l.end(), 0.0f);
        std::fill(in_r.begin(), in_r.end(), 0.0f);
        for (int i = 0; i < kFrames; i += 32) {
            in_l[(std::size_t)i] = 0.5f;
            in_r[(std::size_t)i] = -0.5f;
        }
    }
    void run(PluginSlot& slot) {
        std::array<const float*, 2> ip{in_l.data(), in_r.data()};
        std::array<float*, 2> op{out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> input(ip.data(), 2, kFrames);
        pulp::audio::BufferView<float> output(op.data(), 2, kFrames);
        midi_out.clear();
        slot.process(output, input, midi_in, midi_out, params, kFrames);
        midi_in.clear();
    }
    float peak() const {
        float p = 0.0f;
        for (std::size_t i = 0; i < out_l.size(); ++i)
            p = std::max({p, std::abs(out_l[i]), std::abs(out_r[i])});
        return p;
    }
};

} // namespace

// An in-process AU v3 effect that passes audio through and, on request,
// replaces its parameter tree the way a sampler does on a preset load.
@interface PulpTestTreeUnit : AUAudioUnit
- (void)rebuildTreeWithGeneration:(int)generation;
@end

@implementation PulpTestTreeUnit {
    AUAudioUnitBusArray* _inputs;
    AUAudioUnitBusArray* _outputs;
}
- (instancetype)initWithComponentDescription:(AudioComponentDescription)desc
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError**)error {
    self = [super initWithComponentDescription:desc options:options error:error];
    if (!self)
        return nil;
    AVAudioFormat* format =
        [[[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000.0 channels:2] autorelease];
    AUAudioUnitBus* input_bus = [[[AUAudioUnitBus alloc] initWithFormat:format
                                                                  error:nil] autorelease];
    AUAudioUnitBus* output_bus = [[[AUAudioUnitBus alloc] initWithFormat:format
                                                                   error:nil] autorelease];
    if (!input_bus || !output_bus)
        return nil;
    _inputs = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                     busType:AUAudioUnitBusTypeInput
                                                      busses:@[ input_bus ]];
    _outputs = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                      busType:AUAudioUnitBusTypeOutput
                                                       busses:@[ output_bus ]];
    [self rebuildTreeWithGeneration:1];
    return self;
}
- (void)dealloc {
    [_inputs release];
    [_outputs release];
    [super dealloc];
}
- (AUAudioUnitBusArray*)inputBusses {
    return _inputs;
}
- (AUAudioUnitBusArray*)outputBusses {
    return _outputs;
}
- (void)rebuildTreeWithGeneration:(int)generation {
    NSMutableArray* children = [NSMutableArray array];
    for (int i = 0; i < generation; ++i) {
        NSString* name = [NSString stringWithFormat:@"Level %d", generation];
        if (i > 0)
            name = [NSString stringWithFormat:@"Extra %d", i];
        [children
            addObject:[AUParameterTree
                          createParameterWithIdentifier:[NSString stringWithFormat:@"p%d", i]
                                                   name:name
                                                address:(AUParameterAddress)(generation * 100 + i)
                                                    min:0
                                                    max:1
                                                   unit:kAudioUnitParameterUnit_Generic
                                               unitName:nil
                                                  flags:kAudioUnitParameterFlag_IsWritable |
                                                        kAudioUnitParameterFlag_IsReadable
                                           valueStrings:nil
                                    dependentParameters:nil]];
    }
    self.parameterTree = [AUParameterTree createTreeWithChildren:children];
}
- (AUInternalRenderBlock)internalRenderBlock {
    return ^AUAudioUnitStatus(AudioUnitRenderActionFlags* flags, const AudioTimeStamp* stamp,
                              AUAudioFrameCount frames, NSInteger bus, AudioBufferList* output,
                              const AURenderEvent*, AURenderPullInputBlock pull) {
      if (!pull)
          return kAudioUnitErr_NoConnection;
      return pull(flags, stamp, frames, 0, output);
    };
}
@end

namespace {

bool register_tree_unit() {
    static const bool registered = [] {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'PTtr';
        desc.componentManufacturer = 'PlHp';
        [AUAudioUnit registerSubclass:[PulpTestTreeUnit class]
               asComponentDescription:desc
                                 name:@"Pulp: Tree Test"
                              version:1];
        return true;
    }();
    return registered;
}

} // namespace

TEST_CASE("AUv3 slot runs Apple's AUDelay out of process", "[host][auv3][oop]") {
    auto slot = load("aufx:dely:appl");
    CHECK(runs_out_of_process(*slot));
    CHECK(slot->info().name == "AUDelay");
    CHECK_FALSE(slot->info().is_instrument);
    REQUIRE(slot->prepare(kRate, kFrames));

    // All dry: the plugin's output is its input, so audio crossed the process
    // boundary both ways, sample for sample.
    const auto mix = param_named(*slot, "Dry/Wet");
    slot->set_parameter(mix, 0.0f);
    Stereo s;
    s.impulses();
    s.run(*slot);
    s.run(*slot);
    for (int i = 0; i < kFrames; ++i) {
        INFO("frame " << i);
        REQUIRE(std::abs(s.out_l[(std::size_t)i] - s.in_l[(std::size_t)i]) < 1e-4f);
        REQUIRE(std::abs(s.out_r[(std::size_t)i] - s.in_r[(std::size_t)i]) < 1e-4f);
    }

    // All wet with a long delay: the input is held back, so the block is
    // silent although the input is not. A host edit reaches a unit in
    // another process asynchronously, so blocks run until it lands.
    slot->set_parameter(mix, 100.0f);
    slot->set_parameter(param_named(*slot, "Delay Time"), 1.0f);
    Stereo wet;
    wet.impulses();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        wet.run(*slot);
    } while (wet.peak() >= 1e-3f && std::chrono::steady_clock::now() < deadline);
    CHECK(wet.peak() < 1e-3f);
    slot->release();
}

TEST_CASE("AUv3 slot schedules parameter automation inside the block", "[host][auv3][oop]") {
    auto slot = load("aufx:dely:appl");
    REQUIRE(slot->prepare(kRate, kFrames));
    const auto mix = param_named(*slot, "Dry/Wet");
    slot->set_parameter(mix, 0.0f);
    Stereo s;
    s.impulses();
    s.run(*slot);
    // Automation to fully wet at frame 0 of the next block: with a 1 s delay
    // that block goes silent, which only happens if the event reached the
    // plugin through the render path.
    slot->set_parameter(param_named(*slot, "Delay Time"), 1.0f);
    s.params.push(pulp::host::ParameterEvent{mix, 0, 100.0f, 0});
    s.run(*slot);
    s.params.clear();
    s.run(*slot);
    CHECK(s.peak() < 1e-3f);
}

TEST_CASE("AUv3 slot saves and restores plugin state out of process", "[host][auv3][oop][state]") {
    auto slot = load("aufx:dely:appl");
    REQUIRE(slot->prepare(kRate, kFrames));
    const auto time = param_named(*slot, "Delay Time");
    slot->set_parameter(time, 0.5f);
    const auto saved = slot->save_state();
    REQUIRE_FALSE(saved.empty());
    slot->set_parameter(time, 1.5f);
    REQUIRE(std::abs(slot->get_parameter(time) - 1.5f) < 1e-4f);
    REQUIRE(slot->restore_state(saved));
    CHECK(std::abs(slot->get_parameter(time) - 0.5f) < 1e-4f);
    CHECK(slot->state_change_count() >= 1);

    // The same state restores into a fresh instance in another process.
    auto other = load("aufx:dely:appl");
    REQUIRE(other->restore_state(saved));
    CHECK(std::abs(other->get_parameter(time) - 0.5f) < 1e-4f);
    const std::vector<std::uint8_t> junk{1, 2, 3};
    CHECK_FALSE(other->restore_state(junk));
}

TEST_CASE("AUv3 slot loads in process when asked", "[host][auv3]") {
    auto slot = load("aufx:dely:appl", false);
    AUAudioUnit* unit = audio_unit(*slot);
    REQUIRE(unit != nil);
    if ([unit respondsToSelector:@selector(isLoadedInProcess)])
        CHECK([unit isLoadedInProcess]);
    REQUIRE(slot->prepare(kRate, kFrames));
    slot->set_parameter(param_named(*slot, "Dry/Wet"), 0.0f);
    Stereo s;
    s.impulses();
    s.run(*slot);
    s.run(*slot);
    CHECK(std::abs(s.out_l[0] - 0.5f) < 1e-4f);
}

TEST_CASE("AUv3 slot plays Apple's DLSMusicDevice from host MIDI out of process",
          "[host][auv3][oop][midi]") {
    auto slot = load("aumu:dls :appl");
    CHECK(runs_out_of_process(*slot));
    CHECK(slot->info().is_instrument);
    REQUIRE(slot->prepare(kRate, kFrames));
    Stereo s;
    s.run(*slot);
    CHECK(s.peak() < 1e-6f); // Silent until it is played.

    auto note = pulp::midi::MidiEvent::note_on(0, 60, 110);
    note.sample_offset = 16;
    s.midi_in.add(note);
    float peak = 0.0f;
    for (int b = 0; b < 8; ++b) {
        s.run(*slot);
        peak = std::max(peak, s.peak());
    }
    CHECK(peak > 0.01f);
}

TEST_CASE("AUv3 slot reports latency to SignalGraph delay compensation", "[host][auv3][oop][pdc]") {
    auto owned = load("aufx:lmtr:appl");
    auto* limiter = owned.get();
    pulp::host::SignalGraph graph;
    const auto in = graph.add_input_node(2);
    const auto plug = graph.add_plugin_node(std::move(owned), 2, 2, "limiter");
    const auto out = graph.add_output_node(2);
    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(in, 1, plug, 1));
    REQUIRE(graph.connect(plug, 0, out, 0));
    REQUIRE(graph.connect(plug, 1, out, 1));
    REQUIRE(graph.prepare(kRate, kFrames));
    // AUPeakLimiter looks ahead 2 ms.
    REQUIRE(limiter->latency_query() == PluginSlot::LatencyQuery::Available);
    CHECK(limiter->latency_samples() == 96);
    CHECK(graph.latency_samples() == 96);
}

TEST_CASE("AUv3 slot feeds every input bus of a multi-bus unit out of process",
          "[host][auv3][oop][buses]") {
    auto slot = load("aumx:mcmx:appl");
    REQUIRE(slot->prepare(kRate, kFrames));
    // The mixer starts with every gain at zero and only its first input on.
    // Its addresses do not fit 32 bits, so ids are ordinals and names carry
    // the key path.
    slot->set_parameter(param_named(*slot, "(input.1.1)"), 1.0f); // Enable
    slot->set_parameter(param_named(*slot, "(input.1.0)"), 1.0f); // Gain
    slot->set_parameter(param_named(*slot, "(output.0)"), 1.0f);  // Gain
    // Host buses: silent main input, a signal on the second bus only.
    std::vector<std::vector<float>> store(6, std::vector<float>(kFrames, 0.0f));
    std::fill(store[2].begin(), store[2].end(), 0.25f);
    std::fill(store[3].begin(), store[3].end(), 0.25f);
    std::array<const float*, 2> main_in{store[0].data(), store[1].data()};
    std::array<const float*, 2> second_in{store[2].data(), store[3].data()};
    std::array<float*, 2> main_out{store[4].data(), store[5].data()};
    std::array<fmt::ProcessBusBufferView<const float>, 2> ins{};
    std::array<fmt::ProcessBusBufferView<float>, 1> outs{};
    ins[0].buffer = pulp::audio::BufferView<const float>(main_in.data(), 2, kFrames);
    ins[1].info.role = fmt::BusRole::Sidechain;
    ins[1].buffer = pulp::audio::BufferView<const float>(second_in.data(), 2, kFrames);
    outs[0].buffer = pulp::audio::BufferView<float>(main_out.data(), 2, kFrames);
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::host::ParameterEventQueue params;
    float peak = 0.0f;
    for (int b = 0; b < 4; ++b) {
        fmt::ProcessBuffers buffers{fmt::ProcessBusBufferSet<const float>{std::span(ins)},
                                    fmt::ProcessBusBufferSet<float>{std::span(outs)}};
        slot->process(buffers, midi_in, midi_out, params, kFrames);
        for (float v : store[4])
            peak = std::max(peak, std::abs(v));
    }
    // Only the second bus carries signal, so any output came through it.
    CHECK(peak > 0.05f);
}

TEST_CASE("AUv3 slot refuses an unknown component", "[host][auv3]") {
    PluginInfo info;
    info.format = PluginFormat::AudioUnitV3;
    info.unique_id = "aufx:none:none";
    CHECK(PluginSlot::load(info) == nullptr);
    info.unique_id = "not an id";
    CHECK(PluginSlot::load(info) == nullptr);
}

TEST_CASE("AUv3 slot embeds the plugin's own editor from out of process",
          "[host][auv3][oop][editor]") {
    // A remote editor needs a launched application, as in any GUI host.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];

    auto slot = load("aufx:dely:appl");
    REQUIRE(slot->has_editor());
    NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, 600)];
    auto editor = slot->create_hosted_editor((__bridge void*)parent);
    REQUIRE(editor);
    CHECK(editor->width > 0);
    CHECK(editor->height > 0);
    // The container holds the plugin's view inside the host's parent.
    NSView* container = (__bridge NSView*)editor->native_handle;
    CHECK(container.superview == parent);
    CHECK(container.subviews.count == 1);
    // One editor at a time.
    CHECK(slot->create_hosted_editor((__bridge void*)parent) == nullptr);
    slot->destroy_hosted_editor(std::move(editor));
    CHECK(parent.subviews.count == 0);
    [parent release];
}

TEST_CASE("AUv3 slot delivers MIDI at its sample offset across the process boundary",
          "[host][auv3][oop][midi]") {
    // AUDelay applies scheduled parameter events at the start of the block
    // (in and out of process alike), so it cannot show an offset; an
    // instrument starting a note can.
    auto slot = load("aumu:dls :appl");
    REQUIRE(slot->prepare(kRate, kFrames));
    Stereo s;
    s.run(*slot);
    auto note = pulp::midi::MidiEvent::note_on(0, 60, 127);
    note.sample_offset = 128;
    s.midi_in.add(note);
    s.run(*slot);
    float before = 0.0f, after = 0.0f;
    for (int i = 0; i < 128; ++i)
        before = std::max(before, std::abs(s.out_l[(std::size_t)i]));
    for (int i = 128; i < kFrames; ++i)
        after = std::max(after, std::abs(s.out_l[(std::size_t)i]));
    CHECK(before < 1e-6f); // Nothing before the note's frame.
    CHECK(after > 1e-3f);  // The note sounds from its frame on.
}

TEST_CASE("AUv3 slot follows a plug-in that replaces its parameter tree", "[host][auv3]") {
    REQUIRE(register_tree_unit());
    pulp::host::AuV3LoadOptions in_process;
    in_process.out_of_process = false;
    PluginInfo info;
    info.format = PluginFormat::AudioUnitV3;
    info.unique_id = "aufx:PTtr:PlHp";
    auto slot = pulp::host::load_auv3_plugin(info, in_process);
    REQUIRE(slot);
    auto* unit = (PulpTestTreeUnit*)audio_unit(*slot);
    REQUIRE([unit isKindOfClass:[PulpTestTreeUnit class]]);
    REQUIRE(slot->prepare(kRate, kFrames));
    REQUIRE(slot->parameters().size() == 1);
    CHECK(slot->parameters()[0].name == "Level 1");
    slot->set_parameter(100, 0.5f);
    CHECK(std::abs(slot->get_parameter(100) - 0.5f) < 1e-6f);
    const auto restarts = slot->restart_request_count();

    // The plug-in replaces its tree, as a sampler does on a preset load; the
    // old parameter objects are released with it.
    @autoreleasepool {
        [unit rebuildTreeWithGeneration:2];
    }
    CHECK(slot->restart_request_count() > restarts);
    const auto params = slot->parameters();
    REQUIRE(params.size() == 2);
    CHECK(params[0].name == "Level 2");
    // The old id is gone, not dangling.
    CHECK(slot->get_parameter(100) == 0.0f);
    slot->set_parameter(100, 0.9f);

    // Host edits reach the new tree...
    slot->set_parameter(200, 0.75f);
    CHECK(std::abs([unit.parameterTree parameterWithAddress:200].value - 0.75f) < 1e-6f);
    CHECK(std::abs(slot->get_parameter(200) - 0.75f) < 1e-6f);
    // ...and the plug-in's edits on it reach the host.
    [[unit.parameterTree parameterWithAddress:201] setValue:0.25f originator:nullptr];
    std::array<pulp::host::PluginParameterEdit, 16> edits{};
    bool saw = false;
    for (int i = 0; i < 100 && !saw; ++i) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        const auto n = slot->drain_parameter_edits(edits);
        for (std::size_t k = 0; k < n; ++k)
            saw = saw || (edits[k].id == 201 &&
                          edits[k].kind == pulp::host::PluginParameterEdit::Kind::value &&
                          std::abs(edits[k].plain_value - 0.25f) < 1e-6f);
    }
    CHECK(saw);
    // Audio still runs after the rebuild.
    Stereo s;
    s.impulses();
    s.run(*slot);
    CHECK(std::abs(s.out_l[0] - 0.5f) < 1e-6f);
}
