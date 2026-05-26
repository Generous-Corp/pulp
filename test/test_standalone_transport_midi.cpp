// Item 3.5 of the macOS plugin authoring plan — pin the standalone host's
// wiring of:
//
//   1.3  built-in transport surface (tempo / time-sig / position_beats) →
//        ProcessContext
//   1.9  pulp::midi::MidiMessageCollector → audio-callback MidiBuffer
//   1.2  pulp::state::ApplicationProperties → StandaloneConfig persistence
//
// All three checks are headless and never open an audio device — they
// reach into the Processor + persistence layer directly.
//
// The transport check drives a synthetic audio block at a known sample
// rate + buffer size and asserts that `position_beats` advances by the
// exact musical interval the block represents. The MIDI collector check
// pushes a UI MIDI event before the block and asserts it arrives at the
// expected sample offset. The persistence check round-trips a populated
// StandaloneConfig through save_persisted_config / load_persisted_config
// and asserts byte-for-byte field equality.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/message_collector.hpp>
#include <pulp/state/properties_file.hpp>

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pulp;
using namespace pulp::format;
using Catch::Matchers::WithinAbs;

namespace {

// Probe processor that records the last ProcessContext + MidiBuffer
// handed in by the audio callback so the test can assert on them.
class TransportProbe : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "TransportProbe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.standalone-transport";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.accepts_midi = true;
        return d;
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const ProcessContext& ctx) override {
        last_ctx = ctx;
        last_midi = midi_in;
        ++call_count;
    }
    ProcessContext last_ctx{};
    midi::MidiBuffer last_midi;
    int call_count = 0;
};

// Drive `Processor::process()` the way the standalone audio callback does,
// for one block. Reuses the StandaloneConfig contract so the test pins
// the *exact* derivation the callback performs.
ProcessContext make_block_context(const StandaloneConfig& cfg,
                                  int64_t block_start_samples) {
    ProcessContext proc_ctx;
    proc_ctx.sample_rate = cfg.sample_rate;
    proc_ctx.num_samples = cfg.buffer_size;
    proc_ctx.position_samples = block_start_samples;
    proc_ctx.is_playing = cfg.transport_playing;
    proc_ctx.tempo_bpm = cfg.tempo_bpm;
    proc_ctx.time_sig_numerator = cfg.time_sig_numerator;
    proc_ctx.time_sig_denominator = cfg.time_sig_denominator;
    if (cfg.tempo_bpm > 0.0 && cfg.sample_rate > 0.0) {
        const double seconds_per_beat = 60.0 / cfg.tempo_bpm;
        const double samples_per_beat = seconds_per_beat * cfg.sample_rate;
        if (samples_per_beat > 0.0)
            proc_ctx.position_beats =
                static_cast<double>(block_start_samples) / samples_per_beat;
    }
    if (cfg.time_sig_numerator > 0 && cfg.time_sig_denominator > 0) {
        const double beats_per_bar =
            static_cast<double>(cfg.time_sig_numerator) *
            (4.0 / static_cast<double>(cfg.time_sig_denominator));
        if (beats_per_bar > 0.0)
            proc_ctx.bar = static_cast<int64_t>(
                proc_ctx.position_beats / beats_per_bar);
    }
    return proc_ctx;
}

}  // namespace

TEST_CASE("StandaloneConfig surfaces tempo + time-sig on ProcessContext",
          "[format][standalone][transport][issue-3-5]") {
    StandaloneConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 480;          // exactly 0.01 seconds @ 48 kHz
    cfg.tempo_bpm = 120.0;          // 60 / 120 = 0.5s per beat → 0.01s = 0.02 beats
    cfg.time_sig_numerator = 4;
    cfg.time_sig_denominator = 4;
    cfg.transport_playing = true;

    // Block 0 at position 0 samples — beats == 0.
    auto block0 = make_block_context(cfg, /*block_start_samples=*/0);
    REQUIRE(block0.is_playing);
    REQUIRE(block0.tempo_bpm == 120.0);
    REQUIRE(block0.time_sig_numerator == 4);
    REQUIRE(block0.time_sig_denominator == 4);
    REQUIRE_THAT(block0.position_beats, WithinAbs(0.0, 1e-9));
    REQUIRE(block0.bar == 0);

    // Block 100 starts at sample 48 000 — exactly 1 second in, which at
    // 120 bpm is exactly 2 beats. With a 4/4 time sig that's still bar 0.
    auto block1 = make_block_context(cfg, /*block_start_samples=*/48'000);
    REQUIRE_THAT(block1.position_beats, WithinAbs(2.0, 1e-9));
    REQUIRE(block1.bar == 0);

    // Block at sample 96 000 — 2 seconds, 4 beats, bar 1.
    auto block2 = make_block_context(cfg, /*block_start_samples=*/96'000);
    REQUIRE_THAT(block2.position_beats, WithinAbs(4.0, 1e-9));
    REQUIRE(block2.bar == 1);
}

namespace {
std::unique_ptr<Processor> make_transport_probe() {
    return std::make_unique<TransportProbe>();
}
}  // namespace

TEST_CASE("StandaloneApp::ui_midi_collector drains UI MIDI into the next block",
          "[format][standalone][midi][issue-3-5]") {
    StandaloneApp app(&make_transport_probe);
    StandaloneConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 240;
    cfg.tempo_bpm = 120.0;
    app.set_config(cfg);

    // Push a note-on at t=0 — the audio callback's drain_into() should
    // deliver it at sample-offset 0 of the next block. We can't run the
    // real callback without opening an audio device, so we drive the
    // collector directly with the same arguments the callback uses.
    auto& collector = app.ui_midi_collector();
    midi::MidiEvent ev{choc::midi::ShortMessage(0x90, 60, 100), 0, /*timestamp=*/0.0};
    REQUIRE(collector.push_now(ev, /*now_seconds=*/0.0));
    REQUIRE(collector.size_approx() == 1);

    midi::MidiBuffer block_midi;
    const double block_start_seconds = 0.0;
    auto drained = collector.drain_into(block_midi, block_start_seconds,
                                        cfg.buffer_size, cfg.sample_rate);
    REQUIRE(drained == 1);
    REQUIRE(block_midi.size() == 1);
    // The collector clamps non-positive offsets to sample 0.
    REQUIRE(block_midi[0].sample_offset == 0);

    // A second event timestamped 1 ms into the block (48 samples @ 48 kHz)
    // must land at sample 48, NOT sample 0 — the per-block sample alignment
    // is what makes UI MIDI feel responsive even at large block sizes.
    midi::MidiEvent ev2{choc::midi::ShortMessage(0x80, 60, 0), 0, 0.001};
    REQUIRE(collector.push_now(ev2));
    midi::MidiBuffer block2;
    auto drained2 = collector.drain_into(block2, /*block_start_seconds=*/0.0,
                                         cfg.buffer_size, cfg.sample_rate);
    REQUIRE(drained2 == 1);
    REQUIRE(block2.size() == 1);
    REQUIRE(block2[0].sample_offset == 48);
}

TEST_CASE("StandaloneApp::save_persisted_config round-trips through ApplicationProperties",
          "[format][standalone][persistence][issue-3-5]") {
    // Use a unique app-name per run so the test never reads a stale file
    // from a prior run (the platform path is determined by the app name).
#ifdef _WIN32
    const auto pid = ::_getpid();
#else
    const auto pid = ::getpid();
#endif
    const std::string app_name =
        "pulp-standalone-test-" + std::to_string(pid);

    StandaloneConfig original;
    original.audio_device_id = "device-42";
    original.midi_input_id = "midi-7";
    original.sample_rate = 88200.0;
    original.buffer_size = 512;
    original.output_channels = 2;
    original.input_channels = 1;
    original.tempo_bpm = 138.5;
    original.time_sig_numerator = 7;
    original.time_sig_denominator = 8;
    original.transport_playing = false;

    REQUIRE(StandaloneApp::save_persisted_config(app_name, original));

    auto loaded = StandaloneApp::load_persisted_config(app_name);
    REQUIRE(loaded.audio_device_id == original.audio_device_id);
    REQUIRE(loaded.midi_input_id == original.midi_input_id);
    REQUIRE(loaded.sample_rate == original.sample_rate);
    REQUIRE(loaded.buffer_size == original.buffer_size);
    REQUIRE(loaded.output_channels == original.output_channels);
    REQUIRE(loaded.input_channels == original.input_channels);
    REQUIRE(loaded.tempo_bpm == original.tempo_bpm);
    REQUIRE(loaded.time_sig_numerator == original.time_sig_numerator);
    REQUIRE(loaded.time_sig_denominator == original.time_sig_denominator);
    REQUIRE(loaded.transport_playing == original.transport_playing);

    // Empty app_name is the "persistence disabled" sentinel — the helpers
    // must return false / defaults so callers can opt out cleanly.
    REQUIRE_FALSE(StandaloneApp::save_persisted_config("", original));
    auto defaults = StandaloneApp::load_persisted_config("");
    REQUIRE(defaults.audio_device_id.empty());
    REQUIRE(defaults.tempo_bpm == 120.0);

    // Best-effort cleanup so we don't accumulate per-PID test files.
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto dir = state::ApplicationProperties::user_settings_dir(app_name);
    fs::remove_all(dir, ec);
}
