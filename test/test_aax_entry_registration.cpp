// Which editor the generated AAX entry point registers with the host.
//
// `kAAX_ProcPtrID_Create_EffectGUI` is the switch: register a proc under it and
// Pro Tools instantiates the plugin's own editor; register nothing and the host
// draws its auto-generated parameter strip. The entry-point macros choose which
// of those a plugin gets, so this suite drives both macros through a recording
// collection and asserts the proc IDs that reach the descriptor.
//
// SDK-gated: the macros and `get_effect_descriptions` are written against the
// developer-supplied Avid AAX interfaces, so this builds only where those
// headers are configured. It needs the interface headers alone — no AAX library
// sources — because every type it fakes here is a pure abstract interface.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/aax_entry.hpp>
#include <pulp/format/processor.hpp>

#include <AAX_ICollection.h>
#include <AAX_IComponentDescriptor.h>
#include <AAX_IEffectDescriptor.h>
#include <AAX_IPropertyMap.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

class GainEffect final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "EntryGain",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.entry-gain",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2, false}},
            .output_buses = {{"Main Out", 2, false}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-24.0f, 24.0f, 0.0f, 0.1f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>&,
        const pulp::audio::BufferView<const float>&,
        pulp::midi::MidiBuffer&,
        pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {}
};

std::unique_ptr<pulp::format::Processor> make_gain() {
    return std::make_unique<GainEffect>();
}

/// Accepts every property without recording it: the entry point sets many, and
/// none of them are what this suite is about.
class FakePropertyMap final : public AAX_IPropertyMap {
public:
    AAX_Result AddProperty(AAX_EProperty, AAX_CPropertyValue) override { return AAX_SUCCESS; }
    AAX_Result AddPointerProperty(AAX_EProperty, const void*) override { return AAX_SUCCESS; }
    AAX_Result AddPointerProperty(AAX_EProperty, const char*) override { return AAX_SUCCESS; }
    AAX_Result AddPropertyWithIDArray(AAX_EProperty, const AAX_SPlugInIdentifierTriad*, uint32_t) override {
        return AAX_SUCCESS;
    }
    AAX_CBoolean GetProperty(AAX_EProperty, AAX_CPropertyValue*) const override { return 0; }
    AAX_CBoolean GetPointerProperty(AAX_EProperty, const void**) const override { return 0; }
    AAX_CBoolean GetPropertyWithIDArray(AAX_EProperty, const AAX_SPlugInIdentifierTriad**, uint32_t*) const override {
        return 0;
    }
    AAX_Result RemoveProperty(AAX_EProperty) override { return AAX_SUCCESS; }
    IACFUnknown* GetIUnknown() override { return nullptr; }
};

class FakeComponentDescriptor final : public AAX_IComponentDescriptor {
public:
    AAX_Result Clear() override { return AAX_SUCCESS; }
    AAX_IPropertyMap* NewPropertyMap() const override {
        maps_.push_back(std::make_unique<FakePropertyMap>());
        return maps_.back().get();
    }
    AAX_IPropertyMap* DuplicatePropertyMap(AAX_IPropertyMap*) const override { return NewPropertyMap(); }
    AAX_Result AddReservedField(AAX_CFieldIndex, uint32_t) override { return AAX_SUCCESS; }
    AAX_Result AddAudioIn(AAX_CFieldIndex) override { return AAX_SUCCESS; }
    AAX_Result AddAudioOut(AAX_CFieldIndex) override { return AAX_SUCCESS; }
    AAX_Result AddAudioBufferLength(AAX_CFieldIndex) override { return AAX_SUCCESS; }
    AAX_Result AddSampleRate(AAX_CFieldIndex) override { return AAX_SUCCESS; }
    AAX_Result AddClock(AAX_CFieldIndex) override { return AAX_SUCCESS; }
    AAX_Result AddSideChainIn(AAX_CFieldIndex) override { return AAX_SUCCESS; }
    AAX_Result AddDataInPort(AAX_CFieldIndex, uint32_t, AAX_EDataInPortType) override { return AAX_SUCCESS; }
    AAX_Result AddAuxOutputStem(AAX_CFieldIndex, int32_t, const char[]) override { return AAX_SUCCESS; }
    AAX_Result AddPrivateData(AAX_CFieldIndex, int32_t, uint32_t) override { return AAX_SUCCESS; }
    AAX_Result AddTemporaryData(AAX_CFieldIndex, uint32_t) override { return AAX_SUCCESS; }
    AAX_Result AddDmaInstance(AAX_CFieldIndex, AAX_IDma::EMode) override { return AAX_SUCCESS; }
    AAX_Result AddMIDINode(AAX_CFieldIndex, AAX_EMIDINodeType, const char[], uint32_t) override {
        return AAX_SUCCESS;
    }
    AAX_Result AddProcessProc_Native(AAX_CProcessProc, AAX_IPropertyMap*, AAX_CInstanceInitProc,
                                     AAX_CBackgroundProc, AAX_CSelector*) override {
        return AAX_SUCCESS;
    }
    AAX_Result AddProcessProc_TI(const char[], const char[], AAX_IPropertyMap*, const char[], const char[],
                                 AAX_CSelector*) override {
        return AAX_SUCCESS;
    }
    AAX_Result AddProcessProc(AAX_IPropertyMap*, AAX_CSelector*, int32_t) override { return AAX_SUCCESS; }
    AAX_Result AddMeters(AAX_CFieldIndex, const AAX_CTypeID*, const uint32_t) override { return AAX_SUCCESS; }

private:
    mutable std::vector<std::unique_ptr<FakePropertyMap>> maps_;
};

/// Records the proc IDs the entry point registers. The proc IDs are the whole
/// point of the suite; everything else is accepted and discarded.
class FakeEffectDescriptor final : public AAX_IEffectDescriptor {
public:
    std::vector<AAX_CProcPtrID> proc_ids;

    AAX_IComponentDescriptor* NewComponentDescriptor() override {
        components_.push_back(std::make_unique<FakeComponentDescriptor>());
        return components_.back().get();
    }
    AAX_Result AddComponent(AAX_IComponentDescriptor*) override { return AAX_SUCCESS; }
    AAX_Result AddName(const char*) override { return AAX_SUCCESS; }
    AAX_Result AddCategory(uint32_t) override { return AAX_SUCCESS; }
    AAX_Result SetRole(uint32_t) override { return AAX_SUCCESS; }
    AAX_Result AddCategoryBypassParameter(uint32_t, AAX_CParamID) override { return AAX_SUCCESS; }
    AAX_Result AddProcPtr(void*, AAX_CProcPtrID inProcID) override {
        proc_ids.push_back(inProcID);
        return AAX_SUCCESS;
    }
    AAX_IPropertyMap* NewPropertyMap() override {
        maps_.push_back(std::make_unique<FakePropertyMap>());
        return maps_.back().get();
    }
    AAX_Result SetProperties(AAX_IPropertyMap*) override { return AAX_SUCCESS; }
    AAX_Result AddResourceInfo(AAX_EResourceType, const char*) override { return AAX_SUCCESS; }
    AAX_Result AddMeterDescription(AAX_CTypeID, const char*, AAX_IPropertyMap*) override { return AAX_SUCCESS; }
    AAX_Result AddControlMIDINode(AAX_CTypeID, AAX_EMIDINodeType, const char[], uint32_t) override {
        return AAX_SUCCESS;
    }

    bool registered_gui() const {
        return std::find(proc_ids.begin(), proc_ids.end(),
                         static_cast<AAX_CProcPtrID>(kAAX_ProcPtrID_Create_EffectGUI)) != proc_ids.end();
    }
    bool registered_parameters() const {
        return std::find(proc_ids.begin(), proc_ids.end(),
                         static_cast<AAX_CProcPtrID>(kAAX_ProcPtrID_Create_EffectParameters)) != proc_ids.end();
    }

private:
    std::vector<std::unique_ptr<FakeComponentDescriptor>> components_;
    std::vector<std::unique_ptr<FakePropertyMap>> maps_;
};

class FakeCollection final : public AAX_ICollection {
public:
    FakeEffectDescriptor descriptor;

    AAX_IEffectDescriptor* NewDescriptor() override { return &descriptor; }
    AAX_Result AddEffect(const char*, AAX_IEffectDescriptor*) override { return AAX_SUCCESS; }
    AAX_Result SetManufacturerName(const char*) override { return AAX_SUCCESS; }
    AAX_Result AddPackageName(const char*) override { return AAX_SUCCESS; }
    AAX_Result SetPackageVersion(uint32_t) override { return AAX_SUCCESS; }
    AAX_IPropertyMap* NewPropertyMap() override {
        maps_.push_back(std::make_unique<FakePropertyMap>());
        return maps_.back().get();
    }
    AAX_Result SetProperties(AAX_IPropertyMap*) override { return AAX_SUCCESS; }
    AAX_Result GetHostVersion(uint32_t*) const override { return AAX_SUCCESS; }
    AAX_IDescriptionHost* DescriptionHost() override { return nullptr; }
    const AAX_IDescriptionHost* DescriptionHost() const override { return nullptr; }
    IACFDefinition* HostDefinition() const override { return nullptr; }

private:
    std::vector<std::unique_ptr<FakePropertyMap>> maps_;
};

} // namespace

// Two entry points in one binary. Each named namespace gets its own
// GetEffectDescriptions from the macro under test, so a single suite can drive
// the default and the opt-in side by side.
namespace default_entry {
PULP_AAX_PLUGIN(make_gain)
} // namespace default_entry

namespace gui_entry {
PULP_AAX_PLUGIN_WITH_GUI(make_gain)
} // namespace gui_entry

TEST_CASE("PULP_AAX_PLUGIN leaves the host on its parameter strip", "[aax][entry]") {
    FakeCollection collection;
    REQUIRE(default_entry::GetEffectDescriptions(&collection) == AAX_SUCCESS);

    // No editor proc: this is what keeps a rebuilt plugin on the working strip.
    REQUIRE_FALSE(collection.descriptor.registered_gui());
    // The data model is still registered — the default drops the editor only.
    REQUIRE(collection.descriptor.registered_parameters());
}

TEST_CASE("PULP_AAX_PLUGIN_WITH_GUI registers the custom editor", "[aax][entry]") {
    FakeCollection collection;
    REQUIRE(gui_entry::GetEffectDescriptions(&collection) == AAX_SUCCESS);

    REQUIRE(collection.descriptor.registered_gui());
    REQUIRE(collection.descriptor.registered_parameters());
}

TEST_CASE("the entry macros differ only in the editor proc", "[aax][entry]") {
    FakeCollection strip;
    FakeCollection gui;
    REQUIRE(default_entry::GetEffectDescriptions(&strip) == AAX_SUCCESS);
    REQUIRE(gui_entry::GetEffectDescriptions(&gui) == AAX_SUCCESS);

    // Dropping the editor proc must leave every other registration untouched.
    auto without_gui = gui.descriptor.proc_ids;
    without_gui.erase(std::remove(without_gui.begin(), without_gui.end(),
                                  static_cast<AAX_CProcPtrID>(kAAX_ProcPtrID_Create_EffectGUI)),
                      without_gui.end());
    REQUIRE(without_gui == strip.descriptor.proc_ids);
}
