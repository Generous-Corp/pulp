// AU v2 effect side-chain input contract.
//
// An AU v2 effect whose descriptor declares a second input bus exposes a second
// AU input element named "Side Chain". Hosts discover a side-chain input purely
// from the input scope's element count (Logic shows its Side Chain pop-up only
// for such plug-ins), so these tests pin the host-visible properties and then
// drive the real render path — AUBase::DoRender, with host render callbacks on
// both input elements — to prove the pulled side-chain audio reaches the
// Processor's side-chain buffer while the main bus is unaffected.

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioToolbox/AudioUnit.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>

using pulp::format::PluginDescriptor;

namespace {

constexpr UInt32 kFrames = 64;
constexpr double kSampleRate = 48000.0;
constexpr float kSidechainScale = 1000.0f;

// What the processor observed on its most recent process() call.
struct SidechainObservation {
    int calls = 0;
    bool sidechain_present = false;
    std::size_t sidechain_channels = 0;
};
SidechainObservation g_seen;

int g_sidechain_bus_channels = 2;

// Main input passes through on channel 0. Output channel 1 carries the LAST
// side-chain channel (so a stereo side chain proves both channels arrived), or
// a -1 sentinel when no side chain is delivered.
class SidechainProbe final : public pulp::format::Processor {
public:
    PluginDescriptor descriptor() const override {
        return {
            .name = "AUSidechainProbe",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au-sidechain",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}, {"Sidechain", g_sidechain_bus_channels, true}},
            .output_buses = {{"Main Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = 1, .name = "Gain", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        ++g_seen.calls;
        const auto* sc = sidechain_input();
        g_seen.sidechain_present = sc != nullptr;
        g_seen.sidechain_channels = sc ? sc->num_channels() : 0;
        for (std::size_t n = 0; n < output.num_samples(); ++n) {
            output.channel_ptr(0)[n] = input.channel_ptr(0)[n];
            output.channel_ptr(1)[n] =
                sc ? sc->channel_ptr(sc->num_channels() - 1)[n] : -1.0f;
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_probe() {
    return std::make_unique<SidechainProbe>();
}

class PlainEffect final : public pulp::format::Processor {
public:
    PluginDescriptor descriptor() const override {
        return {.name = "AUPlainEffect",
                .manufacturer = "PulpTest",
                .bundle_id = "com.pulp.test.au-plain",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
};

std::unique_ptr<pulp::format::Processor> create_plain() {
    return std::make_unique<PlainEffect>();
}

struct ScopedFactory {
    explicit ScopedFactory(pulp::format::ProcessorFactory f)
        : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(f);
    }
    ~ScopedFactory() { pulp::format::register_plugin(previous); }
    pulp::format::ProcessorFactory previous;
};

AudioStreamBasicDescription float_format(double sample_rate, UInt32 channels) {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = sample_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                       kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    return fmt;
}

// Host render callbacks. Main input sample n on channel c = n + 1 + 100*c;
// side-chain sample n on channel c = (n + 1 + 100*c) * kSidechainScale. The
// absolute sample position comes from the timestamp so a sliced render is
// still checkable against the same formula.
OSStatus fill_main(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                   UInt32, UInt32 frames, AudioBufferList* io) {
    for (UInt32 c = 0; c < io->mNumberBuffers; ++c) {
        auto* dst = static_cast<float*>(io->mBuffers[c].mData);
        for (UInt32 n = 0; n < frames; ++n) dst[n] = static_cast<float>(n + 1 + 100 * c);
    }
    return noErr;
}

struct SidechainSource {
    int pulls = 0;
    UInt32 last_bus = 0;
    OSStatus status = noErr;
};

OSStatus fill_sidechain(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*,
                        UInt32 bus, UInt32 frames, AudioBufferList* io) {
    auto* source = static_cast<SidechainSource*>(ref);
    ++source->pulls;
    source->last_bus = bus;
    // A silent upstream side chain must not mark the main output silent.
    *flags |= kAudioUnitRenderAction_OutputIsSilence;
    for (UInt32 c = 0; c < io->mNumberBuffers; ++c) {
        auto* dst = static_cast<float*>(io->mBuffers[c].mData);
        for (UInt32 n = 0; n < frames; ++n)
            dst[n] = static_cast<float>(n + 1 + 100 * c) * kSidechainScale;
    }
    return source->status;
}

class TestEffect final : public pulp::format::au::PulpAUEffect {
public:
    TestEffect() : PulpAUEffect(nullptr) {}
};

void configure(TestEffect& effect, UInt32 sidechain_channels) {
    effect.CreateElements();
    const auto in_format = float_format(kSampleRate, 2);
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                       &in_format, sizeof(in_format)) == noErr);
    const auto out_format = float_format(kSampleRate, 2);
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                       0, &out_format, sizeof(out_format)) == noErr);
    if (sidechain_channels > 0) {
        const auto sc_format = float_format(kSampleRate, sidechain_channels);
        REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_StreamFormat,
                                           kAudioUnitScope_Input,
                                           pulp::format::au::kSidechainInputElement,
                                           &sc_format, sizeof(sc_format)) == noErr);
    }
    UInt32 max_frames = kFrames;
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice,
                                       kAudioUnitScope_Global, 0, &max_frames,
                                       sizeof(max_frames)) == noErr);
}

void install_callback(TestEffect& effect, AudioUnitElement element, AURenderCallback proc,
                      void* ref) {
    AURenderCallbackStruct cb{proc, ref};
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_SetRenderCallback,
                                       kAudioUnitScope_Input, element, &cb,
                                       sizeof(cb)) == noErr);
}

struct StereoOut {
    std::array<float, kFrames> left{};
    std::array<float, kFrames> right{};
    struct List {
        AudioBufferList bl;
        AudioBuffer second;
    } list{};
    StereoOut() {
        list.bl.mNumberBuffers = 2;
        list.bl.mBuffers[0] = {1, kFrames * sizeof(float), left.data()};
        list.bl.mBuffers[1] = {1, kFrames * sizeof(float), right.data()};
    }
};

OSStatus render_block(TestEffect& effect, StereoOut& out, Float64 sample_time,
                      AudioUnitRenderActionFlags& flags) {
    AudioTimeStamp ts{};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = sample_time;
    return effect.DoRender(flags, ts, 0, kFrames, out.list.bl);
}

std::string element_name(TestEffect& effect, AudioUnitScope scope, AudioUnitElement element,
                         OSStatus& status) {
    CFStringRef name = nullptr;
    status = effect.DispatchGetProperty(kAudioUnitProperty_ElementName, scope, element, &name);
    if (status != noErr || name == nullptr) return {};
    char buffer[128] = {};
    CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(name);
    return buffer;
}

UInt32 input_element_count(TestEffect& effect) {
    UInt32 count = 0;
    REQUIRE(effect.DispatchGetProperty(kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0,
                                       &count) == noErr);
    return count;
}

}  // namespace

TEST_CASE("AU v2 side-chain element helpers follow the descriptor",
          "[au][au-v2][sidechain]") {
    using namespace pulp::format::au;
    PluginDescriptor plain;  // one stereo input bus
    REQUIRE_FALSE(effect_has_sidechain_element(plain));
    REQUIRE(effect_input_element_count(plain) == 1);
    REQUIRE(sidechain_element_default_channels(plain) == 0);

    PluginDescriptor mono_sc;
    mono_sc.input_buses = {{"Main In", 2}, {"Key", 1, true}};
    REQUIRE(effect_has_sidechain_element(mono_sc));
    REQUIRE(effect_input_element_count(mono_sc) == 2);
    REQUIRE(sidechain_element_default_channels(mono_sc) == 1);

    // A declared-but-zero-width second bus is not a side chain the host can feed.
    PluginDescriptor empty_sc;
    empty_sc.input_buses = {{"Main In", 2}, {"Key", 0, true}};
    REQUIRE_FALSE(effect_has_sidechain_element(empty_sc));

    // Wider than the adapter's per-bus ceiling clamps rather than overflowing
    // the fixed side-chain pointer storage.
    PluginDescriptor wide_sc;
    wide_sc.input_buses = {{"Main In", 2}, {"Key", 64, true}};
    REQUIRE(sidechain_element_default_channels(wide_sc) ==
            pulp::format::boundary::kBoundaryMaxChannels);
}

TEST_CASE("AU v2 side-chain buffer resolution rejects malformed lists",
          "[au][au-v2][sidechain][malformed-layout]") {
    using pulp::format::au::resolve_sidechain_channels;
    std::array<float, kFrames> left{};
    std::array<float, kFrames> right{};
    struct {
        AudioBufferList bl;
        AudioBuffer second;
    } list{};
    list.bl.mNumberBuffers = 2;
    list.bl.mBuffers[0] = {1, kFrames * sizeof(float), left.data()};
    list.bl.mBuffers[1] = {1, kFrames * sizeof(float), right.data()};
    std::array<const float*, 8> ptrs{};

    REQUIRE(resolve_sidechain_channels(&list.bl, 2, 0, kFrames, ptrs.data(), ptrs.size()) == 2);
    REQUIRE(ptrs[0] == left.data());
    REQUIRE(ptrs[1] == right.data());

    // A slice later in the block reads from that frame onward.
    REQUIRE(resolve_sidechain_channels(&list.bl, 2, 16, kFrames - 16, ptrs.data(),
                                       ptrs.size()) == 2);
    REQUIRE(ptrs[0] == left.data() + 16);
    // ...but a slice that would read past the pulled storage is refused.
    REQUIRE(resolve_sidechain_channels(&list.bl, 2, 16, kFrames, ptrs.data(), ptrs.size()) == 0);

    REQUIRE(resolve_sidechain_channels(nullptr, 2, 0, kFrames, ptrs.data(), ptrs.size()) == 0);
    REQUIRE(resolve_sidechain_channels(&list.bl, 1, 0, kFrames, ptrs.data(), ptrs.size()) == 0);
    REQUIRE(resolve_sidechain_channels(&list.bl, 2, 0, kFrames, ptrs.data(), 1) == 0);
    list.bl.mBuffers[1].mData = nullptr;
    REQUIRE(resolve_sidechain_channels(&list.bl, 2, 0, kFrames, ptrs.data(), ptrs.size()) == 0);
    list.bl.mBuffers[1] = {2, kFrames * sizeof(float), right.data()};
    REQUIRE(resolve_sidechain_channels(&list.bl, 2, 0, kFrames, ptrs.data(), ptrs.size()) == 0);
}

TEST_CASE("AU v2 effect without a side-chain bus keeps one input element",
          "[au][au-v2][sidechain][properties]") {
    ScopedFactory factory(create_plain);
    TestEffect effect;
    effect.CreateElements();
    REQUIRE(input_element_count(effect) == 1);
    // Element 1 does not exist: the SDK either reports an error or throws
    // (its component dispatcher turns the throw into an error for a host).
    UInt32 size = 0;
    bool writable = false;
    OSStatus status = noErr;
    try {
        status = effect.DispatchGetPropertyInfo(kAudioUnitProperty_StreamFormat,
                                                kAudioUnitScope_Input, 1, size, writable);
    } catch (...) {
        status = kAudioUnitErr_InvalidElement;
    }
    REQUIRE(status != noErr);
}

TEST_CASE("AU v2 effect with a side-chain bus exposes a named second input element",
          "[au][au-v2][sidechain][properties]") {
    g_sidechain_bus_channels = 1;
    ScopedFactory factory(create_probe);
    TestEffect effect;
    effect.CreateElements();
    g_sidechain_bus_channels = 2;

    REQUIRE(input_element_count(effect) == 2);
    UInt32 output_count = 0;
    REQUIRE(effect.DispatchGetProperty(kAudioUnitProperty_ElementCount, kAudioUnitScope_Output,
                                       0, &output_count) == noErr);
    REQUIRE(output_count == 1);

    OSStatus status = noErr;
    REQUIRE(element_name(effect, kAudioUnitScope_Input, 1, status) == "Side Chain");
    REQUIRE(status == noErr);

    // The side-chain element starts at the declared bus width (mono here) and
    // the main bus's rate and sample layout.
    AudioStreamBasicDescription sc{};
    REQUIRE(effect.DispatchGetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
                                       &sc) == noErr);
    AudioStreamBasicDescription main{};
    REQUIRE(effect.DispatchGetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                       &main) == noErr);
    REQUIRE(sc.mChannelsPerFrame == 1);
    REQUIRE(sc.mSampleRate == main.mSampleRate);
    REQUIRE((sc.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0);
    REQUIRE((sc.mFormatFlags & kAudioFormatFlagIsFloat) != 0);

    // The host may renegotiate the side-chain width within the channel ceiling.
    const auto stereo = float_format(main.mSampleRate, 2);
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
                                       &stereo, sizeof(stereo)) == noErr);
    const auto too_wide = float_format(main.mSampleRate, 9);
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
                                       &too_wide, sizeof(too_wide)) ==
            kAudioUnitErr_FormatNotSupported);

    // A host that only sets the main bus rate carries the side chain along.
    const auto main_96k = float_format(96000.0, 2);
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
                                       &main_96k, sizeof(main_96k)) == noErr);
    REQUIRE(effect.DispatchGetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
                                       &sc) == noErr);
    REQUIRE(sc.mSampleRate == 96000.0);
    REQUIRE(sc.mChannelsPerFrame == 2);  // width the host chose is kept
}

TEST_CASE("AU v2 effect refuses a side-chain rate that differs from the main bus",
          "[au][au-v2][sidechain][properties]") {
    ScopedFactory factory(create_probe);
    TestEffect effect;
    configure(effect, 2);
    const auto mismatched = float_format(44100.0, 2);
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1,
                                       &mismatched, sizeof(mismatched)) == noErr);
    REQUIRE(effect.DoInitialize() == kAudioUnitErr_FormatNotSupported);
}

TEST_CASE("AU v2 effect delivers a connected side chain to the processor",
          "[au][au-v2][sidechain][render]") {
    ScopedFactory factory(create_probe);
    TestEffect effect;
    configure(effect, 2);
    REQUIRE(effect.DoInitialize() == noErr);

    SidechainSource source;
    install_callback(effect, 0, fill_main, nullptr);
    install_callback(effect, 1, fill_sidechain, &source);

    StereoOut out;
    g_seen = {};
    AudioUnitRenderActionFlags flags = 0;
    REQUIRE(render_block(effect, out, 0, flags) == noErr);

    REQUIRE(source.pulls == 1);
    REQUIRE(source.last_bus == pulp::format::au::kSidechainInputElement);
    REQUIRE(g_seen.calls == 1);
    REQUIRE(g_seen.sidechain_present);
    REQUIRE(g_seen.sidechain_channels == 2);
    for (UInt32 n = 0; n < kFrames; ++n) {
        INFO("frame " << n);
        // Main input is untouched by the side-chain pull.
        REQUIRE(out.left[n] == static_cast<float>(n + 1));
        // Side-chain channel 1 (the last one) reached the processor intact.
        REQUIRE(out.right[n] == static_cast<float>(n + 101) * kSidechainScale);
    }
    // The side chain's own silence flag did not leak into the main render.
    REQUIRE((flags & kAudioUnitRenderAction_OutputIsSilence) == 0);

    effect.DoCleanup();
}

TEST_CASE("AU v2 effect keeps the side chain aligned across scheduled-parameter slices",
          "[au][au-v2][sidechain][render][sample-accurate]") {
    ScopedFactory factory(create_probe);
    TestEffect effect;
    configure(effect, 1);
    REQUIRE(effect.DoInitialize() == noErr);

    SidechainSource source;
    install_callback(effect, 0, fill_main, nullptr);
    install_callback(effect, 1, fill_sidechain, &source);

    // An immediate event at frame 16 splits the render into two slices; the
    // second slice must read the side chain from frame 16, not from frame 0.
    AudioUnitParameterEvent event{};
    event.eventType = kParameterEvent_Immediate;
    event.parameter = 1;
    event.scope = kAudioUnitScope_Global;
    event.eventValues.immediate.bufferOffset = 16;
    event.eventValues.immediate.value = 1.0f;
    REQUIRE(effect.ScheduleParameter(&event, 1) == noErr);

    StereoOut out;
    g_seen = {};
    AudioUnitRenderActionFlags flags = 0;
    REQUIRE(render_block(effect, out, 0, flags) == noErr);
    REQUIRE(g_seen.calls == 2);
    REQUIRE(g_seen.sidechain_channels == 1);
    for (UInt32 n = 0; n < kFrames; ++n) {
        INFO("frame " << n);
        REQUIRE(out.left[n] == static_cast<float>(n + 1));
        REQUIRE(out.right[n] == static_cast<float>(n + 1) * kSidechainScale);
    }
    effect.DoCleanup();
}

TEST_CASE("AU v2 effect reports no side chain while element 1 is unconnected",
          "[au][au-v2][sidechain][render]") {
    ScopedFactory factory(create_probe);
    TestEffect effect;
    configure(effect, 2);
    REQUIRE(effect.DoInitialize() == noErr);
    install_callback(effect, 0, fill_main, nullptr);

    StereoOut out;
    out.right.fill(12345.0f);
    g_seen = {};
    AudioUnitRenderActionFlags flags = 0;
    REQUIRE(render_block(effect, out, 0, flags) == noErr);
    REQUIRE(g_seen.calls == 1);
    REQUIRE_FALSE(g_seen.sidechain_present);
    for (UInt32 n = 0; n < kFrames; ++n) {
        INFO("frame " << n);
        REQUIRE(out.left[n] == static_cast<float>(n + 1));
        REQUIRE(out.right[n] == -1.0f);  // the probe's "no side chain" sentinel
    }

    // Removing a previously installed side-chain callback disconnects it again.
    SidechainSource source;
    install_callback(effect, 1, fill_sidechain, &source);
    REQUIRE(render_block(effect, out, kFrames, flags) == noErr);
    REQUIRE(g_seen.sidechain_present);
    install_callback(effect, 1, nullptr, nullptr);
    REQUIRE(render_block(effect, out, 2 * kFrames, flags) == noErr);
    REQUIRE_FALSE(g_seen.sidechain_present);
    REQUIRE(source.pulls == 1);

    effect.DoCleanup();
}

TEST_CASE("AU v2 effect treats a failed side-chain pull as disconnected",
          "[au][au-v2][sidechain][render]") {
    ScopedFactory factory(create_probe);
    TestEffect effect;
    configure(effect, 2);
    REQUIRE(effect.DoInitialize() == noErr);

    SidechainSource source;
    source.status = kAudioUnitErr_NoConnection;
    install_callback(effect, 0, fill_main, nullptr);
    install_callback(effect, 1, fill_sidechain, &source);

    StereoOut out;
    g_seen = {};
    AudioUnitRenderActionFlags flags = 0;
    REQUIRE(render_block(effect, out, 0, flags) == noErr);
    REQUIRE(source.pulls == 1);
    REQUIRE_FALSE(g_seen.sidechain_present);
    REQUIRE(out.left[5] == 6.0f);
    REQUIRE(out.right[5] == -1.0f);
    effect.DoCleanup();
}
