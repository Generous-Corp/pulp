// AU v2 continuous-parameter display (WS-4 / G5).
//
// The gap this closes: GetParameterValueStrings only served DISCRETE params
// (range.step >= 1). Continuous params with a custom ParamInfo::to_string had
// no way to reach the host. The adapter now:
//   * sets kAudioUnitParameterFlag_ValuesHaveStrings when a param declares a
//     to_string, and
//   * answers kAudioUnitProperty_ParameterStringFromValue /
//     ...ValueFromString by round-tripping through to_string / from_string.
// These tests drive the AU property surface directly on a PulpAUEffect.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/au_v2_instrument.hpp>  // pulls the MusicDevice SDK bits
#include <pulp/format/au_v2_midi_processor.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#include <AudioToolbox/AudioUnit.h>
#include <Foundation/Foundation.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

using Catch::Matchers::WithinAbs;

extern char** environ;

namespace {

constexpr pulp::state::ParamID kFreqId = 1;   // continuous, custom display
constexpr pulp::state::ParamID kPlainId = 2;  // no converters
constexpr pulp::state::ParamID kLastMacroId = 12;
class DisplayProcessor;
DisplayProcessor* g_display_processor = nullptr;

class DisplayProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "AuDisplay",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au.display",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_group({.id = 10, .name = "Oscillators"});
        store.add_group({.id = 11, .name = "Wavetable 1", .parent_id = 10});
        // Continuous (step 0) frequency param with a custom formatter.
        pulp::state::ParamInfo freq{.id = kFreqId, .name = "Freq", .unit = "Hz",
                                    .range = {20.0f, 20000.0f, 1000.0f, 0.0f},
                                    .group_id = 11};
        freq.to_string = [](float v) {
            char b[32];
            std::snprintf(b, sizeof(b), "%.0f Hz", v);
            return std::string(b);
        };
        freq.from_string = [](const std::string& s) {
            return static_cast<float>(std::atof(s.c_str()));
        };
        store.add_parameter(freq);

        // No converters — flag must not be set, string property must decline.
        store.add_parameter({.id = kPlainId, .name = "Plain",
                             .range = {0.0f, 1.0f, 0.5f, 0.0f},
                             .group_id = 999});
        for (pulp::state::ParamID id = 3; id <= kLastMacroId; ++id) {
            store.add_parameter({
                .id = id,
                .name = "param_" + std::to_string(id),
                .range = {0.0f, 1.0f, 0.5f, 0.0f},
            });
        }
    }

    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels(); ++ch) {
            auto ic = in.channel(ch);
            auto oc = out.channel(ch);
            for (std::size_t i = 0; i < out.num_samples(); ++i) oc[i] = ic[i];
        }
    }
};

std::unique_ptr<pulp::format::Processor> make_display_processor() {
    auto processor = std::make_unique<DisplayProcessor>();
    g_display_processor = processor.get();
    return processor;
}

struct ScopedFactory {
    ScopedFactory() : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(make_display_processor);
    }
    ~ScopedFactory() {
        g_display_processor = nullptr;
        pulp::format::register_plugin(previous);
    }
    pulp::format::ProcessorFactory previous;
};

template <typename Base>
struct CapturingAdapter : Base {
    using Base::Base;
    using Base::publish_parameter_display_changes;

    std::vector<std::tuple<AudioUnitPropertyID, AudioUnitScope,
                           AudioUnitElement>> changed;
    std::vector<std::thread::id> callback_threads;

    void PropertyChanged(AudioUnitPropertyID id, AudioUnitScope scope,
                         AudioUnitElement element) override {
        changed.emplace_back(id, scope, element);
        callback_threads.push_back(std::this_thread::get_id());
    }
};

using CapturingEffect = CapturingAdapter<pulp::format::au::PulpAUEffect>;
using CapturingInstrument =
    CapturingAdapter<pulp::format::au::PulpAUInstrument>;
using CapturingMidiProcessor =
    CapturingAdapter<pulp::format::au::PulpAUMidiProcessor>;

struct ExternalCapturingEffect : pulp::format::au::PulpAUEffect {
    ExternalCapturingEffect(int& callback_count)
        : PulpAUEffect(nullptr), callback_count(callback_count)
    {
    }

    void PropertyChanged(AudioUnitPropertyID, AudioUnitScope,
                         AudioUnitElement) override {
        ++callback_count;
    }

    int& callback_count;
};

AudioStreamBasicDescription make_float_format(double sample_rate,
                                              UInt32 channels) {
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

template <typename Predicate>
bool pump_main_run_loop_until(Predicate&& predicate, double timeout_seconds) {
    NSDate* deadline =
        [NSDate dateWithTimeIntervalSinceNow:timeout_seconds];
    while (!predicate() && [deadline timeIntervalSinceNow] > 0.0) {
        [[NSRunLoop mainRunLoop]
            runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    return predicate();
}

void pump_main_run_loop_for(double seconds) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([deadline timeIntervalSinceNow] > 0.0) {
        [[NSRunLoop mainRunLoop]
            runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
}

struct ThrowWhenArmedOnCopy {
    explicit ThrowWhenArmedOnCopy(std::shared_ptr<bool> armed)
        : armed(std::move(armed))
    {
    }

    ThrowWhenArmedOnCopy(const ThrowWhenArmedOnCopy& other)
        : armed(other.armed)
    {
        if (*armed)
            throw std::runtime_error("intentional notify copy failure");
    }

    void operator()(pulp::state::ParamID) const {}

    std::shared_ptr<bool> armed;
};

template <typename Adapter>
void require_group_clump_surface(Adapter& adapter)
{
    AudioUnitParameterInfo grouped{};
    REQUIRE(adapter.GetParameterInfo(kAudioUnitScope_Global, kFreqId,
                                     grouped) == noErr);
    REQUIRE((grouped.flags & kAudioUnitParameterFlag_HasClump) != 0);
    REQUIRE(grouped.clumpID == 11);

    AudioUnitParameterInfo ungrouped{};
    REQUIRE(adapter.GetParameterInfo(kAudioUnitScope_Global, kPlainId,
                                     ungrouped) == noErr);
    REQUIRE((ungrouped.flags & kAudioUnitParameterFlag_HasClump) == 0);
    REQUIRE(ungrouped.clumpID == 0);

    UInt32 size = 0;
    bool writable = true;
    REQUIRE(adapter.GetPropertyInfo(kAudioUnitProperty_ParameterClumpName,
                                    kAudioUnitScope_Global, 0, size,
                                    writable) == noErr);
    REQUIRE(size == sizeof(AudioUnitParameterNameInfo));
    REQUIRE_FALSE(writable);

    AudioUnitParameterNameInfo name{.inID = 11,
                                    .inDesiredLength = kAudioUnitParameterName_Full};
    REQUIRE(adapter.GetProperty(kAudioUnitProperty_ParameterClumpName,
                                kAudioUnitScope_Global, 0, &name) == noErr);
    REQUIRE(name.outName != nullptr);
    char text[128]{};
    REQUIRE(CFStringGetCString(name.outName, text, sizeof(text),
                               kCFStringEncodingUTF8));
    REQUIRE(std::string(text) == "Oscillators/Wavetable 1");
    CFRelease(name.outName);

    AudioUnitParameterNameInfo short_name{.inID = 11,
                                          .inDesiredLength = 11};
    REQUIRE(adapter.GetProperty(kAudioUnitProperty_ParameterClumpName,
                                kAudioUnitScope_Global, 0,
                                &short_name) == noErr);
    REQUIRE(CFStringGetLength(short_name.outName) == 11);
    CFRelease(short_name.outName);

    AudioUnitParameterNameInfo zero_length{.inID = 11,
                                            .inDesiredLength = 0};
    REQUIRE(adapter.GetProperty(kAudioUnitProperty_ParameterClumpName,
                                kAudioUnitScope_Global, 0,
                                &zero_length) == noErr);
    REQUIRE(CFStringGetCString(zero_length.outName, text, sizeof(text),
                               kCFStringEncodingUTF8));
    REQUIRE(std::string(text) == "Oscillators/Wavetable 1");
    CFRelease(zero_length.outName);

    AudioUnitParameterNameInfo invalid{.inID = 999};
    REQUIRE(adapter.GetProperty(kAudioUnitProperty_ParameterClumpName,
                                kAudioUnitScope_Global, 0, &invalid) ==
            kAudioUnitErr_InvalidPropertyValue);
}

}  // namespace

TEST_CASE("AU v2 projects validated groups through every adapter clump surface",
          "[au][au-v2][params][groups]")
{
    ScopedFactory factory;
    pulp::format::au::PulpAUEffect effect(nullptr);
    require_group_clump_surface(effect);
    pulp::format::au::PulpAUInstrument instrument(nullptr);
    require_group_clump_surface(instrument);
    pulp::format::au::PulpAUMidiProcessor midi(nullptr);
    require_group_clump_surface(midi);
}

TEST_CASE("AU v2 continuous param advertises ValuesHaveStrings when to_string is set",
          "[au][au-v2][params][display]") {
    ScopedFactory factory;
    pulp::format::au::PulpAUEffect effect(nullptr);

    AudioUnitParameterInfo info{};
    REQUIRE(effect.GetParameterInfo(kAudioUnitScope_Global, kFreqId, info) == noErr);
    REQUIRE((info.flags & kAudioUnitParameterFlag_ValuesHaveStrings) != 0);

    AudioUnitParameterInfo plain{};
    REQUIRE(effect.GetParameterInfo(kAudioUnitScope_Global, kPlainId, plain) == noErr);
    REQUIRE((plain.flags & kAudioUnitParameterFlag_ValuesHaveStrings) == 0);
}

// GetParameterInfo feeds every AU host's automation lanes and generic editor.
// If the adapter reports a wrong range, default, or unit the control is
// silently mis-scaled — no crash, just wrong knob ranges. Only the
// ValuesHaveStrings flag was covered; assert the numeric metadata + unit map.
TEST_CASE("AU v2 GetParameterInfo maps range, default, and unit",
          "[au][au-v2][params][metadata]") {
    ScopedFactory factory;
    pulp::format::au::PulpAUEffect effect(nullptr);

    AudioUnitParameterInfo freq{};
    REQUIRE(effect.GetParameterInfo(kAudioUnitScope_Global, kFreqId, freq) == noErr);
    REQUIRE_THAT(freq.minValue, WithinAbs(20.0f, 0.001f));
    REQUIRE_THAT(freq.maxValue, WithinAbs(20000.0f, 0.001f));
    REQUIRE_THAT(freq.defaultValue, WithinAbs(1000.0f, 0.001f));
    REQUIRE(freq.unit == kAudioUnitParameterUnit_Hertz);  // "Hz" → Hertz

    AudioUnitParameterInfo plain{};
    REQUIRE(effect.GetParameterInfo(kAudioUnitScope_Global, kPlainId, plain) == noErr);
    REQUIRE_THAT(plain.minValue, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(plain.maxValue, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(plain.defaultValue, WithinAbs(0.5f, 0.001f));
}

TEST_CASE("AU v2 republishes display names without changing parameter identity",
          "[au][au-v2][instrument][params][metadata]") {
    ScopedFactory factory;
    CapturingInstrument instrument(nullptr);
    REQUIRE(g_display_processor != nullptr);

    std::array<AudioUnitParameterID, 12> before_ids{};
    UInt32 before_count = 0;
    REQUIRE(instrument.GetParameterList(
                kAudioUnitScope_Global, before_ids.data(), before_count) == noErr);
    REQUIRE(before_count == before_ids.size());

    REQUIRE(instrument.SetParameter(
                kFreqId, kAudioUnitScope_Global, 0, 440.0f, 0) == noErr);
    AudioUnitParameterInfo before{};
    REQUIRE(instrument.GetParameterInfo(
                kAudioUnitScope_Global, kFreqId, before) == noErr);
    REQUIRE(std::string(reinterpret_cast<char*>(before.name)) == "Freq");
    if (before.cfNameString) CFRelease(before.cfNameString);

    REQUIRE(g_display_processor->state().set_parameter_display_name(
        kFreqId, "Cutoff"));
    REQUIRE(g_display_processor->state().set_parameter_display_name(
        kFreqId, "Filter Cutoff"));
    REQUIRE(g_display_processor->state().set_parameter_display_name(
        kLastMacroId, "Macro 12 (unused)"));
    const auto control_thread = std::this_thread::get_id();
    instrument.publish_parameter_display_changes();

    AudioUnitParameterInfo after{};
    REQUIRE(instrument.GetParameterInfo(
                kAudioUnitScope_Global, kFreqId, after) == noErr);
    CHECK(std::string(reinterpret_cast<char*>(after.name)) == "Filter Cutoff");
    if (after.cfNameString) CFRelease(after.cfNameString);

    Float32 retained_value = 0.0f;
    REQUIRE(instrument.GetParameter(
                kFreqId, kAudioUnitScope_Global, 0, retained_value) == noErr);
    CHECK_THAT(retained_value, WithinAbs(440.0f, 0.001f));

    std::array<AudioUnitParameterID, 12> after_ids{};
    UInt32 after_count = 0;
    REQUIRE(instrument.GetParameterList(
                kAudioUnitScope_Global, after_ids.data(), after_count) == noErr);
    CHECK(after_count == before_count);
    CHECK(after_ids == before_ids);

    REQUIRE(instrument.changed.size() == 2);
    CHECK(instrument.changed[0] ==
          std::tuple{static_cast<AudioUnitPropertyID>(
                         kAudioUnitProperty_ParameterInfo),
                     static_cast<AudioUnitScope>(kAudioUnitScope_Global),
                     static_cast<AudioUnitElement>(kFreqId)});
    CHECK(instrument.changed[1] ==
          std::tuple{static_cast<AudioUnitPropertyID>(
                         kAudioUnitProperty_ParameterInfo),
                     static_cast<AudioUnitScope>(kAudioUnitScope_Global),
                     static_cast<AudioUnitElement>(kLastMacroId)});
    CHECK(instrument.callback_threads ==
          std::vector<std::thread::id>{control_thread, control_thread});

    instrument.publish_parameter_display_changes();
    CHECK(instrument.changed.size() == 2);

    REQUIRE(g_display_processor->state().set_parameter_display_name(
        kFreqId, "Resonance"));
    instrument.publish_parameter_display_changes();
    REQUIRE(instrument.changed.size() == 3);
    CHECK(std::get<2>(instrument.changed.back()) == kFreqId);

    AudioUnitParameterInfo regenerated{};
    REQUIRE(instrument.GetParameterInfo(
                kAudioUnitScope_Global, kFreqId, regenerated) == noErr);
    CHECK(std::string(reinterpret_cast<char*>(regenerated.name)) == "Resonance");
    if (regenerated.cfNameString) CFRelease(regenerated.cfNameString);
}

TEST_CASE("AU v2 effect and MIDI adapters publish exact display-name IDs",
          "[au][au-v2][params][metadata][main-thread]") {
    ScopedFactory factory;

    {
        CapturingEffect effect(nullptr);
        REQUIRE(g_display_processor != nullptr);
        REQUIRE(g_display_processor->state().set_parameter_display_name(
            kPlainId, "Mix"));
        effect.publish_parameter_display_changes();
        REQUIRE(effect.changed.size() == 1);
        CHECK(std::get<2>(effect.changed[0]) == kPlainId);
    }

    {
        CapturingMidiProcessor midi(nullptr);
        REQUIRE(g_display_processor != nullptr);
        REQUIRE(g_display_processor->state().set_parameter_display_name(
            kFreqId, "Transpose"));
        midi.publish_parameter_display_changes();
        REQUIRE(midi.changed.size() == 1);
        CHECK(std::get<2>(midi.changed[0]) == kFreqId);
    }
}

TEST_CASE("AU v2 display-name publisher schedules on the host main thread",
          "[au][au-v2][params][metadata][main-thread]") {
    ScopedFactory factory;
    CapturingEffect effect(nullptr);
    REQUIRE(g_display_processor != nullptr);
    REQUIRE([NSThread isMainThread]);

    REQUIRE(g_display_processor->state().set_parameter_display_name(
        kFreqId, "Scheduled Cutoff"));
    REQUIRE(pump_main_run_loop_until(
        [&] { return !effect.changed.empty(); }, 1.0));

    REQUIRE(effect.changed.size() == 1);
    CHECK(std::get<0>(effect.changed[0]) ==
          kAudioUnitProperty_ParameterInfo);
    CHECK(std::get<1>(effect.changed[0]) == kAudioUnitScope_Global);
    CHECK(std::get<2>(effect.changed[0]) == kFreqId);
    CHECK(effect.callback_threads ==
          std::vector<std::thread::id>{std::this_thread::get_id()});
}

TEST_CASE("AU v2 display-name publisher backs off while idle and resets fast",
          "[au][au-v2][params][metadata][main-thread]") {
    ScopedFactory factory;
    CapturingEffect effect(nullptr);
    REQUIRE(g_display_processor != nullptr);

    pump_main_run_loop_for(0.1);
    REQUIRE(g_display_processor->state().set_parameter_display_name(
        kFreqId, "Idle Cutoff"));

    pump_main_run_loop_for(0.2);
    CHECK(effect.changed.empty());
    REQUIRE(pump_main_run_loop_until(
        [&] { return effect.changed.size() == 1; }, 1.1));
    CHECK(std::get<2>(effect.changed[0]) == kFreqId);

    REQUIRE(g_display_processor->state().set_parameter_display_name(
        kFreqId, "Fast Cutoff"));
    REQUIRE(pump_main_run_loop_until(
        [&] { return effect.changed.size() == 2; }, 0.25));
    CHECK(std::get<2>(effect.changed[1]) == kFreqId);
}

TEST_CASE("AU v2 display-name publisher deduplicates stable parameter IDs",
          "[au][au-v2][params][metadata][main-thread]") {
    pulp::state::StateStore store;
    store.add_parameter({.id = kFreqId, .name = "First"});
    store.add_parameter({.id = kFreqId, .name = "Second"});

    pulp::format::au::ParameterDisplayNamePublisher publisher;
    std::vector<pulp::state::ParamID> changed;
    publisher.start(store, [&](pulp::state::ParamID id) {
        changed.push_back(id);
    });

    REQUIRE(store.set_parameter_display_name(kFreqId, "Shared"));
    publisher.poll_main_thread();
    CHECK(changed == std::vector<pulp::state::ParamID>{kFreqId});
}

TEST_CASE("AU v2 display-name publisher tolerates reentrant teardown",
          "[au][au-v2][params][metadata][main-thread]") {
    pulp::state::StateStore store;
    store.add_parameter({.id = kFreqId, .name = "Freq"});
    store.add_parameter({.id = kPlainId, .name = "Plain"});

    pulp::format::au::ParameterDisplayNamePublisher publisher;
    std::vector<pulp::state::ParamID> changed;
    publisher.start(store, [&](pulp::state::ParamID id) {
        changed.push_back(id);
        publisher.stop();
    });

    REQUIRE(store.set_parameter_display_name(kFreqId, "Cutoff"));
    REQUIRE(store.set_parameter_display_name(kPlainId, "Mix"));
    publisher.poll_main_thread();

    CHECK(changed == std::vector<pulp::state::ParamID>{kFreqId});
}

TEST_CASE("AU v2 display-name publisher releases a drain after snapshot failure",
          "[au][au-v2][params][metadata][main-thread]") {
    constexpr const char* kChildEnvironment =
        "PULP_AU_DISPLAY_DRAIN_EXCEPTION_CHILD";
    if (std::getenv(kChildEnvironment)) {
        pulp::state::StateStore store;
        store.add_parameter({.id = kFreqId, .name = "Freq"});

        auto armed = std::make_shared<bool>(false);
        pulp::format::au::ParameterDisplayNamePublisher::Notify notify{
            ThrowWhenArmedOnCopy{armed}};
        pulp::format::au::ParameterDisplayNamePublisher publisher;
        publisher.start(store, std::move(notify));

        REQUIRE(store.set_parameter_display_name(kFreqId, "Cutoff"));
        *armed = true;
        REQUIRE_THROWS_AS(publisher.poll_main_thread(), std::runtime_error);

        // Before the drain's lifetime was guarded from its increment, the
        // throwing std::function copy leaked drains_in_flight and this
        // teardown waited forever.
        publisher.stop();
        return;
    }

    const char* executable =
        [[[NSBundle mainBundle] executablePath] fileSystemRepresentation];
    REQUIRE(executable != nullptr);
    std::string executable_path(executable);
    std::string test_name(
        "AU v2 display-name publisher releases a drain after snapshot failure");
    char* child_argv[] = {
        executable_path.data(),
        test_name.data(),
        nullptr,
    };

    REQUIRE(::setenv(kChildEnvironment, "1", 1) == 0);
    pid_t child = -1;
    const int spawn_result = ::posix_spawn(
        &child, executable_path.c_str(), nullptr, nullptr, child_argv, environ);
    REQUIRE(::unsetenv(kChildEnvironment) == 0);
    REQUIRE(spawn_result == 0);

    int status = 0;
    bool exited = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        REQUIRE(waited >= 0);
        if (waited == child) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!exited) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }

    REQUIRE(exited);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("AU v2 delayed display-name task is inert after adapter destruction",
          "[au][au-v2][params][metadata][main-thread]") {
    ScopedFactory factory;
    int callback_count = 0;
    {
        ExternalCapturingEffect effect(callback_count);
        REQUIRE(g_display_processor != nullptr);
        REQUIRE(g_display_processor->state().set_parameter_display_name(
            kFreqId, "Destroyed Cutoff"));
    }

    pump_main_run_loop_for(0.1);
    CHECK(callback_count == 0);
}

TEST_CASE("AU v2 instrument Render never publishes parameter metadata",
          "[au][au-v2][instrument][params][rt-safety]") {
    ScopedFactory factory;
    CapturingInstrument instrument(nullptr);
    REQUIRE(g_display_processor != nullptr);

    constexpr UInt32 kFrames = 64;
    instrument.CreateElements();
    REQUIRE(instrument.GetOutput(0)->SetStreamFormat(
                make_float_format(48000.0, 2)) == noErr);
    UInt32 max_frames = kFrames;
    REQUIRE(instrument.DispatchSetProperty(
                kAudioUnitProperty_MaximumFramesPerSlice,
                kAudioUnitScope_Global, 0, &max_frames,
                sizeof(max_frames)) == noErr);
    REQUIRE(instrument.DoInitialize() == noErr);

    REQUIRE(g_display_processor->state().set_parameter_display_name(
        kFreqId, "Cutoff"));
    instrument.changed.clear();
    instrument.callback_threads.clear();

    std::thread audio_thread([&] {
        instrument.publish_parameter_display_changes();
    });
    audio_thread.join();
    CHECK(instrument.changed.empty());

    AudioUnitRenderActionFlags flags = 0;
    AudioTimeStamp timestamp{};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    REQUIRE(instrument.Render(flags, timestamp, kFrames) == noErr);
    CHECK(instrument.changed.empty());

    instrument.publish_parameter_display_changes();
    REQUIRE(instrument.changed.size() == 1);
    CHECK(std::get<2>(instrument.changed[0]) == kFreqId);
    instrument.DoCleanup();
}

TEST_CASE("AU v2 ParameterStringFromValue formats via to_string",
          "[au][au-v2][params][display]") {
    ScopedFactory factory;
    pulp::format::au::PulpAUEffect effect(nullptr);

    // GetPropertyInfo advertises the property at global scope.
    UInt32 size = 0;
    bool writable = true;
    REQUIRE(effect.GetPropertyInfo(kAudioUnitProperty_ParameterStringFromValue,
                                   kAudioUnitScope_Global, 0, size, writable) == noErr);
    REQUIRE(size == sizeof(AudioUnitParameterStringFromValue));
    REQUIRE_FALSE(writable);

    Float32 value = 440.0f;
    AudioUnitParameterStringFromValue sfv{};
    sfv.inParamID = kFreqId;
    sfv.inValue = &value;
    sfv.outString = nullptr;
    REQUIRE(effect.GetProperty(kAudioUnitProperty_ParameterStringFromValue,
                               kAudioUnitScope_Global, 0, &sfv) == noErr);
    REQUIRE(sfv.outString != nullptr);
    char buf[64] = {0};
    REQUIRE(CFStringGetCString(sfv.outString, buf, sizeof(buf), kCFStringEncodingUTF8));
    REQUIRE(std::string(buf) == "440 Hz");
    CFRelease(sfv.outString);

    // A continuous param without a custom formatter uses canonical numeric
    // fallback, matching the shared adapter text contract.
    Float32 plain_value = 0.5f;
    AudioUnitParameterStringFromValue plain{};
    plain.inParamID = kPlainId;
    plain.inValue = &plain_value;
    REQUIRE(effect.GetProperty(kAudioUnitProperty_ParameterStringFromValue,
                               kAudioUnitScope_Global, 0, &plain) == noErr);
    REQUIRE(plain.outString != nullptr);
    REQUIRE(CFStringGetCString(plain.outString, buf, sizeof(buf),
                               kCFStringEncodingUTF8));
    REQUIRE(std::string(buf) == "0.50");
    CFRelease(plain.outString);
}

TEST_CASE("AU v2 ParameterValueFromString parses via from_string",
          "[au][au-v2][params][display]") {
    ScopedFactory factory;
    pulp::format::au::PulpAUEffect effect(nullptr);

    AudioUnitParameterValueFromString vfs{};
    vfs.inParamID = kFreqId;
    vfs.inString = CFSTR("880 Hz");
    vfs.outValue = 0.0f;
    REQUIRE(effect.GetProperty(kAudioUnitProperty_ParameterValueFromString,
                               kAudioUnitScope_Global, 0, &vfs) == noErr);
    REQUIRE_THAT(vfs.outValue, WithinAbs(880.0f, 1e-3f));

    // Round-trip: value -> string -> value.
    AudioUnitParameterStringFromValue sfv{};
    Float32 v = vfs.outValue;
    sfv.inParamID = kFreqId;
    sfv.inValue = &v;
    REQUIRE(effect.GetProperty(kAudioUnitProperty_ParameterStringFromValue,
                               kAudioUnitScope_Global, 0, &sfv) == noErr);
    char buf[64] = {0};
    REQUIRE(CFStringGetCString(sfv.outString, buf, sizeof(buf), kCFStringEncodingUTF8));
    REQUIRE(std::string(buf) == "880 Hz");
    CFRelease(sfv.outString);

    // A continuous param without a custom parser uses canonical numeric
    // fallback, matching the shared adapter text contract.
    AudioUnitParameterValueFromString plain{};
    plain.inParamID = kPlainId;
    plain.inString = CFSTR("0.5");
    REQUIRE(effect.GetProperty(kAudioUnitProperty_ParameterValueFromString,
                               kAudioUnitScope_Global, 0, &plain) == noErr);
    REQUIRE_THAT(plain.outValue, WithinAbs(0.5f, 1e-6f));
}
