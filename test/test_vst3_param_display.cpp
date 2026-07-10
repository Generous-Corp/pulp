// VST3 parameter display-string round-trip (WS-4 / G5).
//
// The gap this closes: PulpVst3Processor did not override
// getParamStringByValue / getParamValueByString, so a plugin's
// ParamInfo::to_string / from_string were IGNORED and the host fell back to
// SingleComponentEffect's stock numeric formatting. These tests drive the two
// overrides directly and assert:
//   * a normalized value is denormalized through the ParamRange, formatted by
//     to_string, and returned;
//   * a typed string is parsed by from_string and normalized back;
//   * parameters WITHOUT converters still get the base numeric formatting
//     (the override declines to the base class), so existing plugins are
//     unchanged.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/processor.hpp>

#include <pluginterfaces/base/ustring.h>
#include <pluginterfaces/vst/ivsthostapplication.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using Catch::Matchers::WithinAbs;

namespace {

constexpr pulp::state::ParamID kGainId = 1;   // dB, custom to/from string
constexpr pulp::state::ParamID kPlainId = 2;  // no converters (base fallback)

// A minimal processor with one "smart" parameter (custom display) and one
// plain parameter (no converters).
class DisplayProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "VstDisplay",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.vst3.display",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo gain{.id = kGainId, .name = "Gain", .unit = "dB",
                                    .range = {-60.0f, 24.0f, 0.0f, 0.0f}};
        gain.to_string = [](float v) {
            char b[32];
            std::snprintf(b, sizeof(b), "%.1f dB", v);
            return std::string(b);
        };
        gain.from_string = [](const std::string& s) {
            return static_cast<float>(std::atof(s.c_str()));
        };
        store.add_parameter(gain);

        // No to_string / from_string — must fall back to the base formatter.
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

class HostApp final : public Steinberg::Vst::IHostApplication {
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        Steinberg::UString(name, 128).fromAscii("PulpTest");
        return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID,
                                                 void** obj) override {
        if (obj) *obj = nullptr;
        return Steinberg::kNotImplemented;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
};

std::string to_ascii(const Steinberg::Vst::String128 s) {
    char buf[256] = {0};
    Steinberg::UString(const_cast<Steinberg::Vst::TChar*>(s), 128)
        .toAscii(buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

TEST_CASE("VST3 getParamStringByValue uses ParamInfo::to_string",
          "[vst3][params][display]") {
    HostApp host;
    pulp::format::vst3::PulpVst3Processor processor(make_display_processor);
    REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);

    // Gain range [-60, 24]; normalized 0.0 -> -60 dB, 1.0 -> +24 dB.
    Steinberg::Vst::String128 out{};
    REQUIRE(processor.getParamStringByValue(kGainId, 0.0, out) == Steinberg::kResultTrue);
    REQUIRE(to_ascii(out) == "-60.0 dB");

    REQUIRE(processor.getParamStringByValue(kGainId, 1.0, out) == Steinberg::kResultTrue);
    REQUIRE(to_ascii(out) == "24.0 dB");

    // Normalized 0.5 -> midpoint of [-60, 24] = -18 dB.
    REQUIRE(processor.getParamStringByValue(kGainId, 0.5, out) == Steinberg::kResultTrue);
    REQUIRE(to_ascii(out) == "-18.0 dB");

    processor.terminate();
}

TEST_CASE("VST3 getParamValueByString uses ParamInfo::from_string",
          "[vst3][params][display]") {
    HostApp host;
    pulp::format::vst3::PulpVst3Processor processor(make_display_processor);
    REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);

    // "-18 dB" -> plain -18 -> normalized (−18 − (−60)) / 84 = 0.5.
    Steinberg::Vst::TChar in[128] = {0};
    Steinberg::UString(in, 128).fromAscii("-18 dB");
    Steinberg::Vst::ParamValue norm = 0.0;
    REQUIRE(processor.getParamValueByString(kGainId, in, norm) == Steinberg::kResultTrue);
    REQUIRE_THAT(norm, WithinAbs(0.5, 1e-5));

    // Round-trip: string -> value -> string.
    Steinberg::Vst::String128 out{};
    REQUIRE(processor.getParamStringByValue(kGainId, norm, out) == Steinberg::kResultTrue);
    REQUIRE(to_ascii(out) == "-18.0 dB");

    processor.terminate();
}

TEST_CASE("VST3 param display declines to the base formatter without converters",
          "[vst3][params][display]") {
    HostApp host;
    pulp::format::vst3::PulpVst3Processor processor(make_display_processor);
    REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);

    // The "Plain" param has no to_string; the override must defer to the base
    // class, which still produces *some* valid string (not our "%.1f dB" form).
    Steinberg::Vst::String128 out{};
    REQUIRE(processor.getParamStringByValue(kPlainId, 0.5, out) == Steinberg::kResultTrue);
    REQUIRE(to_ascii(out).find("dB") == std::string::npos);

    processor.terminate();
}
