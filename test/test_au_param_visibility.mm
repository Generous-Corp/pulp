// AU projection of the shared hidden / read-only / non-automatable parameter
// attributes.
//
// `ParamInfo` carries the three attributes and every format adapter reads the
// same predicates, so these tests assert the AU side of that contract: the v2
// `AudioUnitParameterInfo` flags returned by the real `GetParameterInfo` entry
// point, and the v3 `AUParameter.flags` the host observes on the parameter
// tree. AU has no literal hidden/readonly/automatable triple, so each attribute
// maps to the flag whose documented meaning matches (see au_v2_common.cpp).
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/state/parameter.hpp>

#import "../core/format/src/au_audio_unit.h"

#include <memory>

namespace {

constexpr pulp::state::ParamID kVisibleId = 1;
constexpr pulp::state::ParamID kHiddenId = 2;
constexpr pulp::state::ParamID kMeterId = 3;
constexpr pulp::state::ParamID kNoAutoId = 4;

// One processor declaring the three attributes alongside an ordinary
// parameter, so each projection is asserted against a live adapter rather than
// against the predicate it was built from.
class VisibilityProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "AuVisibility",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au.visibility",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = kVisibleId, .name = "Gain",
                             .range = {0.0f, 1.0f, 0.5f, 0.0f}});

        pulp::state::ParamInfo hidden{.id = kHiddenId, .name = "Internal",
                                      .range = {0.0f, 1.0f, 0.0f, 0.0f}};
        hidden.hidden = true;
        store.add_parameter(hidden);

        pulp::state::ParamInfo meter{.id = kMeterId, .name = "Output Level",
                                     .unit = "dB",
                                     .range = {-60.0f, 0.0f, -60.0f, 0.0f}};
        meter.read_only = true;
        store.add_parameter(meter);

        pulp::state::ParamInfo no_auto{.id = kNoAutoId, .name = "Quality",
                                       .range = {0.0f, 1.0f, 0.0f, 0.0f}};
        no_auto.automatable = false;
        store.add_parameter(no_auto);
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

std::unique_ptr<pulp::format::Processor> create_visibility_processor() {
    return std::make_unique<VisibilityProcessor>();
}

struct ScopedFactoryRegistration {
    explicit ScopedFactoryRegistration(pulp::format::ProcessorFactory factory)
        : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(factory);
    }
    ~ScopedFactoryRegistration() { pulp::format::register_plugin(previous); }
    pulp::format::ProcessorFactory previous;
};

}  // namespace

TEST_CASE("AU v2 GetParameterInfo projects declared hidden and read-only parameters",
          "[au][auv2][params][visibility]") {
    ScopedFactoryRegistration registration(create_visibility_processor);
    pulp::format::au::PulpAUEffect effect(nullptr);

    const auto info_for = [&effect](pulp::state::ParamID id) {
        AudioUnitParameterInfo info{};
        REQUIRE(effect.GetParameterInfo(kAudioUnitScope_Global,
                                        static_cast<AudioUnitParameterID>(id),
                                        info) == noErr);
        return info;
    };

    SECTION("an undeclared parameter is unchanged") {
        const auto info = info_for(kVisibleId);
        REQUIRE((info.flags & kAudioUnitParameterFlag_IsWritable) != 0);
        REQUIRE((info.flags & kAudioUnitParameterFlag_IsReadable) != 0);
        REQUIRE((info.flags & kAudioUnitParameterFlag_MeterReadOnly) == 0);
        REQUIRE((info.flags & kAudioUnitParameterFlag_ExpertMode) == 0);
        REQUIRE((info.flags & kAudioUnitParameterFlag_NonRealTime) == 0);
    }

    SECTION("a hidden parameter takes the expert-mode display hint") {
        const auto info = info_for(kHiddenId);
        REQUIRE((info.flags & kAudioUnitParameterFlag_ExpertMode) != 0);
    }

    SECTION("a read-only parameter withholds IsWritable and reads as a meter") {
        const auto info = info_for(kMeterId);
        REQUIRE((info.flags & kAudioUnitParameterFlag_IsWritable) == 0);
        REQUIRE((info.flags & kAudioUnitParameterFlag_MeterReadOnly) != 0);
        // Still readable — the host renders the published value.
        REQUIRE((info.flags & kAudioUnitParameterFlag_IsReadable) != 0);
    }

    SECTION("a non-automatable parameter is flagged NonRealTime but stays writable") {
        const auto info = info_for(kNoAutoId);
        REQUIRE((info.flags & kAudioUnitParameterFlag_NonRealTime) != 0);
        REQUIRE((info.flags & kAudioUnitParameterFlag_IsWritable) != 0);
    }
}

TEST_CASE("AU v3 parameter tree projects declared hidden and read-only parameters",
          "[au][auv3][params][visibility]") {
    ScopedFactoryRegistration registration(create_visibility_processor);

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Effect;
    desc.componentSubType = 'TstV';
    desc.componentManufacturer = 'Plup';

    NSError* error = nil;
    PulpAudioUnit* unit =
        [[PulpAudioUnit alloc] initWithComponentDescription:desc options:0 error:&error];
    REQUIRE(unit != nil);
    REQUIRE(error == nil);

    AUParameterTree* tree = unit.parameterTree;
    REQUIRE(tree != nil);

    const auto flags_for = [tree](pulp::state::ParamID id) {
        AUParameter* param =
            [tree parameterWithAddress:static_cast<AUParameterAddress>(id)];
        REQUIRE(param != nil);
        return param.flags;
    };

    const auto visible = flags_for(kVisibleId);
    REQUIRE((visible & kAudioUnitParameterFlag_IsWritable) != 0);
    REQUIRE((visible & kAudioUnitParameterFlag_ExpertMode) == 0);
    REQUIRE((visible & kAudioUnitParameterFlag_MeterReadOnly) == 0);
    REQUIRE((visible & kAudioUnitParameterFlag_NonRealTime) == 0);

    REQUIRE((flags_for(kHiddenId) & kAudioUnitParameterFlag_ExpertMode) != 0);

    const auto meter = flags_for(kMeterId);
    REQUIRE((meter & kAudioUnitParameterFlag_MeterReadOnly) != 0);
    REQUIRE((meter & kAudioUnitParameterFlag_IsWritable) == 0);
    REQUIRE((meter & kAudioUnitParameterFlag_IsReadable) != 0);

    const auto no_auto = flags_for(kNoAutoId);
    REQUIRE((no_auto & kAudioUnitParameterFlag_NonRealTime) != 0);
    REQUIRE((no_auto & kAudioUnitParameterFlag_IsWritable) != 0);

    [unit release];
}
