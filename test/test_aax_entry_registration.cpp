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
#include <pulp/format/aax_runtime_testing.hpp>
#include <pulp/format/processor.hpp>

#include <AAX_ICollection.h>
#include <AAX_IComponentDescriptor.h>
#include <AAX_IEffectDescriptor.h>
#include <AAX_IPropertyMap.h>

#include <algorithm>
#include <cstddef>
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
  std::vector<std::pair<AAX_EProperty, AAX_CPropertyValue>> properties;
  AAX_Result AddProperty(AAX_EProperty property, AAX_CPropertyValue value) override {
      properties.emplace_back(property, value);
      return AAX_SUCCESS;
  }
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
  mutable std::vector<std::unique_ptr<FakePropertyMap>> maps_;
  FakePropertyMap* process_properties = nullptr;
  AAX_Result Clear() override {
      return AAX_SUCCESS;
  }
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
    AAX_Result AddProcessProc_Native(AAX_CProcessProc, AAX_IPropertyMap* properties,
                                     AAX_CInstanceInitProc, AAX_CBackgroundProc,
                                     AAX_CSelector*) override {
        process_properties = static_cast<FakePropertyMap*>(properties);
        return AAX_SUCCESS;
    }
    AAX_Result AddProcessProc_TI(const char[], const char[], AAX_IPropertyMap*, const char[], const char[],
                                 AAX_CSelector*) override {
        return AAX_SUCCESS;
    }
    AAX_Result AddProcessProc(AAX_IPropertyMap*, AAX_CSelector*, int32_t) override { return AAX_SUCCESS; }
    AAX_Result AddMeters(AAX_CFieldIndex, const AAX_CTypeID*, const uint32_t) override { return AAX_SUCCESS; }

};

/// Records the proc IDs the entry point registers. The proc IDs are the whole
/// point of the suite; everything else is accepted and discarded.
class FakeEffectDescriptor final : public AAX_IEffectDescriptor {
public:
    std::vector<AAX_CProcPtrID> proc_ids;
    std::vector<std::unique_ptr<FakeComponentDescriptor>> components_;
    std::vector<std::unique_ptr<FakePropertyMap>> maps_;
    FakePropertyMap* effect_properties = nullptr;

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
    AAX_Result SetProperties(AAX_IPropertyMap* properties) override {
        effect_properties = static_cast<FakePropertyMap*>(properties);
        return AAX_SUCCESS;
    }
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

TEST_CASE("AAX component properties identify the GUI lane and data-model placement",
          "[aax][entry]") {
    FakeCollection strip;
    REQUIRE(default_entry::GetEffectDescriptions(&strip) == AAX_SUCCESS);
    REQUIRE(strip.descriptor.components_.size() == 1);
    const auto& strip_props = strip.descriptor.components_.front()->process_properties->properties;
    const auto property_value = [&](const auto& props, AAX_EProperty property) {
        const auto it = std::find_if(props.begin(), props.end(), [property](const auto& item) {
            return item.first == property;
        });
        REQUIRE(it != props.end());
        return it->second;
    };
    REQUIRE(property_value(strip_props, AAX_eProperty_UsesClientGUI) == 1);
    REQUIRE(property_value(strip_props, AAX_eProperty_Constraint_Location) ==
            AAX_eConstraintLocationMask_DataModel);

    FakeCollection gui;
    REQUIRE(gui_entry::GetEffectDescriptions(&gui) == AAX_SUCCESS);
    const auto& gui_props = gui.descriptor.components_.front()->process_properties->properties;
    REQUIRE(property_value(gui_props, AAX_eProperty_UsesClientGUI) == 0);
    REQUIRE(property_value(gui_props, AAX_eProperty_Constraint_Location) ==
            AAX_eConstraintLocationMask_DataModel);
}

TEST_CASE("AAX host-generated GUI request is only set for the strip lane", "[aax][entry]") {
    FakeCollection strip;
    REQUIRE(default_entry::GetEffectDescriptions(&strip) == AAX_SUCCESS);
    REQUIRE(strip.descriptor.effect_properties != nullptr);
    REQUIRE(strip.descriptor.effect_properties->properties.size() == 3);
    const auto strip_gui =
        std::find_if(strip.descriptor.effect_properties->properties.begin(),
                     strip.descriptor.effect_properties->properties.end(),
                     [](const auto& item) { return item.first == AAX_eProperty_UsesClientGUI; });
    REQUIRE(strip_gui != strip.descriptor.effect_properties->properties.end());
    REQUIRE(strip_gui->second == 1);

    FakeCollection gui;
    REQUIRE(gui_entry::GetEffectDescriptions(&gui) == AAX_SUCCESS);
    REQUIRE(gui.descriptor.effect_properties != nullptr);
    const auto gui_host_request =
        std::find_if(gui.descriptor.effect_properties->properties.begin(),
                     gui.descriptor.effect_properties->properties.end(),
                     [](const auto& item) { return item.first == AAX_eProperty_UsesClientGUI; });
    REQUIRE(gui_host_request == gui.descriptor.effect_properties->properties.end());
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

// ── Editor <-> audio-owner binding ──────────────────────────────────────────
//
// AAX keeps a data model and an algorithm instance; the model constructs the
// instance's Processor itself (ResetFieldData), so the editor can run on the
// Processor that renders audio. These drive that lifecycle the way the host
// does, with no window and no audio.

namespace {

struct ModelHandle {
    IACFUnknown* unknown = nullptr;
    pulp::format::aax::EditorHost* host = nullptr;
    explicit ModelHandle(const pulp::format::aax::EntryConfig& config)
        : unknown(pulp::format::aax::create_effect_parameters(config)) {
        REQUIRE(unknown != nullptr);
        unknown->AddRef();
        host = pulp::format::aax::testing::editor_host(unknown);
        REQUIRE(host != nullptr);
    }
    ~ModelHandle() {
        if (unknown)
            unknown->Release();
    }
    void release() {
        if (unknown)
            unknown->Release();
        unknown = nullptr;
        host = nullptr;
    }
};

pulp::format::aax::EntryConfig binding_config() {
    return pulp::format::aax::make_entry_config(&make_gain, "PulP", "Gain", "GnNt");
}

} // namespace

TEST_CASE("AAX editor binds to the processor that renders audio", "[aax][entry][view-bridge]") {
    using namespace pulp::format::aax::testing;
    ModelHandle model(binding_config());
    std::vector<std::byte> block(private_data_block_size());

    REQUIRE(reset_private_data(model.unknown, block.data(), block.size()) == AAX_SUCCESS);
    auto* audio_owner = algorithm_processor(block.data());
    REQUIRE(audio_owner != nullptr);

    CHECK(model.host->editor_processor() == audio_owner);
    CHECK(model.host->editor_store() == algorithm_store(block.data()));

    // An editor edit lands in the store the audio thread reads.
    model.host->editor_store()->set_value(1, 6.0f);
    CHECK(algorithm_store(block.data())->get_value(1) == 6.0f);

    // A host reset re-prepares in place: the editor's Processor survives.
    REQUIRE(instance_init_for_test(block.data(), AAX_eComponentInstanceInitAction_ResetInstance) ==
            0);
    CHECK(algorithm_processor(block.data()) == audio_owner);
    CHECK(model.host->editor_processor() == audio_owner);

    // Removing the instance invalidates the editor BEFORE the Processor dies.
    bool invalidated = false;
    pulp::format::Processor* seen_at_invalidation = nullptr;
    model.host->set_editor_invalidation_handler([&] {
        invalidated = true;
        seen_at_invalidation = algorithm_processor(block.data());
    });
    REQUIRE(instance_init_for_test(block.data(),
                                   AAX_eComponentInstanceInitAction_RemovingInstance) == 0);
    CHECK(invalidated);
    CHECK(seen_at_invalidation == audio_owner);
    CHECK(algorithm_processor(block.data()) == nullptr);

    // The model falls back to its own Processor for a later open. The
    // allocator may hand the fallback the dead instance's address, so prove
    // it is a fresh pair by its state: the audio store held +6 dB.
    auto* fallback = model.host->editor_processor();
    REQUIRE(fallback != nullptr);
    REQUIRE(model.host->editor_store() != nullptr);
    CHECK(model.host->editor_store()->get_value(1) == 0.0f);
}

TEST_CASE("AAX editor opened before any algorithm instance keeps its fallback",
          "[aax][entry][view-bridge]") {
    using namespace pulp::format::aax::testing;
    ModelHandle model(binding_config());
    auto* fallback = model.host->editor_processor();
    REQUIRE(fallback != nullptr);

    std::vector<std::byte> block(private_data_block_size());
    REQUIRE(reset_private_data(model.unknown, block.data(), block.size()) == AAX_SUCCESS);
    REQUIRE(algorithm_processor(block.data()) != nullptr);
    // No live rebind: an open editor's view tree points at the fallback.
    CHECK(model.host->editor_processor() == fallback);

    bool invalidated = false;
    model.host->set_editor_invalidation_handler([&] { invalidated = true; });
    REQUIRE(instance_init_for_test(block.data(),
                                   AAX_eComponentInstanceInitAction_RemovingInstance) == 0);
    CHECK_FALSE(invalidated); // the fallback was never bound to the instance
    CHECK(model.host->editor_processor() == fallback);
}

TEST_CASE("AAX model destroyed before its algorithm instance leaves the instance intact",
          "[aax][entry][view-bridge]") {
    using namespace pulp::format::aax::testing;
    std::vector<std::byte> block(private_data_block_size());
    {
        ModelHandle model(binding_config());
        REQUIRE(reset_private_data(model.unknown, block.data(), block.size()) == AAX_SUCCESS);
        REQUIRE(model.host->editor_processor() == algorithm_processor(block.data()));
        model.release();
    }
    auto* audio_owner = algorithm_processor(block.data());
    REQUIRE(audio_owner != nullptr);
    // The store the dead model subscribed to is still usable...
    algorithm_store(block.data())->set_value(1, -3.0f);
    CHECK(algorithm_store(block.data())->get_value(1) == -3.0f);
    // ...and the instance tears down on its own.
    REQUIRE(instance_init_for_test(block.data(),
                                   AAX_eComponentInstanceInitAction_RemovingInstance) == 0);
    CHECK(algorithm_processor(block.data()) == nullptr);
}
