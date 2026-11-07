// VST3 publishes a plug-in's factory presets as a program list (IUnitInfo) and
// lets the host select one through the program-change parameter. These tests
// drive the adapter the way a host does: through IUnitInfo, IEditController and
// IAudioProcessor, with preset files staged in a temporary folder.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/vst3_program_list.hpp>
#include <pulp/format/vst3_adapter.hpp>

#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <public.sdk/source/common/memorystream.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format;
namespace fs = std::filesystem;

namespace {

constexpr state::ParamID kLevel = 1;
constexpr state::ParamID kMix = 2;
constexpr auto kProgramParam = detail::kVst3ProgramChangeParamId;

class HostApp final : public Steinberg::Vst::IHostApplication {
  public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* kName = "PulpTest";
        for (int i = 0; i < 8; ++i)
            name[i] = static_cast<Steinberg::Vst::TChar>(kName[i]);
        name[8] = 0;
        return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID,
                                                 void** obj) override {
        if (obj)
            *obj = nullptr;
        return Steinberg::kNotImplemented;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        return 1;
    }
};

/// Counts the restarts the adapter asks the host for.
class CountingHandler final : public Steinberg::Vst::IComponentHandler {
  public:
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID,
                                              Steinberg::Vst::ParamValue) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
        restart_flags |= flags;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        return 1;
    }

    Steinberg::int32 restart_flags = 0;
};

std::vector<NoteName>& shared_note_names() {
    static std::vector<NoteName> names;
    return names;
}

/// The values the processor read on its last block, as the audio thread sees them.
std::atomic<float> g_level_seen{-1.0f};
std::atomic<float> g_mix_seen{-1.0f};

class PresetProcessor : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        return {
            .name = "ProgramList",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.programlist",
            .version = "1.0.0",
            .category = PluginCategory::Effect,
            .input_buses = {{"In", 2}},
            .output_buses = {{"Out", 2}},
        };
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kLevel, .name = "Level", .range = {0.0f, 1.0f, 0.5f}});
        store.add_parameter({.id = kMix, .name = "Mix", .range = {0.0f, 1.0f, 0.5f}});
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const ProcessContext&) override {
        g_level_seen.store(state().get_value(kLevel));
        g_mix_seen.store(state().get_value(kMix));
    }
    std::vector<NoteName> note_names() const override {
        return shared_note_names();
    }
};

std::unique_ptr<Processor> make_processor() {
    return std::make_unique<PresetProcessor>();
}

/// A temporary factory-preset folder, removed when the test ends.
class PresetFolder {
  public:
    PresetFolder() {
        static std::atomic<int> counter{0};
        dir_ = fs::temp_directory_path() /
               ("pulp-vst3-program-list-" + std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
    }
    ~PresetFolder() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    void add(const std::string& name, float level, float mix) const {
        std::ofstream out(dir_ / (name + ".json"));
        out << "{\n  \"name\": \"" << name << "\",\n  \"parameters\": {\"Level\": " << level
            << ", \"Mix\": " << mix << "}\n}\n";
    }
    void add_raw(const std::string& file, const std::string& content) const {
        std::ofstream out(dir_ / file);
        out << content;
    }
    const fs::path& path() const {
        return dir_;
    }

  private:
    fs::path dir_;
};

std::string to_string(const Steinberg::Vst::String128 text) {
    std::string out;
    for (int i = 0; i < 128 && text[i] != 0; ++i)
        out.push_back(static_cast<char>(text[i]));
    return out;
}

/// A processor ready for a host: presets staged, initialized, and (optionally)
/// running so process() can be driven.
struct Fixture {
    HostApp host;
    CountingHandler handler;
    vst3::PulpVst3Processor processor{&make_processor};

    explicit Fixture(const fs::path& presets) {
        shared_note_names().clear();
        processor.set_factory_presets_directory(presets);
        REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);
        processor.setComponentHandler(&handler);
    }
    ~Fixture() {
        processor.terminate();
    }

    void activate() {
        Steinberg::Vst::SpeakerArrangement in_arr[1] = {Steinberg::Vst::SpeakerArr::kStereo};
        Steinberg::Vst::SpeakerArrangement out_arr[1] = {Steinberg::Vst::SpeakerArr::kStereo};
        REQUIRE(processor.setBusArrangements(in_arr, 1, out_arr, 1) == Steinberg::kResultTrue);
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = 64;
        setup.sampleRate = 48000.0;
        REQUIRE(processor.setupProcessing(setup) == Steinberg::kResultOk);
        REQUIRE(processor.setActive(true) == Steinberg::kResultOk);
    }

    /// One block carrying a program-change point.
    void process_program_change(Steinberg::Vst::ParamValue normalized) {
        float in_l[64] = {}, in_r[64] = {}, out_l[64] = {}, out_r[64] = {};
        float* in_ptrs[2] = {in_l, in_r};
        float* out_ptrs[2] = {out_l, out_r};
        Steinberg::Vst::AudioBusBuffers audio_in{};
        audio_in.numChannels = 2;
        audio_in.channelBuffers32 = in_ptrs;
        Steinberg::Vst::AudioBusBuffers audio_out{};
        audio_out.numChannels = 2;
        audio_out.channelBuffers32 = out_ptrs;

        Steinberg::Vst::ParameterChanges changes(2);
        Steinberg::int32 queue_index = 0;
        auto* queue = changes.addParameterData(kProgramParam, queue_index);
        REQUIRE(queue != nullptr);
        Steinberg::int32 point = 0;
        REQUIRE(queue->addPoint(8, normalized, point) == Steinberg::kResultTrue);

        Steinberg::Vst::ProcessData data{};
        data.processMode = Steinberg::Vst::kRealtime;
        data.symbolicSampleSize = Steinberg::Vst::kSample32;
        data.numSamples = 64;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &audio_in;
        data.outputs = &audio_out;
        data.inputParameterChanges = &changes;
        REQUIRE(processor.process(data) == Steinberg::kResultOk);
    }
};

Steinberg::Vst::ParameterInfo find_parameter(vst3::PulpVst3Processor& processor,
                                             Steinberg::Vst::ParamID id, bool& found) {
    Steinberg::Vst::ParameterInfo info{};
    found = false;
    for (Steinberg::int32 i = 0; i < processor.getParameterCount(); ++i) {
        REQUIRE(processor.getParameterInfo(i, info) == Steinberg::kResultOk);
        if (info.id == id) {
            found = true;
            return info;
        }
    }
    return {};
}

} // namespace

TEST_CASE("program index arithmetic maps the normalized value onto the list", "[vst3][program]") {
    using detail::vst3_normalized_from_program_index;
    using detail::vst3_program_index_from_normalized;

    REQUIRE(vst3_program_index_from_normalized(0.0, 4) == 0);
    REQUIRE(vst3_program_index_from_normalized(1.0, 4) == 3);
    REQUIRE(vst3_program_index_from_normalized(1.0 / 3.0, 4) == 1);
    // Out of range and non-finite input stays inside the list.
    REQUIRE(vst3_program_index_from_normalized(-2.0, 4) == 0);
    REQUIRE(vst3_program_index_from_normalized(7.0, 4) == 3);
    REQUIRE(vst3_program_index_from_normalized(std::numeric_limits<double>::quiet_NaN(), 4) == 0);
    REQUIRE(vst3_program_index_from_normalized(0.5, 0) == 0);
    REQUIRE(vst3_program_index_from_normalized(0.5, 1) == 0);
    // Every index round-trips.
    for (std::size_t i = 0; i < 9; ++i)
        REQUIRE(vst3_program_index_from_normalized(vst3_normalized_from_program_index(i, 9), 9) ==
                i);
}

TEST_CASE("VST3 publishes no program list when the plugin ships no factory presets",
          "[vst3][program]") {
    PresetFolder empty;
    Fixture fx(empty.path());

    REQUIRE(fx.processor.getProgramListCount() == 0);
    Steinberg::Vst::ProgramListInfo list{};
    REQUIRE(fx.processor.getProgramListInfo(0, list) == Steinberg::kResultFalse);

    Steinberg::Vst::UnitInfo root{};
    REQUIRE(fx.processor.getUnitInfo(0, root) == Steinberg::kResultOk);
    REQUIRE(root.programListId == Steinberg::Vst::kNoProgramListId);

    bool found = true;
    find_parameter(fx.processor, kProgramParam, found);
    REQUIRE_FALSE(found);
}

TEST_CASE("VST3 program list count and names match the factory presets", "[vst3][program]") {
    PresetFolder folder;
    folder.add("Warm", 0.25f, 0.75f);
    folder.add("Bright", 0.9f, 0.1f);
    folder.add("Init", 0.5f, 0.5f);
    // A file with no parameter section is not a preset and is left out.
    folder.add_raw("Broken.json", "{ \"name\": \"Broken\" }");

    Fixture fx(folder.path());

    REQUIRE(fx.processor.getProgramListCount() == 1);
    Steinberg::Vst::ProgramListInfo list{};
    REQUIRE(fx.processor.getProgramListInfo(0, list) == Steinberg::kResultOk);
    REQUIRE(list.programCount == 3);
    REQUIRE(to_string(list.name) == "Factory Presets");
    REQUIRE(fx.processor.getProgramListInfo(1, list) == Steinberg::kResultFalse);
    REQUIRE(fx.processor.getProgramListInfo(-1, list) == Steinberg::kResultFalse);

    // The order is the one PresetManager::factory_presets() returns: by name.
    const std::vector<std::string> expected{"Bright", "Init", "Warm"};
    for (Steinberg::int32 i = 0; i < 3; ++i) {
        Steinberg::Vst::String128 name{};
        REQUIRE(fx.processor.getProgramName(list.id, i, name) == Steinberg::kResultOk);
        REQUIRE(to_string(name) == expected[static_cast<std::size_t>(i)]);

        Steinberg::Vst::String128 attribute{};
        REQUIRE(fx.processor.getProgramInfo(list.id, i, "Name", attribute) == Steinberg::kResultOk);
        REQUIRE(to_string(attribute) == expected[static_cast<std::size_t>(i)]);
    }

    // Indexes and lists outside what the plug-in published are refused.
    Steinberg::Vst::String128 name{};
    REQUIRE(fx.processor.getProgramName(list.id, 3, name) == Steinberg::kResultFalse);
    REQUIRE(fx.processor.getProgramName(list.id, -1, name) == Steinberg::kResultFalse);
    REQUIRE(fx.processor.getProgramName(list.id + 1, 0, name) == Steinberg::kResultFalse);
    REQUIRE(fx.processor.getProgramInfo(list.id, 0, "Unknown", name) == Steinberg::kResultFalse);
    REQUIRE(fx.processor.getProgramInfo(list.id, 0, nullptr, name) == Steinberg::kResultFalse);

    // The root unit points at the list and the program-change parameter selects
    // within it.
    Steinberg::Vst::UnitInfo root{};
    REQUIRE(fx.processor.getUnitInfo(0, root) == Steinberg::kResultOk);
    REQUIRE(root.programListId == list.id);

    bool found = false;
    const auto param = find_parameter(fx.processor, kProgramParam, found);
    REQUIRE(found);
    REQUIRE((param.flags & Steinberg::Vst::ParameterInfo::kIsProgramChange) != 0);
    REQUIRE(param.stepCount == 2);
    REQUIRE(param.unitId == Steinberg::Vst::kRootUnitId);

    // The host's generic UI shows the program name for each step.
    Steinberg::Vst::String128 text{};
    REQUIRE(fx.processor.getParamStringByValue(kProgramParam, 0.5, text) == Steinberg::kResultTrue);
    REQUIRE(to_string(text) == "Init");
}

TEST_CASE("VST3 selecting a program applies the preset", "[vst3][program]") {
    PresetFolder folder;
    folder.add("Bright", 0.9f, 0.1f);
    folder.add("Init", 0.5f, 0.5f);
    folder.add("Warm", 0.25f, 0.75f);
    Fixture fx(folder.path());

    // Warm is program 2 of 3.
    REQUIRE(fx.processor.setParamNormalized(kProgramParam, 1.0) == Steinberg::kResultOk);
    REQUIRE(fx.processor.getParamNormalized(kLevel) == 0.25);
    REQUIRE(fx.processor.getParamNormalized(kMix) == 0.75);

    // The host is told parameter values changed, from a safe point.
    fx.processor.poll_pending_restart_for_test();
    REQUIRE((fx.handler.restart_flags & Steinberg::Vst::kParamValuesChanged) != 0);

    // The selection reaches the state the host saves.
    Steinberg::MemoryStream saved;
    REQUIRE(fx.processor.getState(&saved) == Steinberg::kResultOk);
    REQUIRE(saved.getSize() > 0);

    SECTION("a host that replays the current selection does not undo later edits") {
        REQUIRE(fx.processor.setParamNormalized(kLevel, 0.8) == Steinberg::kResultOk);
        REQUIRE(fx.processor.setParamNormalized(kProgramParam, 1.0) == Steinberg::kResultOk);
        REQUIRE(fx.processor.getParamNormalized(kLevel) == 0.8);
    }

    SECTION("selecting a different program applies that program") {
        REQUIRE(fx.processor.setParamNormalized(kProgramParam, 0.0) == Steinberg::kResultOk);
        REQUIRE(fx.processor.getParamNormalized(kLevel) == 0.9f);
        REQUIRE(fx.processor.getParamNormalized(kMix) == 0.1f);
    }

    SECTION("an out-of-range value selects the nearest program") {
        REQUIRE(fx.processor.setParamNormalized(kProgramParam, 0.0) == Steinberg::kResultOk);
        REQUIRE(fx.processor.setParamNormalized(kProgramParam, 2.0) == Steinberg::kResultOk);
        // setParamNormalized clamps to 1.0 in the base class, which is Warm.
        REQUIRE(fx.processor.getParamNormalized(kLevel) == 0.25);
    }
}

TEST_CASE("VST3 program change from the audio thread applies the preset", "[vst3][program]") {
    PresetFolder folder;
    folder.add("Bright", 0.9f, 0.1f);
    folder.add("Warm", 0.25f, 0.75f);
    Fixture fx(folder.path());
    fx.activate();

    g_level_seen.store(-1.0f);
    g_mix_seen.store(-1.0f);
    fx.process_program_change(1.0);

    // The block that carried the change already renders with the new values.
    REQUIRE(g_level_seen.load() == 0.25f);
    REQUIRE(g_mix_seen.load() == 0.75f);

    // The host-visible parameters catch up at the next main-thread delivery,
    // and the host is told to re-read them.
    fx.processor.poll_pending_restart_for_test();
    REQUIRE((fx.handler.restart_flags & Steinberg::Vst::kParamValuesChanged) != 0);
    REQUIRE(fx.processor.getParamNormalized(kLevel) == 0.25);
    REQUIRE(fx.processor.getParamNormalized(kMix) == 0.75);
    REQUIRE(fx.processor.getParamNormalized(kProgramParam) == 1.0);
    fx.processor.setActive(false);
}

TEST_CASE("VST3 setUnitProgramData selects a program and refuses foreign data", "[vst3][program]") {
    PresetFolder folder;
    folder.add("Bright", 0.9f, 0.1f);
    folder.add("Warm", 0.25f, 0.75f);
    Fixture fx(folder.path());

    const auto list_id = detail::kVst3FactoryProgramListId;
    REQUIRE(fx.processor.setUnitProgramData(list_id, 1, nullptr) == Steinberg::kResultOk);
    REQUIRE(fx.processor.getParamNormalized(kLevel) == 0.25);

    REQUIRE(fx.processor.setUnitProgramData(list_id, 2, nullptr) == Steinberg::kResultFalse);
    REQUIRE(fx.processor.setUnitProgramData(list_id, -1, nullptr) == Steinberg::kResultFalse);
    REQUIRE(fx.processor.setUnitProgramData(list_id + 5, 0, nullptr) == Steinberg::kResultFalse);

    // Factory programs are read-only: a payload cannot replace one.
    Steinberg::MemoryStream payload;
    Steinberg::int32 written = 0;
    const char bytes[] = "not a preset";
    payload.write(const_cast<char*>(bytes), sizeof(bytes), &written);
    payload.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
    REQUIRE(fx.processor.setUnitProgramData(list_id, 0, &payload) == Steinberg::kResultFalse);
    REQUIRE(fx.processor.getParamNormalized(kLevel) == 0.25);
}

TEST_CASE("VST3 program list reports the processor's note names as pitch names",
          "[vst3][program]") {
    PresetFolder folder;
    folder.add("Kit", 0.5f, 0.5f);
    Fixture fx(folder.path());
    const auto list_id = detail::kVst3FactoryProgramListId;

    REQUIRE(fx.processor.hasProgramPitchNames(list_id, 0) == Steinberg::kResultFalse);

    shared_note_names() = {{.name = "Kick", .port = -1, .channel = -1, .key = 36}};
    REQUIRE(fx.processor.hasProgramPitchNames(list_id, 0) == Steinberg::kResultTrue);
    REQUIRE(fx.processor.hasProgramPitchNames(list_id, 1) == Steinberg::kResultFalse);

    Steinberg::Vst::String128 name{};
    REQUIRE(fx.processor.getProgramPitchName(list_id, 0, 36, name) == Steinberg::kResultOk);
    REQUIRE(to_string(name) == "Kick");
    REQUIRE(fx.processor.getProgramPitchName(list_id, 0, 37, name) == Steinberg::kResultFalse);
    shared_note_names().clear();
}
