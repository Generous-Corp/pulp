// VST3 publishes Processor::note_names() through IKeyswitchController, the
// surface a VST3 host uses to label individual keys instead of showing bare
// pitches — a drum instrument's "Kick", a sampler's articulation switches.
//
// Each name becomes a single-key switch (min == max == the named key). Entries
// carry -1 wildcards for port and channel, so one name can cover every bus and
// channel; these pin that matching.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/vst3_adapter.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format;

namespace {

/// Minimal IHostApplication. `PulpVst3Processor::initialize()` requires a host
/// context; nothing here calls back into it.
class HostApp final : public Steinberg::Vst::IHostApplication {
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char* kName = "PulpTest";
        for (int i = 0; i < 8; ++i) name[i] = static_cast<Steinberg::Vst::TChar>(kName[i]);
        name[8] = 0;
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

std::vector<NoteName>& shared_note_names() {
    static std::vector<NoteName> names;
    return names;
}

class NamingProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        return {
            .name = "NoteNames",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.notenames",
            .version = "1.0.0",
            .category = PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
        };
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = 1, .name = "Level", .range = {0.0f, 1.0f, 1.0f}});
    }
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const ProcessContext&) override {}

    std::vector<NoteName> note_names() const override { return shared_note_names(); }
};

std::unique_ptr<Processor> make_naming_processor() {
    return std::make_unique<NamingProcessor>();
}

std::string title_of(const Steinberg::Vst::KeyswitchInfo& info) {
    std::string out;
    for (int i = 0; i < 128 && info.title[i] != 0; ++i)
        out.push_back(static_cast<char>(info.title[i]));
    return out;
}

}  // namespace

TEST_CASE("VST3 reports no key switches when the plugin names no notes",
          "[vst3][note-name]") {
    shared_note_names().clear();

    HostApp host;
    vst3::PulpVst3Processor processor(&make_naming_processor);
    REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);

    // The host falls back to bare pitches, which is what every plugin did
    // before note names existed.
    REQUIRE(processor.getKeyswitchCount(0, 0) == 0);

    Steinberg::Vst::KeyswitchInfo info{};
    REQUIRE(processor.getKeyswitchInfo(0, 0, 0, info) == Steinberg::kResultFalse);

    processor.terminate();
}

TEST_CASE("VST3 publishes each note name as a single-key switch",
          "[vst3][note-name]") {
    shared_note_names() = {
        {.name = "Kick", .port = -1, .channel = -1, .key = 36},
        {.name = "Snare", .port = -1, .channel = -1, .key = 38},
    };

    HostApp host;
    vst3::PulpVst3Processor processor(&make_naming_processor);
    REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);

    REQUIRE(processor.getKeyswitchCount(0, 0) == 2);

    Steinberg::Vst::KeyswitchInfo info{};
    REQUIRE(processor.getKeyswitchInfo(0, 0, 0, info) == Steinberg::kResultTrue);
    REQUIRE(title_of(info) == "Kick");
    // One name names one key, so the switch spans a single note.
    REQUIRE(info.keyswitchMin == 36);
    REQUIRE(info.keyswitchMax == 36);
    REQUIRE(info.keyRemapped == -1);
    REQUIRE(info.unitId == -1);

    REQUIRE(processor.getKeyswitchInfo(0, 0, 1, info) == Steinberg::kResultTrue);
    REQUIRE(title_of(info) == "Snare");
    REQUIRE(info.keyswitchMin == 38);

    // Out-of-range and negative indices decline rather than read past the list.
    REQUIRE(processor.getKeyswitchInfo(0, 0, 2, info) == Steinberg::kResultFalse);
    REQUIRE(processor.getKeyswitchInfo(0, 0, -1, info) == Steinberg::kResultFalse);

    processor.terminate();
    shared_note_names().clear();
}

TEST_CASE("VST3 key switches honour port and channel wildcards",
          "[vst3][note-name]") {
    shared_note_names() = {
        {.name = "Everywhere", .port = -1, .channel = -1, .key = 60},
        {.name = "Channel 9 only", .port = -1, .channel = 9, .key = 61},
        {.name = "Bus 1 only", .port = 1, .channel = -1, .key = 62},
    };

    HostApp host;
    vst3::PulpVst3Processor processor(&make_naming_processor);
    REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);

    // Bus 0, channel 0: only the fully wildcarded entry applies.
    REQUIRE(processor.getKeyswitchCount(0, 0) == 1);
    Steinberg::Vst::KeyswitchInfo info{};
    REQUIRE(processor.getKeyswitchInfo(0, 0, 0, info) == Steinberg::kResultTrue);
    REQUIRE(title_of(info) == "Everywhere");

    // Bus 0, channel 9 adds the channel-scoped entry.
    REQUIRE(processor.getKeyswitchCount(0, 9) == 2);

    // Bus 1, channel 0 adds the bus-scoped entry instead.
    REQUIRE(processor.getKeyswitchCount(1, 0) == 2);
    REQUIRE(processor.getKeyswitchInfo(1, 0, 1, info) == Steinberg::kResultTrue);
    REQUIRE(title_of(info) == "Bus 1 only");

    // Bus 1, channel 9 sees all three.
    REQUIRE(processor.getKeyswitchCount(1, 9) == 3);

    processor.terminate();
    shared_note_names().clear();
}

TEST_CASE("VST3 skips note names that identify no single key",
          "[vst3][note-name]") {
    shared_note_names() = {
        {.name = "Named key", .port = -1, .channel = -1, .key = 40},
        // A key wildcard names no single key, so it cannot become a key switch
        // (a VST3 key switch must carry a concrete min/max note).
        {.name = "Every key", .port = -1, .channel = -1, .key = -1},
        {.name = "Out of range", .port = -1, .channel = -1, .key = 200},
    };

    HostApp host;
    vst3::PulpVst3Processor processor(&make_naming_processor);
    REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);

    REQUIRE(processor.getKeyswitchCount(0, 0) == 1);
    Steinberg::Vst::KeyswitchInfo info{};
    REQUIRE(processor.getKeyswitchInfo(0, 0, 0, info) == Steinberg::kResultTrue);
    REQUIRE(title_of(info) == "Named key");

    processor.terminate();
    shared_note_names().clear();
}

TEST_CASE("VST3 hands out IKeyswitchController through queryInterface",
          "[vst3][note-name]") {
    HostApp host;
    vst3::PulpVst3Processor processor(&make_naming_processor);
    REQUIRE(processor.initialize(&host) == Steinberg::kResultOk);

    // A host reaches the surface by querying for the interface; declaring the
    // methods without exposing them here would leave note names unreachable.
    void* obj = nullptr;
    REQUIRE(processor.queryInterface(Steinberg::Vst::IKeyswitchController::iid, &obj) ==
            Steinberg::kResultTrue);
    REQUIRE(obj != nullptr);

    processor.terminate();
}
