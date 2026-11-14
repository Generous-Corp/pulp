// What a real plug-in bundle observes from Pulp's host, and what the host
// gets back from it.
//
// Loads the Pulp Host Probe fixture (test/fixtures/host_probe) as a real VST3
// and a real CLAP bundle through PluginSlot::load, the same path a user's
// plug-in takes. On macOS it also loads the probe's AU component through the
// AUAudioUnit slot: registered in this process by default, or, with
// PULP_TEST_AUV3_PROBE_OUT_OF_PROCESS=1 and the component installed in
// ~/Library/Audio/Plug-Ins/Components, out of process. The probe reports what it saw through read-only parameters,
// so each check below is a fact observed inside the plug-in, delivered by the
// format's own adapter: transport, offline mode, prepared sample rate and
// block size, sidechain input and aux output, mapped MIDI, plug-in-originated
// parameter edits, and a latency change rebuilding SignalGraph's latency
// compensation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "fixtures/host_probe/host_probe.hpp"

#include <pulp/events/message_loop_integration.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#ifdef PULP_TEST_HOST_PROBE_AU
#include "support/au_component_registration.hpp"
#include <pulp/host/plugin_slot_auv3.hpp>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::PluginParameterEdit;
using pulp::host::PluginSlot;
using Probe = pulp::test_fixtures::HostProbe;
namespace fmt = pulp::format;

namespace {

constexpr int kFrames = 128;

struct Target {
    PluginFormat format;
    const char* path;
    const char* label;
};

#ifdef PULP_TEST_HOST_PROBE_AU
bool au_probe_out_of_process() {
    const char* v = std::getenv("PULP_TEST_AUV3_PROBE_OUT_OF_PROCESS");
    return v && std::string(v) == "1";
}

/// "TYPE:SUBT:MANU" of the probe component, read from its Info.plist. Unless
/// the run is out of process, the component is also registered with this
/// process (once) through the factory the plist names, so in-process
/// AUAudioUnit instantiation finds it without an install.
std::string register_au_component(const char* bundle_path) {
    const auto desc = pulp::test::au_component_description(bundle_path, !au_probe_out_of_process());
    return desc ? pulp::test::au_component_id(*desc) : std::string{};
}

/// The component's id, registered with this process once per bundle unless
/// the run is out of process.
const std::string& au_component_id(const char* bundle_path) {
    static std::map<std::string, std::string> ids;
    auto it = ids.find(bundle_path);
    if (it == ids.end())
        it = ids.emplace(bundle_path, register_au_component(bundle_path)).first;
    return it->second;
}

const std::string& au_probe_component_id() {
    return au_component_id(PULP_TEST_HOST_PROBE_AU);
}

std::unique_ptr<PluginSlot> load_au(const char* bundle_path) {
    PluginInfo info;
    info.format = PluginFormat::AudioUnitV3;
    info.path = bundle_path;
    info.unique_id = au_component_id(bundle_path);
    REQUIRE_FALSE(info.unique_id.empty());
    pulp::host::AuV3LoadOptions options;
    options.out_of_process = au_probe_out_of_process();
    auto slot = pulp::host::load_auv3_plugin(info, options);
    REQUIRE(slot);
    return slot;
}
#endif

std::vector<Target> targets() {
    std::vector<Target> out;
#ifdef PULP_TEST_HOST_PROBE_VST3
    out.push_back({PluginFormat::VST3, PULP_TEST_HOST_PROBE_VST3, "VST3"});
#endif
#ifdef PULP_TEST_HOST_PROBE_CLAP
    out.push_back({PluginFormat::CLAP, PULP_TEST_HOST_PROBE_CLAP, "CLAP"});
#endif
#ifdef PULP_TEST_HOST_PROBE_AU
    out.push_back(
        {PluginFormat::AudioUnitV3, PULP_TEST_HOST_PROBE_AU,
         au_probe_out_of_process() ? "AUv3 slot, out of process" : "AUv3 slot, in process"});
#endif
    return out;
}

std::unique_ptr<PluginSlot> load(const Target& target) {
    REQUIRE(std::filesystem::exists(target.path));
    PluginInfo info;
    info.format = target.format;
    info.path = target.path;
    info.name = "Pulp Host Probe";
    info.num_inputs = 2;
    info.num_outputs = 2;
    std::unique_ptr<PluginSlot> slot;
#ifdef PULP_TEST_HOST_PROBE_AU
    if (target.format == PluginFormat::AudioUnitV3) {
        info.unique_id = au_probe_component_id();
        REQUIRE_FALSE(info.unique_id.empty());
        pulp::host::AuV3LoadOptions options;
        options.out_of_process = au_probe_out_of_process();
        slot = pulp::host::load_auv3_plugin(info, options);
    }
#endif
    if (!slot && target.format != PluginFormat::AudioUnitV3)
        slot = PluginSlot::load(info);
    REQUIRE(slot);
    REQUIRE(slot->is_loaded());
    return slot;
}

// Host buses for one block: stereo main in/out, stereo sidechain in, stereo
// aux out.
struct Block {
    std::vector<std::vector<float>> store =
        std::vector<std::vector<float>>(8, std::vector<float>(kFrames, 0.0f));
    std::array<const float*, 2> main_in{store[0].data(), store[1].data()};
    std::array<const float*, 2> side_in{store[2].data(), store[3].data()};
    std::array<float*, 2> main_out{store[4].data(), store[5].data()};
    std::array<float*, 2> aux_out{store[6].data(), store[7].data()};
    std::array<fmt::ProcessBusBufferView<const float>, 2> ins{};
    std::array<fmt::ProcessBusBufferView<float>, 2> outs{};
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::host::ParameterEventQueue params;

    Block() {
        ins[0].buffer = pulp::audio::BufferView<const float>(main_in.data(), 2, kFrames);
        ins[1].info.role = fmt::BusRole::Sidechain;
        ins[1].buffer = pulp::audio::BufferView<const float>(side_in.data(), 2, kFrames);
        outs[0].buffer = pulp::audio::BufferView<float>(main_out.data(), 2, kFrames);
        outs[1].info.role = fmt::BusRole::Aux;
        outs[1].buffer = pulp::audio::BufferView<float>(aux_out.data(), 2, kFrames);
    }
    void run(PluginSlot& slot, const fmt::ProcessContext& ctx) {
        fmt::ProcessBuffers buffers{fmt::ProcessBusBufferSet<const float>{std::span(ins)},
                                    fmt::ProcessBusBufferSet<float>{std::span(outs)}};
        slot.process(buffers, midi_in, midi_out, params, kFrames, ctx);
        midi_in.clear();
    }
};

fmt::ProcessContext transport() {
    using F = fmt::TransportField;
    fmt::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = kFrames;
    ctx.is_playing = true;
    ctx.tempo_bpm = 128.0;
    ctx.position_beats = 6.5;
    ctx.position_samples = 146250;
    ctx.time_sig_numerator = 3;
    ctx.time_sig_denominator = 4;
    ctx.is_recording = true;
    ctx.is_looping = true;
    ctx.loop_start_beats = 4.0;
    ctx.loop_end_beats = 12.0;
    for (auto f : {F::Playing, F::Recording, F::Looping, F::Tempo, F::BeatPosition,
                   F::SamplePosition, F::TimeSignature, F::LoopRange})
        ctx.transport_validity.set(f);
    return ctx;
}

bool near(float a, float b, float tol = 0.01f) {
    return std::abs(a - b) <= tol;
}

// Service the main thread the way a GUI host would, until `done` or a
// deadline. Formats deliver some plug-in requests (VST3 restartComponent, a
// CLAP on_main_thread callback) only from there.
template <typename Done> bool pump_until(PluginSlot& slot, Done&& done) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::array<PluginParameterEdit, 64> scratch{};
    while (std::chrono::steady_clock::now() < deadline) {
        slot.drain_parameter_edits(scratch);
        (void)slot.latency_samples();
        if (done())
            return true;
        const auto r = pulp::events::MessageLoopIntegration::pump_main_loop_for(10ms);
        if (r == pulp::events::MainLoopPumpResult::Unsupported ||
            r == pulp::events::MainLoopPumpResult::WrongThread)
            std::this_thread::sleep_for(10ms);
    }
    return done();
}

// Plug-in edits, collected while servicing the main thread until `done`
// accepts them. An AU reports its own parameter changes through AU event
// listeners, which deliver on the main run loop.
template <typename Done>
std::vector<PluginParameterEdit> collect_edits(PluginSlot& slot, Done&& done) {
    using namespace std::chrono_literals;
    std::vector<PluginParameterEdit> all;
    std::array<PluginParameterEdit, 256> scratch{};
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (true) {
        const auto n = slot.drain_parameter_edits(scratch);
        all.insert(all.end(), scratch.begin(), scratch.begin() + (std::ptrdiff_t)n);
        if (done(all) || std::chrono::steady_clock::now() >= deadline)
            return all;
        const auto r = pulp::events::MessageLoopIntegration::pump_main_loop_for(10ms);
        if (r == pulp::events::MainLoopPumpResult::Unsupported ||
            r == pulp::events::MainLoopPumpResult::WrongThread)
            std::this_thread::sleep_for(10ms);
    }
}

// What the probe's AU component cannot report, whoever hosts it: an `aufx`
// effect receives no MIDI, the AU v2 transport callback carries no recording
// flag, and the AU effect base has a single output element, so no aux bus.
bool is_au(const Target& target) {
    return target.format == PluginFormat::AudioUnitV3;
}

} // namespace

TEST_CASE("Host probe bundles are built for every hosted format", "[host][probe]") {
    CHECK_FALSE(targets().empty());
}

TEST_CASE("Host probe sees the prepared sample rate and block size and the host transport",
          "[host][probe][transport]") {
    for (const auto& target : targets()) {
        INFO(target.label);
        auto slot = load(target);
        REQUIRE(slot->prepare(48000.0, 256));
        Block b;
        b.run(*slot, transport());
        b.run(*slot, transport());
        CHECK(near(slot->get_parameter(Probe::kSampleRate), 48000.0f, 1.0f));
        CHECK(near(slot->get_parameter(Probe::kMaxBlock), 256.0f, 0.5f));
        CHECK(near(slot->get_parameter(Probe::kTempo), 128.0f));
        CHECK(near(slot->get_parameter(Probe::kPlaying), 1.0f));
        CHECK(near(slot->get_parameter(Probe::kBeats), 6.5f));
        CHECK(near(slot->get_parameter(Probe::kTimeSigNumerator), 3.0f));
        if (!is_au(target))
            CHECK(near(slot->get_parameter(Probe::kRecording), 1.0f));
        CHECK(near(slot->get_parameter(Probe::kLooping), 1.0f));
        CHECK(near(slot->get_parameter(Probe::kOffline), 0.0f));
        // The plug-in's own adapter marks these fields as supplied.
        const auto bits = (unsigned)std::lround(slot->get_parameter(Probe::kValidityBits));
        using F = fmt::TransportField;
        for (auto f :
             {F::Playing, F::Recording, F::Looping, F::Tempo, F::BeatPosition, F::TimeSignature}) {
            if (is_au(target) && f == F::Recording)
                continue;
            INFO("transport field " << (int)f << " bits " << bits);
            CHECK((bits & (1u << (unsigned)f)) != 0);
        }
    }
}

TEST_CASE("Host probe runs in offline mode when the host renders offline",
          "[host][probe][offline]") {
    for (const auto& target : targets()) {
        INFO(target.label);
        auto slot = load(target);
        auto settings = slot->host_settings();
        settings.process_mode = fmt::ProcessMode::Offline;
        REQUIRE(slot->set_host_settings(settings));
        REQUIRE(slot->prepare(48000.0, kFrames));
        Block b;
        b.run(*slot, transport());
        b.run(*slot, transport());
        CHECK(near(slot->get_parameter(Probe::kOffline), 1.0f));
    }
}

TEST_CASE("Host probe reads the host sidechain and fills the host aux output",
          "[host][probe][buses]") {
    for (const auto& target : targets()) {
        INFO(target.label);
        auto slot = load(target);
        REQUIRE(slot->prepare(48000.0, kFrames));
        Block b;
        std::fill(b.store[2].begin(), b.store[2].end(), 0.25f); // sidechain L
        b.run(*slot, transport());
        b.run(*slot, transport());
        CHECK(near(slot->get_parameter(Probe::kSidechainPeak), 0.25f));
        CHECK(near(slot->get_parameter(Probe::kActiveInputBuses), 2.0f));
        CHECK(near(slot->get_parameter(Probe::kSidechainChannels), 2.0f));
        if (is_au(target)) {
            CHECK(near(slot->get_parameter(Probe::kActiveOutputBuses), 1.0f));
            continue;
        }
        CHECK(near(slot->get_parameter(Probe::kActiveOutputBuses), 2.0f));
        CHECK(b.store[6][0] == Probe::kAuxLevel);
        CHECK(b.store[7][kFrames - 1] == Probe::kAuxLevel);
    }
}

TEST_CASE("Host probe receives a mapped controller and pitch bend", "[host][probe][midi]") {
    for (const auto& target : targets()) {
        INFO(target.label);
        if (is_au(target))
            continue;
        auto slot = load(target);
        REQUIRE(slot->prepare(48000.0, kFrames));
        Block b;
        pulp::midi::MidiEvent volume;
        volume.sample_offset = 4;
        volume.message = choc::midi::ShortMessage(0xB0, 7, 99);
        b.midi_in.add(volume);
        pulp::midi::MidiEvent bend;
        bend.sample_offset = 8;
        bend.message = choc::midi::ShortMessage(0xE0, 0x00, 0x60); // 0x3000
        b.midi_in.add(bend);
        auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
        note.sample_offset = 12;
        b.midi_in.add(note);
        b.run(*slot, transport());
        b.run(*slot, transport());
        CHECK(near(slot->get_parameter(Probe::kLastCc7), 99.0f, 0.6f));
        CHECK(near(slot->get_parameter(Probe::kPitchBend), 12288.0f, 2.0f));
        CHECK(near(slot->get_parameter(Probe::kNoteOns), 1.0f));
    }
}

TEST_CASE("Host probe's own parameter edit reaches the host", "[host][probe][params]") {
    for (const auto& target : targets()) {
        INFO(target.label);
        auto slot = load(target);
        REQUIRE(slot->prepare(48000.0, kFrames));
        Block b;
        b.run(*slot, transport());
        std::array<PluginParameterEdit, 256> edits{};
        slot->drain_parameter_edits(edits); // discard the first block's reports

        slot->set_parameter(Probe::kCommand, 1.0f);
        b.run(*slot, transport());
        b.run(*slot, transport());
        auto is_edit = [](const PluginParameterEdit& e) {
            return e.id == Probe::kGain && e.kind == PluginParameterEdit::Kind::value &&
                   near(e.plain_value, Probe::kEditedGainDb, 0.05f);
        };
        const auto got = collect_edits(
            *slot, [&](const auto& all) { return std::any_of(all.begin(), all.end(), is_edit); });
        CHECK(std::any_of(got.begin(), got.end(), is_edit));
        CHECK(near(slot->get_parameter(Probe::kGain), Probe::kEditedGainDb, 0.05f));
    }
}

TEST_CASE("Host probe editor gesture reaches the host as begin value end",
          "[host][probe][params]") {
    for (const auto& target : targets()) {
        INFO(target.label);
        auto slot = load(target);
        REQUIRE(slot->prepare(48000.0, kFrames));
        Block b;
        b.run(*slot, transport());
        std::array<PluginParameterEdit, 256> edits{};
        slot->drain_parameter_edits(edits); // discard the first block's reports

        // Command 3: the next prepare() performs a gesture on Gain on the main
        // thread, the way an editor drag would.
        slot->set_parameter(Probe::kCommand, 3.0f);
        b.run(*slot, transport());
        REQUIRE(slot->prepare(48000.0, kFrames));
        b.run(*slot, transport());
        b.run(*slot, transport());

        const auto got = collect_edits(*slot, [](const auto& all) {
            return std::any_of(all.begin(), all.end(), [](const PluginParameterEdit& e) {
                return e.id == Probe::kGain && e.kind == PluginParameterEdit::Kind::end_gesture;
            });
        });
        int begin_at = -1, value_at = -1, end_at = -1;
        for (std::size_t i = 0; i < got.size(); ++i) {
            if (got[i].id != Probe::kGain)
                continue;
            using K = PluginParameterEdit::Kind;
            if (got[i].kind == K::begin_gesture && begin_at < 0)
                begin_at = (int)i;
            if (got[i].kind == K::value && value_at < 0 &&
                near(got[i].plain_value, Probe::kGestureGainDb, 0.05f))
                value_at = (int)i;
            if (got[i].kind == K::end_gesture)
                end_at = (int)i;
        }
        CHECK(begin_at >= 0);
        CHECK(value_at > begin_at);
        CHECK(end_at > value_at);
        CHECK(near(slot->get_parameter(Probe::kGain), Probe::kGestureGainDb, 0.05f));
    }
}

TEST_CASE("Host probe latency change rebuilds SignalGraph latency compensation",
          "[host][probe][restart][pdc]") {
    for (const auto& target : targets()) {
        INFO(target.label);
        auto slot = load(target);
        auto* raw = slot.get();
        pulp::host::SignalGraph graph;
        const auto in = graph.add_input_node(2);
        const auto plug = graph.add_plugin_node(std::move(slot), 2, 2, "probe");
        const auto out = graph.add_output_node(2);
        REQUIRE(graph.connect(in, 0, plug, 0));
        REQUIRE(graph.connect(in, 1, plug, 1));
        REQUIRE(graph.connect(plug, 0, out, 0));
        REQUIRE(graph.connect(plug, 1, out, 1));
        REQUIRE(graph.prepare(48000.0, kFrames));
        CHECK(graph.latency_samples() == Probe::kInitialLatency);

        std::vector<float> l(kFrames), r(kFrames), ol(kFrames), or_(kFrames);
        std::array<const float*, 2> ip{l.data(), r.data()};
        std::array<float*, 2> op{ol.data(), or_.data()};
        pulp::audio::BufferView<const float> input(ip.data(), 2, kFrames);
        pulp::audio::BufferView<float> output(op.data(), 2, kFrames);

        raw->set_parameter(Probe::kCommand, 2.0f);
        graph.process(output, input, kFrames);
        graph.process(output, input, kFrames);
        REQUIRE(pump_until(*raw, [&] { return graph.plugin_restart_pending(); }));
        REQUIRE(graph.apply_plugin_restarts());
        CHECK(graph.latency_samples() == Probe::kChangedLatency);
        CHECK_FALSE(graph.plugin_restart_pending());
    }
}

#ifdef PULP_TEST_HOST_MIDI_PROBE_AU
TEST_CASE("AUv3 slot returns a MIDI effect's output at its sample offset",
          "[host][probe][auv3][midi]") {
    INFO((au_probe_out_of_process() ? "out of process" : "in process"));
    auto slot = load_au(PULP_TEST_HOST_MIDI_PROBE_AU);
    REQUIRE(slot->prepare(48000.0, kFrames));
    Block b;
    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 10;
    b.midi_in.add(note);
    pulp::midi::MidiEvent off;
    off.sample_offset = 70;
    off.message = choc::midi::ShortMessage(0x80, 60, 0);
    b.midi_in.add(off);
    b.run(*slot, transport());
    // The plug-in answers each note an octave up, at the frame it arrived.
    std::vector<std::pair<int, std::array<std::uint8_t, 3>>> got;
    for (const auto& e : b.midi_out)
        got.push_back(
            {e.sample_offset, {e.message.data()[0], e.message.data()[1], e.message.data()[2]}});
    REQUIRE(got.size() == 2);
    CHECK(got[0].first == 10);
    CHECK(got[0].second == std::array<std::uint8_t, 3>{0x90, 72, 100});
    CHECK(got[1].first == 70);
    CHECK(got[1].second == std::array<std::uint8_t, 3>{0x80, 72, 0});
}
#endif

#ifdef PULP_TEST_HOST_PROBE_AU
TEST_CASE("AUv3 slot reports a plug-in that dies out of process", "[host][probe][auv3][oop]") {
    if (!au_probe_out_of_process())
        SKIP("needs PULP_TEST_AUV3_PROBE_OUT_OF_PROCESS=1 and the installed probe component");
    auto slot = load_au(PULP_TEST_HOST_PROBE_AU);
    REQUIRE(slot->prepare(48000.0, kFrames));
    Block b;
    b.run(*slot, transport());
    REQUIRE(slot->is_loaded());
    const auto restarts = slot->restart_request_count();
    // Command 4 aborts the extension's process from inside process().
    slot->set_parameter(Probe::kCommand, 4.0f);
    REQUIRE(pump_until(*slot, [&] {
        b.run(*slot, transport());
        return !slot->is_loaded();
    }));
    CHECK(slot->restart_request_count() > restarts);
    // The host keeps running: later blocks pass audio through and are
    // reported failed, without touching the dead unit.
    std::fill(b.store[0].begin(), b.store[0].end(), 0.5f);
    slot->begin_process();
    b.run(*slot, transport());
    CHECK(slot->process_status() == PluginSlot::ProcessStatus::failed);
    CHECK(b.store[4][0] == 0.5f);
}
#endif
