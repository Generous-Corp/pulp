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
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#include <AudioToolbox/AudioUnit.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using Catch::Matchers::WithinAbs;

namespace {

constexpr pulp::state::ParamID kFreqId = 1;   // continuous, custom display
constexpr pulp::state::ParamID kPlainId = 2;  // no converters

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
        // Continuous (step 0) frequency param with a custom formatter.
        pulp::state::ParamInfo freq{.id = kFreqId, .name = "Freq", .unit = "Hz",
                                    .range = {20.0f, 20000.0f, 1000.0f, 0.0f}};
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
                             .range = {0.0f, 1.0f, 0.5f, 0.0f}});
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
    return std::make_unique<DisplayProcessor>();
}

struct ScopedFactory {
    ScopedFactory() : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(make_display_processor);
    }
    ~ScopedFactory() { pulp::format::register_plugin(previous); }
    pulp::format::ProcessorFactory previous;
};

}  // namespace

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

    // A param without to_string declines.
    AudioUnitParameterStringFromValue plain{};
    plain.inParamID = kPlainId;
    plain.inValue = &value;
    REQUIRE(effect.GetProperty(kAudioUnitProperty_ParameterStringFromValue,
                               kAudioUnitScope_Global, 0, &plain) != noErr);
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

    // A param without from_string declines.
    AudioUnitParameterValueFromString plain{};
    plain.inParamID = kPlainId;
    plain.inString = CFSTR("0.5");
    REQUIRE(effect.GetProperty(kAudioUnitProperty_ParameterValueFromString,
                               kAudioUnitScope_Global, 0, &plain) != noErr);
}
