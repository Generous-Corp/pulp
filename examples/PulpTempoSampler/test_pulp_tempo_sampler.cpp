// PulpTempoSampler — headless integration tests for loop analysis, slicing,
// tempo matching, and editor/audio interaction.
//
// Exercises the full instrument pipeline without a host: load loop -> detect
// BPM + slices -> background OfflineStretch render to host tempo (generation-
// published) -> MIDI note plays the cached stretched buffer -> grid-lock
// (published length == round(raw * loop_bpm / host_bpm)). Also a render-while-
// playing race (run under ASan/TSAN to catch use-after-free / data races).

#include <catch2/catch_test_macros.hpp>
#include "pulp_tempo_sampler.hpp"

#include <pulp/format/settings_panel.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/ui_components.hpp>
#if defined(__APPLE__)
#include <pulp/view/musical_typing_keyboard.hpp>  // the SDK keyboard primitive
#include <pulp/view/screenshot.hpp>  // render_to_png: drives one layout+paint pass
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef PULP_TEMPO_SAMPLER_SOURCE_DIR
#  error "PULP_TEMPO_SAMPLER_SOURCE_DIR must be defined by the PulpTempoSampler test target"
#endif

using namespace pulp;
using namespace pulp::examples;

namespace {

// The default root note (slice 0 plays here). Tests trigger root-relative notes so
// they don't break when the default octave changes.
constexpr int kRoot = PulpTempoSamplerProcessor::kDefaultRootNote;

std::vector<float> sine(double f, double sr, long n) {
    std::vector<float> v(static_cast<size_t>(n));
    const double w = 2.0 * 3.14159265358979323846 * f / sr;
    for (long i = 0; i < n; ++i) v[static_cast<size_t>(i)] = 0.4f * static_cast<float>(std::sin(w * i));
    return v;
}

// A loop of `beats` decaying percussive bursts — the onset detector yields a
// slice per burst. Used to exercise slicing + sensitivity.
std::vector<float> percussive_loop(long n, int beats) {
    std::vector<float> v(static_cast<size_t>(n), 0.0f);
    const long beat = n / beats;
    for (long i = 0; i < n; ++i) {
        const double t = static_cast<double>(i % beat) / static_cast<double>(beat);
        const double env = std::exp(-9.0 * t);
        const double freq = 90.0 + 50.0 * static_cast<double>((i / beat) % 3);
        v[static_cast<size_t>(i)] =
            static_cast<float>(0.85 * env * std::sin(2.0 * 3.14159265358979323846 * freq * i / 48000.0));
    }
    return v;
}

// Write a minimal 16-bit mono PCM WAV so load_loop_from_path can decode it.
void put32(std::ofstream& o, std::uint32_t v) { o.put(char(v)); o.put(char(v>>8)); o.put(char(v>>16)); o.put(char(v>>24)); }
void put16(std::ofstream& o, std::uint16_t v) { o.put(char(v)); o.put(char(v>>8)); }
bool write_wav(const std::string& path, const std::vector<float>& mono, int sr) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(mono.size() * 2);
    o.write("RIFF", 4); put32(o, 36 + data_bytes); o.write("WAVE", 4);
    o.write("fmt ", 4); put32(o, 16); put16(o, 1); put16(o, 1);
    put32(o, static_cast<std::uint32_t>(sr)); put32(o, static_cast<std::uint32_t>(sr * 2));
    put16(o, 2); put16(o, 16);
    o.write("data", 4); put32(o, data_bytes);
    for (float s : mono) {
        const int v = static_cast<int>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        put16(o, static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
    }
    return static_cast<bool>(o);
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

template <typename Pred>
bool wait_for(Pred p, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!p() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return p();
}

struct Fixture {
    state::StateStore store;
    std::unique_ptr<PulpTempoSamplerProcessor> proc;
    Fixture() {
        proc = std::make_unique<PulpTempoSamplerProcessor>();
        proc->set_state_store(&store);
        proc->define_parameters(store);
        format::PrepareContext ctx;
        ctx.sample_rate = 48000;
        ctx.max_buffer_size = 512;
        ctx.input_channels = 0;
        ctx.output_channels = 2;
        proc->prepare(ctx);
        // Most tests exercise SLICING, so the fixture engages a slicing sensitivity.
        // The PRODUCT default is whole-loop (SENS 0, 1 slice) — verified explicitly
        // in "default loads the whole loop as a single region".
        store.set_value(kOnsetSens, 1.0f);
    }
};

void process_block(PulpTempoSamplerProcessor& p, double tempo_bpm, bool note_on, int note,
                   std::vector<float>& l, std::vector<float>& r) {
    const int n = static_cast<int>(l.size());
    float* op[2] = {l.data(), r.data()};
    audio::BufferView<float> out(op, 2, static_cast<std::size_t>(n));
    const float* ip[1] = {nullptr};
    audio::BufferView<const float> in(ip, 0, static_cast<std::size_t>(n));
    midi::MidiBuffer min, mout;
    if (note_on) min.add(midi::MidiEvent::note_on(0, note, 100));
    format::ProcessContext ctx{48000, n};
    ctx.tempo_bpm = tempo_bpm;
    ctx.is_playing = true;
    p.process(out, in, min, mout, ctx);
}

// Process one block driven by an explicit MIDI buffer (notes + CC + pitch wheel).
void process_midi(PulpTempoSamplerProcessor& p, double tempo_bpm, midi::MidiBuffer& min,
                  std::vector<float>& l, std::vector<float>& r) {
    const int n = static_cast<int>(l.size());
    float* op[2] = {l.data(), r.data()};
    audio::BufferView<float> out(op, 2, static_cast<std::size_t>(n));
    const float* ip[1] = {nullptr};
    audio::BufferView<const float> in(ip, 0, static_cast<std::size_t>(n));
    midi::MidiBuffer mout;
    format::ProcessContext ctx{48000, n};
    ctx.tempo_bpm = tempo_bpm;
    ctx.is_playing = true;
    p.process(out, in, min, mout, ctx);
}

double block_energy(const std::vector<float>& l) {
    double e = 0.0;
    for (float v : l) e += static_cast<double>(v) * v;
    return e;
}

} // namespace

TEST_CASE("PulpTempoSampler descriptor + params", "[tempo-sampler]") {
    PulpTempoSamplerProcessor p;
    const auto d = p.descriptor();
    REQUIRE(d.name == "PulpTempoSampler");
    REQUIRE(d.version == kPulpTempoSamplerVersion);
    REQUIRE(d.category == format::PluginCategory::Instrument);
    REQUIRE(d.accepts_midi);
    REQUIRE(d.output_buses.size() == 1);
    state::StateStore s; p.define_parameters(s);
    REQUIRE(s.param_count() == 17);
    REQUIRE(s.get_value(kPlaybackMode) == 0.0f);  // default mode = Classic
    REQUIRE(s.get_value(kDirection) == 0.0f);  // default direction = Forward
    REQUIRE(s.get_value(kLoopMode) == 0.0f);   // default loop shape = Forward
    REQUIRE(s.get_value(kTempoLoop) == 0.0f);  // default LOOP = None (play once)
    REQUIRE(s.get_value(kGate) == 1.0f);       // default Gate on (key-up releases)
    REQUIRE(s.get_value(kFlexSpeed) == 2.0f);  // default 1x (index 2 of {1/4,1/2,1,2,4})
}

TEST_CASE("PulpTempoSampler package metadata uses the descriptor version", "[tempo-sampler]") {
    const auto source_dir = std::filesystem::path(PULP_TEMPO_SAMPLER_SOURCE_DIR);

    const auto cmake = read_text_file(source_dir / "CMakeLists.txt");
    const auto cmake_version_key = cmake.find("VERSION");
    REQUIRE(cmake_version_key != std::string::npos);
    REQUIRE(cmake.find(std::string("\"") + kPulpTempoSamplerVersion + "\"",
                       cmake_version_key) != std::string::npos);

    // These sources compile against the header constant; the text check guards
    // against reintroducing per-entry version literals.
    const auto standalone = read_text_file(source_dir / "main.cpp");
    REQUIRE(standalone.find("kPulpTempoSamplerVersion") != std::string::npos);

    const auto vst3 = read_text_file(source_dir / "vst3_entry.cpp");
    REQUIRE(vst3.find("kPulpTempoSamplerVersion") != std::string::npos);
}

TEST_CASE("loads loop, detects bpm/slices, publishes a tempo-matched buffer", "[tempo-sampler]") {
    Fixture f;
    auto buf = sine(440.0, 48000.0, 48000); // 1 s
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));

    // Background worker renders + publishes.
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(f.proc->detected_bpm() >= 0.0); // analyzer ran (may be 0 on a pure tone)

    // Grid-lock: pin loop BPM, ask for a host tempo, and confirm the published
    // length is exactly round(raw * loop/host).
    f.proc->set_loop_bpm_for_test(120.0);
    std::vector<float> l(512), r(512);
    process_block(*f.proc, 90.0, false, 0, l, r); // host 90 -> R = 120/90
    const long expected = static_cast<long>(std::llround(48000.0 * 120.0 / 90.0)); // 64000
    REQUIRE(wait_for([&] { return f.proc->published_frames() == expected; }));
}

// Regression: a sample whose (stretched) length exceeds the per-slot store
// capacity used to be REJECTED by load_*, so the publish was skipped and every
// slice mapped past an empty buffer → tapping any slice was silent (the
// "long samples don't play" bug). The store now holds a longer sample, and the
// publish is clamped to the cap so anything still longer plays its head instead
// of nothing.
TEST_CASE("a sample longer than the old 60 s cap still publishes (not silent)",
          "[tempo-sampler]") {
    Fixture f;
    constexpr long kOldCap = 48000L * 60L;        // the previous per-slot cap
    const long n = 48000L * 61L;                  // 61 s — over the old cap, under the new
    REQUIRE(n > kOldCap);
    REQUIRE(n <= static_cast<long>(SamplerSampleStore::kMaxFrames));
    auto buf = sine(220.0, 48000.0, n);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, n, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    std::vector<float> l(512), r(512);
    process_block(*f.proc, 120.0, false, 0, l, r); // host == loop ⇒ R = 1
    // Publishes the full 61 s (was 0 before the fix), and never exceeds the cap.
    REQUIRE(wait_for([&] { return f.proc->published_frames() == n; },
                     std::chrono::seconds(20)));
    REQUIRE(f.proc->published_frames() <= static_cast<long>(SamplerSampleStore::kMaxFrames));
}

// Regression (#112): a sub-host-rate sample (e.g. 44.1k loaded into a 48k host)
// used to be tagged + played at the host rate WITHOUT resampling, so it played
// back 48000/44100 = 1.088x faster — audibly ~+0.9 semitone higher. load_loop
// now resamples to the host rate on load, so the stored sample sits at the host
// rate with its pitch (cycle count) preserved.
TEST_CASE("a sub-host-rate sample is resampled to the host rate on load (pitch-locked)",
          "[tempo-sampler][issue-112]") {
    Fixture f; // host = 48000
    // 200 Hz tone, exactly 1 s at 44.1k: 200 cycles -> ~400 zero crossings,
    // independent of the rate it ends up stored at.
    auto buf = sine(200.0, 44100.0, 44100);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 44100, 44100.0));

    std::vector<float> mono;
    float sr = 0.0f;
    std::vector<long> slices;
    REQUIRE(f.proc->snapshot_for_view(mono, sr, slices));

    // Stored at the host rate, length-locked to round(N * host/native) = 48000.
    REQUIRE(sr == 48000.0f);
    REQUIRE(std::llabs(static_cast<long>(mono.size()) - 48000L) <= 1);

    // Pitch preserved: still ~200 cycles (~400 sign changes) — NOT scaled up to
    // 200 * 48000/44100 = ~218 Hz (which would read ~436 crossings). Count sign
    // changes, skipping a few edge samples for the trimmed filter ramp.
    int crossings = 0;
    for (std::size_t i = 6; i + 6 < mono.size(); ++i)
        if ((mono[i - 1] <= 0.0f) != (mono[i] <= 0.0f)) ++crossings;
    REQUIRE(crossings >= 392);
    REQUIRE(crossings <= 408);
}

TEST_CASE("MIDI note plays the cached stretched buffer", "[tempo-sampler]") {
    Fixture f;
    auto buf = sine(330.0, 48000.0, 24000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 24000, 48000.0));
    f.proc->set_loop_bpm_for_test(100.0);
    std::vector<float> l(512), r(512);
    process_block(*f.proc, 100.0, false, 0, l, r); // R = 1
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    double energy = 0.0;
    for (int b = 0; b < 8; ++b) {
        process_block(*f.proc, 100.0, b == 0, kRoot, l, r);
        for (int i = 0; i < 512; ++i) energy += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
        for (float v : l) REQUIRE(std::isfinite(v));
    }
    CHECK(energy > 1e-6); // produced audio
}

// #111: the keyboard's sustain / pitch-bend / modulation pads (and the matching
// host CC64 / pitch-wheel / CC1) used to light up but never reach the voices.
// They now drive the audio: sustain defers note-off, pitch-bend + mod-wheel
// vibrato scale the playback rate.

TEST_CASE("sustain pedal (CC64) holds a note through note-off",
          "[tempo-sampler][issue-111]") {
    Fixture f;
    auto buf = sine(220.0, 48000.0, 48000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.store.set_value(kTempoLoop, 1.0f);      // loop so the voice rings until released
    f.store.set_value(kTempoRelease, 5.0f);   // fast release once it DOES release
    f.store.set_value(kTempoSustain, 100.0f); // full sustain level
    f.proc->set_loop_bpm_for_test(120.0);
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }   // kick the render
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    const int note = kRoot;
    // Pedal DOWN, play the note, then release the KEY (note-off).
    { midi::MidiBuffer m; m.add(midi::MidiEvent::cc(0, 64, 127));
      m.add(midi::MidiEvent::note_on(0, note, 100)); process_midi(*f.proc, 120.0, m, l, r); }
    for (int b = 0; b < 4; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
    { midi::MidiBuffer m; m.add(midi::MidiEvent::note_off(0, note)); process_midi(*f.proc, 120.0, m, l, r); }

    double held = 0.0;
    for (int b = 0; b < 8; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); held += block_energy(l); }
    CHECK(held > 1e-3);  // still ringing despite the note-off — the pedal held it

    // Pedal UP -> the held voice finally releases and decays away.
    { midi::MidiBuffer m; m.add(midi::MidiEvent::cc(0, 64, 0)); process_midi(*f.proc, 120.0, m, l, r); }
    for (int b = 0; b < 24; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); } // let the 5 ms release finish
    double tail = 0.0;
    for (int b = 0; b < 8; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); tail += block_energy(l); }
    CHECK(tail < held * 0.05);  // decayed after the pedal lifted
}

TEST_CASE("pitch-bend and mod-wheel reach the rendered audio",
          "[tempo-sampler][issue-111]") {
    // Render the same looped note three ways — no expression, full up-bend, full
    // mod-wheel — and confirm each expression materially changes the output (both
    // scale the playback rate, so the waveform diverges from the dry render).
    auto render = [](int mode, std::vector<float>& acc) {
        Fixture f;
        auto buf = sine(220.0, 48000.0, 48000);
        const float* ch[1] = {buf.data()};
        REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
        f.store.set_value(kTempoLoop, 1.0f);
        f.proc->set_loop_bpm_for_test(120.0);
        std::vector<float> l(512), r(512);
        { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
        REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
        { midi::MidiBuffer m;
          if (mode == 1) m.add(midi::MidiEvent::pitch_bend(0, 16383));  // full up bend
          if (mode == 2) m.add(midi::MidiEvent::cc(0, 1, 127));         // full mod wheel
          m.add(midi::MidiEvent::note_on(0, kRoot, 100));
          process_midi(*f.proc, 120.0, m, l, r); }
        acc.clear();
        for (int b = 0; b < 16; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
            acc.insert(acc.end(), l.begin(), l.end()); }
    };
    std::vector<float> dry, bent, modded;
    render(0, dry); render(1, bent); render(2, modded);

    double energy = 0.0, dbend = 0.0, dmod = 0.0;
    for (std::size_t i = 0; i < dry.size(); ++i) {
        energy += static_cast<double>(dry[i]) * dry[i];
        const double a = dry[i] - bent[i];
        const double b = dry[i] - modded[i];
        dbend += a * a; dmod += b * b;
    }
    REQUIRE(energy > 1e-3);            // the dry render is audible
    CHECK(dbend > energy * 0.10);      // +2 semitone bend clearly diverges
    CHECK(dmod > energy * 0.01);       // vibrato (small but real) diverges too
}

// Each slice maps to its own chromatic note (idx = note - root). A note off the
// map must be SILENT — it must NOT fall back to playing the whole sample (the
// Reaper/host "MIDI triggers the entire sample across the keyboard" bug).
TEST_CASE("MIDI note outside the slice map is silent (no whole-sample fallback)",
          "[tempo-sampler]") {
    Fixture f;
    f.store.set_value(kPlaybackMode, 2.0f);  // Slice: no whole-sample fallback
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    std::vector<float> l(512), r(512);
    process_block(*f.proc, 120.0, false, 0, l, r);
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));

    // Note BELOW the root (idx = note - root < 0) maps to no slice. Root = 60.
    double oor = 0.0;
    for (int b = 0; b < 8; ++b) {
        process_block(*f.proc, 120.0, b == 0, 59, l, r);
        for (int i = 0; i < 512; ++i) oor += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
    }
    CHECK(oor < 1e-9);  // silent — did NOT play the whole sample

    // Sanity: the root note (slice 0) still plays (we didn't mute everything).
    double inr = 0.0;
    for (int b = 0; b < 8; ++b) {
        process_block(*f.proc, 120.0, b == 0, kRoot, l, r);
        for (int i = 0; i < 512; ++i) inr += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
    }
    CHECK(inr > 1e-6);
}

// Changing SENS must reach the AUDIO trigger mapping (slices_stretched_), not
// just the UI slice count (slices_orig_). A higher key plays at high sensitivity
// (more slices) and goes silent at low sensitivity (fewer slices) — i.e. the
// keyboard mapping follows the slider.
TEST_CASE("sensitivity change reaches the keyboard trigger mapping", "[tempo-sampler]") {
    Fixture f;
    f.store.set_value(kPlaybackMode, 2.0f);  // Slice: per-key slice mapping
    auto loop = percussive_loop(48000, 8);  // ~8 onsets available
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    std::vector<float> l(512), r(512);
    process_block(*f.proc, 120.0, false, 0, l, r);
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto plays = [&](int note) {
        // Drain any one-shot voice still ringing from a previous trigger so we
        // measure a FRESH note, not a lingering slice tail.
        for (int b = 0; b < 80; ++b) process_block(*f.proc, 120.0, false, 0, l, r);
        double e = 0.0;
        for (int b = 0; b < 8; ++b) {
            process_block(*f.proc, 120.0, b == 0, note, l, r);
            for (int i = 0; i < 512; ++i) e += static_cast<double>(l[(size_t)i]) * l[(size_t)i];
        }
        return e;
    };

    const int root = kRoot;

    // High sensitivity -> several slices; key root+6 maps to a slice.
    f.store.set_value(kOnsetSens, 1.0f);
    f.proc->request_reanalyze();
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 7; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let the re-render refresh slices_stretched_
    CHECK(plays(root + 6) > 1e-6);   // root+6 triggers a slice

    // Low sensitivity -> 1 slice; root+6 must now be SILENT (mapping followed SENS).
    f.store.set_value(kOnsetSens, 0.0f);
    f.proc->request_reanalyze();
    REQUIRE(wait_for([&] { return f.proc->num_slices() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(plays(root + 6) < 1e-9);
}

TEST_CASE("render while playing is finite + stable (race)", "[tempo-sampler]") {
    Fixture f;
    auto buf = sine(220.0, 48000.0, 24000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 24000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    std::vector<float> l(512), r(512);
    // Hold a note while sweeping host tempo (re-renders fire on the worker).
    for (int b = 0; b < 40; ++b) {
        const double tempo = 80.0 + (b % 10) * 8.0;
        process_block(*f.proc, tempo, b == 0, kRoot, l, r);
        for (float v : l) REQUIRE(std::isfinite(v));
        for (float v : r) REQUIRE(std::isfinite(v));
    }
    SUCCEED();
}

// #race-2: changing tempo/slices while voices are sounding must NOT leave the
// trigger mapping pointing past the live buffer. The re-render publishes
// generation-safely and may SKIP the publish while a held voice still occupies a
// store slot; the slice map is now refreshed only AFTER a successful publish, so a
// skipped publish keeps the OLD (still-consistent) slices instead of stranding
// every note on silence until the next render. Sweep tempo under sustained voices,
// then a fresh trigger must still produce audio.
TEST_CASE("tempo sweep under held voices keeps fresh triggers audible",
          "[tempo-sampler][issue-race]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    std::vector<float> l(512), r(512);
    process_block(*f.proc, 120.0, false, 0, l, r);
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));

    const int root = kRoot;
    // Hold two distinct slice notes (different store generations as renders land),
    // and sweep host tempo so the worker keeps re-rendering / re-publishing.
    process_block(*f.proc, 100.0, true, root, l, r);       // voice A on
    for (int b = 0; b < 30; ++b) {
        const double tempo = 70.0 + (b % 12) * 10.0;        // re-renders fire
        const bool on = (b == 4);                           // voice B on mid-sweep
        process_block(*f.proc, tempo, on, root + 1, l, r);
        for (float v : l) REQUIRE(std::isfinite(v));
    }

    // A fresh trigger after all that churn must still map to a slice and sound.
    double energy = 0.0;
    for (int b = 0; b < 12; ++b) {
        process_block(*f.proc, 120.0, b == 0, root + 2, l, r);
        for (int i = 0; i < 512; ++i) energy += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
    }
    CHECK(energy > 1e-6);  // not stranded on silence by a skipped-publish slice update
}

TEST_CASE("drop replaces the loaded sample", "[tempo-sampler]") {
    Fixture f;
    auto a = sine(440.0, 48000.0, 24000); // 0.5 s
    const float* ca[1] = {a.data()};
    REQUIRE(f.proc->load_loop(ca, 1, 24000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    const std::uint64_t gen_a = f.proc->raw_generation();

    auto b = sine(220.0, 48000.0, 48000); // 1.0 s — different length
    const float* cb[1] = {b.data()};
    REQUIRE(f.proc->load_loop(cb, 1, 48000, 48000.0)); // "drop on top" path
    REQUIRE(f.proc->raw_generation() > gen_a);         // view sees a change

    std::vector<float> mono; float sr = 0; std::vector<long> slices;
    REQUIRE(f.proc->snapshot_for_view(mono, sr, slices));
    REQUIRE(mono.size() == 48000); // reflects B, not A
}

TEST_CASE("drop decodes an audio file off the audio thread", "[tempo-sampler]") {
    Fixture f;
    const std::string path = "/tmp/pulp_tempo_drop_test.wav";
    REQUIRE(write_wav(path, percussive_loop(48000, 4), 48000));

    f.proc->request_load_path(path);              // what the UI drop handler calls
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(f.proc->num_slices() >= 2);           // sliced on load
    std::remove(path.c_str());
}

// Regression: the standalone main.cpp factory runs INSIDE StandaloneApp::start()
// BEFORE start() binds the processor's StateStore. Queuing a command-line loop
// there must not touch state() (a synchronous load_loop -> analyze_locked ->
// state().get_value() dereferenced a null store and crashed before the window
// ever painted — the "empty screen" the live GPU host showed). request_load_path
// only stashes the path + sets a flag; the deferred worker (started in prepare(),
// after the store is bound) does the decode/analyze. This mirrors that ordering.
TEST_CASE("queued load before the state store is bound does not crash",
          "[tempo-sampler][issue-empty-screen]") {
    const std::string path = "/tmp/pulp_tempo_prebind_test.wav";
    REQUIRE(write_wav(path, percussive_loop(48000, 4), 48000));

    state::StateStore store;
    auto proc = std::make_unique<PulpTempoSamplerProcessor>();
    // Factory order: queue the loop while state_store_ is still null.
    proc->request_load_path(path);
    // start() order: bind the store, define params, THEN prepare() (start_worker).
    proc->set_state_store(&store);
    proc->define_parameters(store);
    store.set_value(kOnsetSens, 1.0f);  // exercise slicing (product default is whole-loop)
    format::PrepareContext ctx;
    ctx.sample_rate = 48000;
    ctx.max_buffer_size = 512;
    ctx.input_channels = 0;
    ctx.output_channels = 2;
    proc->prepare(ctx);  // worker starts here and picks up the queued path

    REQUIRE(wait_for([&] { return proc->has_sample(); }));
    REQUIRE(proc->num_slices() >= 2);  // analyzed/sliced once the store was bound
    std::remove(path.c_str());
}

TEST_CASE("invalid drop path is a graceful no-op", "[tempo-sampler]") {
    Fixture f;
    f.proc->request_load_path("/tmp/this-does-not-exist-xyz.wav");
    // Give the worker a moment; nothing should load and nothing should crash.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(f.proc->has_sample());
}

TEST_CASE("UI play_slice triggers audio with no host MIDI", "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    std::vector<float> l(512), r(512);
    process_block(*f.proc, 120.0, false, 0, l, r);  // publish at host tempo
    REQUIRE(wait_for([&] { return f.proc->published_frames() > 0; }));

    f.proc->play_slice(0, true);  // a UI click / key press on slice 0
    double energy = 0.0;
    for (int b = 0; b < 8; ++b) {
        process_block(*f.proc, 120.0, false, 0, l, r);  // note_on=false: no host MIDI
        for (int i = 0; i < 512; ++i) energy += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
        for (float v : l) REQUIRE(std::isfinite(v));
    }
    CHECK(energy > 1e-6);  // UI-injected note produced audio
    f.proc->play_slice(0, false);
}

TEST_CASE("root note remaps slice-to-key (idx = note - root)", "[tempo-sampler]") {
    Fixture f;
    // Default root is C2 (48 in this editor's labeling).
    REQUIRE(f.proc->slice_index_for_note_test(kRoot) == 0);
    REQUIRE(f.proc->slice_index_for_note_test(kRoot + 3) == 3);

    f.store.set_value(kRootNote, 48.0f);
    REQUIRE(f.proc->slice_index_for_note_test(48) == 0);
    REQUIRE(f.proc->slice_index_for_note_test(60) == 12);
}

TEST_CASE("onset sensitivity changes the slice count", "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 6); // 6 bursts
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    // Minimum sensitivity -> exactly ONE slice (the whole sample triggers as one).
    f.store.set_value(kOnsetSens, 0.0f);
    f.proc->request_reanalyze();
    REQUIRE(wait_for([&] { return f.proc->num_slices() == 1; }));
    const std::size_t low = f.proc->num_slices();
    REQUIRE(low == 1);

    // Higher sensitivity -> more slices (up to the available onsets).
    f.store.set_value(kOnsetSens, 1.0f);
    f.proc->request_reanalyze();
    REQUIRE(wait_for([&] { return f.proc->num_slices() > low; }));
    REQUIRE(f.proc->num_slices() > low);
}

// LOOP acts on currently-HELD notes: press a note, let its one-shot finish, then
// engage LOOP (without re-pressing) and it loops; disengage LOOP while still held
// and it stops. Natural expected behaviour.
TEST_CASE("LOOP toggle engages/disengages a currently-held note",
          "[tempo-sampler][issue-loop-held]") {
    Fixture f;
    auto loop = percussive_loop(48000, 8);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    f.store.set_value(kTempoLoop, 0.0f);                       // start one-shot
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));

    // Press + HOLD note 60 (slice 0); play the one-shot out until it goes silent.
    { midi::MidiBuffer m; m.add(midi::MidiEvent::note_on(0, kRoot, 100));
      process_midi(*f.proc, 120.0, m, l, r); }
    double tail = 0.0;
    for (int b = 0; b < 240; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
        tail = block_energy(l); }
    CHECK(tail < 1e-6);   // one-shot finished + silent, but the key is STILL held (no note-off)

    // Engage LOOP while held -> it re-engages looping with no re-press.
    f.store.set_value(kTempoLoop, 1.0f);
    double looped = 0.0;
    for (int b = 0; b < 400; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
        looped += block_energy(l); }
    CHECK(looped > 1e-3);

    // Disengage LOOP while still held -> looping stops (finishes the pass, then silent).
    f.store.set_value(kTempoLoop, 0.0f);
    for (int b = 0; b < 400; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
    double after = 0.0;
    for (int b = 0; b < 8; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
        after += block_energy(l); }
    CHECK(after < 1e-6);
}

// A MIDI panic (CC123 All Notes Off) must clear the HELD-note set, not just stop
// voices: otherwise a later LOOP-on resurrects a phantom chord the host already
// silenced. Mirrors the held-but-silent setup of the LOOP test, then panics.
TEST_CASE("MIDI panic clears the held set so LOOP cannot resurrect a phantom note",
          "[tempo-sampler][issue-loop-panic]") {
    Fixture f;
    auto loop = percussive_loop(48000, 8);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    f.store.set_value(kTempoLoop, 0.0f);                       // start one-shot
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));

    // Press + HOLD note 60 (no note-off); let the one-shot play out to silence.
    { midi::MidiBuffer m; m.add(midi::MidiEvent::note_on(0, kRoot, 100));
      process_midi(*f.proc, 120.0, m, l, r); }
    double tail = 0.0;
    for (int b = 0; b < 240; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
        tail = block_energy(l); }
    REQUIRE(tail < 1e-6);   // one-shot finished + silent, key still "held" (no note-off)

    // Host panic: All Notes Off. The held set must clear here.
    { midi::MidiBuffer m; m.add(midi::MidiEvent::cc(0, 123, 0));
      process_midi(*f.proc, 120.0, m, l, r); }

    // Engage LOOP. With the held set cleared, there is nothing to resurrect -> silent.
    f.store.set_value(kTempoLoop, 1.0f);
    double after = 0.0;
    for (int b = 0; b < 400; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
        after += block_energy(l); }
    CHECK(after < 1e-6);   // no phantom loop (without the panic-clear this rings)
}

// CC120 (All Sound Off) hard-stops every voice immediately (vs CC123's release)
// AND clears the held set — a ringing looped note is silenced at once and cannot
// be resurrected by a later LOOP-on.
TEST_CASE("CC120 All Sound Off hard-stops ringing voices and clears the held set",
          "[tempo-sampler][issue-loop-panic]") {
    Fixture f;
    auto loop = percussive_loop(48000, 8);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    f.store.set_value(kTempoLoop, 1.0f);                      // loop so the voice keeps ringing
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));

    // Hold note 60; let the loop establish a steady ring.
    { midi::MidiBuffer m; m.add(midi::MidiEvent::note_on(0, kRoot, 100));
      process_midi(*f.proc, 120.0, m, l, r); }
    double ringing = 0.0;
    for (int b = 0; b < 60; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
        ringing += block_energy(l); }
    REQUIRE(ringing > 1e-3);                                  // actively looping

    // All Sound Off: immediate silence (next block), no release tail.
    { midi::MidiBuffer m; m.add(midi::MidiEvent::cc(0, 120, 0));
      process_midi(*f.proc, 120.0, m, l, r); }
    double after = 0.0;
    for (int b = 0; b < 4; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
        after += block_energy(l); }
    CHECK(after < 1e-6);                                      // hard-stopped at once

    // Held set cleared too: toggling LOOP off then on must not resurrect note 60.
    f.store.set_value(kTempoLoop, 0.0f);
    { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
    f.store.set_value(kTempoLoop, 1.0f);
    double resurrected = 0.0;
    for (int b = 0; b < 200; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
        resurrected += block_energy(l); }
    CHECK(resurrected < 1e-6);
}

// Every slice plays at the SAME speed (native rate 1.0): the loop is time-
// stretched ONCE into a single host-tempo buffer and each slice is a [start,end)
// region into it, so no slice can play faster/slower than another. Prove it by
// triggering the shortest + longest slice one-shot and asserting each plays for ~
// its region length — a per-slice rate bug would make duration not track length.
TEST_CASE("all slices play back at the same (native) rate — none faster/slower",
          "[tempo-sampler][issue-slice-rate]") {
    Fixture f;
    f.store.set_value(kPlaybackMode, 2.0f);  // Slice: each key is its own slice at native rate
    auto loop = percussive_loop(48000, 6);            // 48k loop into 48k host -> R=1 at matched tempo
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);
    f.store.set_value(kTempoLoop, 0.0f);              // loop off so a voice stops at region end
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }   // host==loop -> R=1
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 3; }));

    std::vector<float> mono; float sr = 0.0f; std::vector<long> slices;
    REQUIRE(f.proc->snapshot_for_view(mono, sr, slices));
    REQUIRE(slices.size() >= 4);
    const int root = static_cast<int>(f.store.get_value(kRootNote));

    // Play a slice one-shot and count samples until it falls silent (2 silent blocks).
    auto play_len = [&](int idx) -> long {
        std::vector<float> ll(512), rr(512);
        { midi::MidiBuffer m; m.add(midi::MidiEvent::note_on(0, root + idx, 100));
          process_midi(*f.proc, 120.0, m, ll, rr); }
        long active = 512; int silent = 0;
        for (int b = 0; b < 400 && silent < 2; ++b) {
            midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, ll, rr);
            if (block_energy(ll) > 1e-7) { active = static_cast<long>(b + 2) * 512; silent = 0; }
            else ++silent;
        }
        { midi::MidiBuffer m; m.add(midi::MidiEvent::note_off(0, root + idx));
          process_midi(*f.proc, 120.0, m, ll, rr); }
        for (int b = 0; b < 60; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, ll, rr); } // drain
        return active;
    };

    // Shortest + longest slice region (seed from slice 0 so no <climits> needed).
    int si = 0, li = 0; long smin = slices[1] - slices[0], smax = smin;
    for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
        const long len = slices[i + 1] - slices[i];
        if (len < smin) { smin = len; si = static_cast<int>(i); }
        if (len > smax) { smax = len; li = static_cast<int>(i); }
    }
    REQUIRE(smax > smin);
    const long played_short = play_len(si);
    const long played_long  = play_len(li);
    INFO("short slice " << si << " region=" << smin << " played=" << played_short);
    INFO("long  slice " << li << " region=" << smax << " played=" << played_long);

    // Each slice plays for ~its own region length (rate ≈ 1.0, not 0.5x/2x). A
    // generous band absorbs the ~5 ms release fade + 512-sample block granularity.
    auto within_rate1 = [](long played, long region) {
        return played >= region * 6 / 10 && played <= region * 16 / 10 + 1200;
    };
    CHECK(within_rate1(played_short, smin));
    CHECK(within_rate1(played_long, smax));
    // And the longer region genuinely plays longer — the SAME rate orders them.
    CHECK(played_long > played_short);
}

// bar_exact_bpm derives the EXACT bar-multiple tempo so a loop tiles perfectly at
// any host tempo, and uses the host as an octave prior to resolve half/double
// detection errors.
TEST_CASE("bar-exact tempo: perfect tiling + host-prior octave resolution",
          "[tempo-sampler][issue-tempo-barsnap]") {
    using P = PulpTempoSamplerProcessor;
    const double sr = 48000.0;
    // A 2-bar (8-beat) loop whose TRUE tempo is fractional (103.4). The analyzer
    // rounds to 103; bar_exact recovers ~103.4.
    const double true_bpm = 103.4;
    const long frames = std::lround(8.0 * 60.0 / true_bpm * sr);   // exactly 8 beats long
    const double exact = P::bar_exact_bpm(frames, sr, /*host*/120.0, /*analyzer*/103.0);
    CHECK(std::abs(exact - true_bpm) < 0.05);
    // Tiling invariant: frames * exact == 8 beats * 60 * sr (host-independent), so
    // the loop fills an exact 2 bars at ANY host tempo.
    const double ideal = 8.0 * 60.0 * sr;
    CHECK(std::abs(static_cast<double>(frames) * exact - ideal) < 0.002 * ideal);

    // OCTAVE FIX: the analyzer detected DOUBLE tempo (206) for the same 2-bar loop,
    // but the host is 120, so bar_exact picks the 8-beat reading (~103.4), not 206.
    CHECK(std::abs(P::bar_exact_bpm(frames, sr, 120.0, 206.0) - true_bpm) < 0.1);
    // With NO host hint it trusts the analyzer's octave (16 beats at ~206.8).
    CHECK(P::bar_exact_bpm(frames, sr, 0.0, 206.0) > 200.0);
    // And a genuine 3-bar loop (12 beats) is NOT collapsed to 2 bars by the host
    // prior — the octave neighbours of the analyzer's count don't include 8.
    const long f3 = std::lround(12.0 * 60.0 / 155.0 * sr);         // 3 bars at 155
    const double e3 = P::bar_exact_bpm(f3, sr, 120.0, 155.0);
    CHECK(std::abs(static_cast<double>(f3) * e3 - 12.0 * 60.0 * sr) < 0.002 * 12.0 * 60.0 * sr);
    CHECK(e3 > 140.0);  // ~155 (3 bars), not ~103 (2 bars)

    // Degenerate inputs pass straight through.
    CHECK(P::bar_exact_bpm(frames, sr, 120.0, 0.0) == 0.0);
    CHECK(P::bar_exact_bpm(0, sr, 120.0, 120.0) == 120.0);
}

// A manually-set TEMPO must survive close+reopen (serialize/deserialize) — it
// lives in a non-param atomic that v1 state did not persist.
TEST_CASE("tempo override persists across serialize/deserialize",
          "[tempo-sampler][issue-tempo-persist]") {
    Fixture f;
    auto buf = sine(440.0, 48000.0, 48000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    f.proc->set_target_bpm(150.0);                 // manual tempo (auto-unlinks)
    CHECK_FALSE(f.proc->tempo_linked());
    const auto blob = f.proc->serialize_plugin_state();
    REQUIRE(!blob.empty());

    Fixture g;                                     // a freshly reopened plugin instance
    REQUIRE(g.proc->deserialize_plugin_state(
        std::span<const std::uint8_t>(blob.data(), blob.size())));
    REQUIRE(wait_for([&] { return g.proc->has_sample(); }));
    CHECK(std::lround(g.proc->effective_bpm()) == 150);  // tempo restored, not reset
    CHECK_FALSE(g.proc->tempo_linked());                 // still manual
}

// TEMPO LINK: default linked (follows host); a manual value unlinks; re-link
// follows the host again. Mirrors the editor's LINK toggle + drag-to-unlink.
TEST_CASE("tempo LINK: default-linked follows host, manual unlinks, relink re-follows",
          "[tempo-sampler][issue-tempo-link]") {
    Fixture f;
    auto buf = sine(440.0, 48000.0, 48000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    f.proc->set_loop_bpm_for_test(120.0);
    std::vector<float> l(512), r(512);

    // Default LINKED → follows the host transport.
    CHECK(f.proc->tempo_linked());
    process_block(*f.proc, 90.0, false, 0, l, r);
    const long exp90 = std::llround(48000.0 * 120.0 / 90.0);
    REQUIRE(wait_for([&] { return f.proc->published_frames() == exp90; }));
    process_block(*f.proc, 140.0, false, 0, l, r);       // change host tempo
    const long exp140 = std::llround(48000.0 * 120.0 / 140.0);
    REQUIRE(wait_for([&] { return f.proc->published_frames() == exp140; }));  // auto-follows

    // Manual tempo UNLINKS — host changes are now ignored.
    f.proc->set_target_bpm(100.0);
    CHECK_FALSE(f.proc->tempo_linked());
    process_block(*f.proc, 200.0, false, 0, l, r);       // host ignored
    const long exp100 = std::llround(48000.0 * 120.0 / 100.0);
    REQUIRE(wait_for([&] { return f.proc->published_frames() == exp100; }));

    // Re-link → follows the host again.
    f.proc->set_tempo_linked(true);
    CHECK(f.proc->tempo_linked());
    process_block(*f.proc, 90.0, false, 0, l, r);
    REQUIRE(wait_for([&] { return f.proc->published_frames() == exp90; }));
}

// In the standalone (no DAW transport) LINKED resolves to the loop's OWN detected
// tempo, so a dropped loop plays at its natural tempo (R≈1) rather than a host
// default. set_standalone_host(true) is what the editor reports on view-open.
TEST_CASE("standalone LINKED follows the loop's detected tempo, not the host default",
          "[tempo-sampler][issue-tempo-link]") {
    Fixture f;
    f.proc->set_standalone_host(true);
    auto buf = sine(440.0, 48000.0, 48000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    f.proc->set_loop_bpm_for_test(120.0);            // detected = 120
    CHECK(f.proc->tempo_linked());
    std::vector<float> l(512), r(512);
    process_block(*f.proc, 90.0, false, 0, l, r);    // host says 90 — ignored in standalone
    // Linked-standalone renders at the detected tempo → R = 120/120 = 1 → raw length.
    REQUIRE(wait_for([&] { return f.proc->published_frames() == 48000; }));
}

// Unlinking engages the SAMPLE's own (fractional, bar-snapped) tempo so the loop
// plays at its natural speed and the readout shows it — not the host BPM.
TEST_CASE("unlinking engages the sample's own detected tempo (not the host)",
          "[tempo-sampler][issue-tempo-link]") {
    Fixture f;
    auto buf = sine(440.0, 48000.0, 48000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    f.proc->set_loop_bpm_for_test(103.4);   // the loop's fractional detected tempo
    REQUIRE(f.proc->tempo_linked());        // default linked
    f.proc->set_tempo_linked(false);        // unlink → adopt the sample's tempo
    CHECK_FALSE(f.proc->tempo_linked());
    CHECK(std::abs(f.proc->effective_bpm() - 103.4) < 0.01);
}

// A fresh drop WHILE UNLINKED adopts the new sample's detected tempo (the readout
// follows the dropped sample, not a stale value).
TEST_CASE("a fresh drop while unlinked adopts the new sample's detected tempo",
          "[tempo-sampler][issue-tempo-link]") {
    Fixture f;
    auto a = percussive_loop(48000, 8);
    const float* ca[1] = {a.data()};
    REQUIRE(f.proc->load_loop(ca, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    f.proc->set_tempo_linked(false);        // unlink
    auto b = percussive_loop(48000, 4);     // a DIFFERENT loop
    const float* cb[1] = {b.data()};
    REQUIRE(f.proc->load_loop(cb, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    CHECK_FALSE(f.proc->tempo_linked());
    const double det = f.proc->detected_bpm();
    if (det > 0.0)                          // when the analyzer found a tempo for the drop
        CHECK(std::abs(f.proc->effective_bpm() - det) < 0.01);  // override adopted it
}

// Product default is CLASSIC: every key plays the WHOLE tempo-matched loop
// (region [0, N)), so holding a key with LOOP=Forward tiles the entire loop — not
// a chop. The Slice DEFAULT, by contrast, chops the loop into several slices
// (never 1), so switching to Slice is immediately playable per-key.
TEST_CASE("default is Classic (whole loop); Slice chops into >1",
          "[tempo-sampler][issue-loop-tile]") {
    state::StateStore store;
    auto proc = std::make_unique<PulpTempoSamplerProcessor>();
    proc->set_state_store(&store);
    proc->define_parameters(store);   // installs DEFAULT mode (Classic) + SENS
    format::PrepareContext ctx; ctx.sample_rate = 48000; ctx.max_buffer_size = 512;
    ctx.input_channels = 0; ctx.output_channels = 2; proc->prepare(ctx);
    CHECK(store.get_value(kPlaybackMode) == 0.0f);   // default mode = Classic
    auto loop = percussive_loop(48000, 8);
    const float* ch[1] = {loop.data()};
    REQUIRE(proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return proc->has_sample(); }));
    CHECK(proc->num_slices() > 1);                    // Slice default chops (never 1)
    // Classic: the root's region is the WHOLE published loop, [0, N).
    const long whole = proc->published_frames();
    const auto r = proc->region_range_for_note_test(48, /*mode=*/0);  // 0 = Classic
    CHECK(r.first == 0);
    CHECK(static_cast<long>(r.second) == whole);
}

// AUDIO-LEVEL tiling proof (the "loops perfectly" claim): a click-per-beat loop,
// played whole (1 slice) with LOOP on at the matched tempo, must reproduce clicks
// exactly on the beat grid across multiple loop passes — proving the loop length,
// the downbeat alignment, AND no per-pass drift (not just num_frames).
TEST_CASE("whole-loop LOOP repeats clicks on the beat grid (no drift)",
          "[tempo-sampler][issue-loop-tile]") {
    Fixture f;
    f.store.set_value(kOnsetSens, 0.0f);              // whole loop (1 slice)
    const double sr = 48000.0, bpm = 120.0;
    const long beat = std::lround(60.0 / bpm * sr);   // 24000
    const int beats = 4;                              // 1 bar
    const long frames = beat * beats;                 // 96000
    std::vector<float> buf(static_cast<size_t>(frames), 0.0f);
    for (int b = 0; b < beats; ++b) {                 // a click at each beat
        const long p = b * beat; buf[static_cast<size_t>(p)] = 1.0f;
        if (p + 1 < frames) buf[static_cast<size_t>(p + 1)] = -0.9f;
    }
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, frames, sr));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() == 1; }));
    f.proc->set_loop_bpm_for_test(bpm);               // exact -> R=1 at host 120 (identity stretch)
    f.store.set_value(kTempoLoop, 1.0f);              // LOOP on
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, bpm, m, l, r); }   // settle the render
    REQUIRE(wait_for([&] { return std::abs(f.proc->published_frames() - frames) <= 8; }));

    std::vector<float> out; out.reserve(300000);
    { midi::MidiBuffer m; m.add(midi::MidiEvent::note_on(0, kRoot, 110));
      process_midi(*f.proc, bpm, m, l, r); out.insert(out.end(), l.begin(), l.end()); }
    for (int blk = 0; blk < 520; ++blk) {             // ~2.7 bars
        midi::MidiBuffer m; process_midi(*f.proc, bpm, m, l, r);
        out.insert(out.end(), l.begin(), l.end());
    }
    // Local-maxima peak pick; each must land on the beat grid (period = frames).
    std::vector<long> peaks;
    for (long i = 1; i + 1 < static_cast<long>(out.size()); ++i) {
        const float a = std::abs(out[static_cast<size_t>(i)]);
        if (a > 0.3f && a >= std::abs(out[static_cast<size_t>(i - 1)]) &&
            a > std::abs(out[static_cast<size_t>(i + 1)]))
            peaks.push_back(i);
    }
    REQUIRE(peaks.size() >= 8);                        // >= 8 beats over ~2.7 bars
    long long worst = 0;
    for (long p : peaks) {
        const long grid = std::llround(static_cast<double>(p) / beat) * beat;
        worst = std::max<long long>(worst, std::llabs(static_cast<long long>(p - grid)));
    }
    INFO("worst beat-grid deviation = " << worst << " samples (" << (worst * 1000.0 / sr) << " ms)");
    CHECK(worst <= 128);                              // within ~2.7 ms of the grid, every pass
}

// Same proof while STRETCHING (host slower than the loop): the clicks must land
// on the HOST beat grid — "worse when slowing" was the user's report, so this
// guards the stretched (R>1) path specifically.
TEST_CASE("whole-loop LOOP tiles under stretch (slowed host)",
          "[tempo-sampler][issue-loop-tile]") {
    Fixture f;
    f.store.set_value(kOnsetSens, 0.0f);
    const double sr = 48000.0, loop_bpm = 120.0, host = 90.0;   // slowing: R = 120/90 = 1.333
    const long beat_src = std::lround(60.0 / loop_bpm * sr);     // 24000 (loop's own beat)
    const int beats = 4; const long frames = beat_src * beats;   // 96000 (1 bar @120)
    std::vector<float> buf(static_cast<size_t>(frames), 0.0f);
    for (int b = 0; b < beats; ++b) { const long p = b * beat_src;
        buf[static_cast<size_t>(p)] = 1.0f; if (p + 1 < frames) buf[static_cast<size_t>(p + 1)] = -0.9f; }
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, frames, sr));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() == 1; }));
    f.proc->set_loop_bpm_for_test(loop_bpm);
    f.store.set_value(kTempoLoop, 1.0f);
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, host, m, l, r); }     // render at the slowed host
    const long beat_host = std::lround(60.0 / host * sr);            // 32000 (beat at 90)
    REQUIRE(wait_for([&] { return std::abs(f.proc->published_frames() - beat_host * beats) <= 8; }));

    std::vector<float> out; out.reserve(400000);
    { midi::MidiBuffer m; m.add(midi::MidiEvent::note_on(0, kRoot, 110));
      process_midi(*f.proc, host, m, l, r); out.insert(out.end(), l.begin(), l.end()); }
    for (int blk = 0; blk < 640; ++blk) {                            // ~2.5 bars @90
        midi::MidiBuffer m; process_midi(*f.proc, host, m, l, r);
        out.insert(out.end(), l.begin(), l.end());
    }
    std::vector<long> peaks;
    for (long i = 1; i + 1 < static_cast<long>(out.size()); ++i) {
        const float a = std::abs(out[static_cast<size_t>(i)]);
        if (a > 0.3f && a >= std::abs(out[static_cast<size_t>(i - 1)]) &&
            a > std::abs(out[static_cast<size_t>(i + 1)])) peaks.push_back(i);
    }
    REQUIRE(peaks.size() >= 8);
    long long worst = 0;
    for (long p : peaks) {
        const long grid = std::llround(static_cast<double>(p) / beat_host) * beat_host;
        worst = std::max<long long>(worst, std::llabs(static_cast<long long>(p - grid)));
    }
    INFO("slowed worst beat-grid deviation = " << worst << " samples (" << (worst * 1000.0 / sr) << " ms)");
    CHECK(worst <= 256);   // within ~5 ms of the host grid even when stretched 1.33x
}

// SLICE region semantics: a key plays ONLY its slice ([slice k, slice k+1) of the
// published buffer); a key outside the slice range plays nothing (never the whole
// sample). With SENS 0 the loop is a single whole-loop slice on the root.
TEST_CASE("Slice: each key plays just its slice; out-of-range key is silent",
          "[tempo-sampler][issue-loop-tile][slice]") {
    Fixture f;
    const double sr = 48000.0, bpm = 120.0;
    const long beat = std::lround(60.0 / bpm * sr);        // 24000
    const int beats = 4; const long frames = beat * beats; // 96000 (1 bar)
    std::vector<float> buf(static_cast<size_t>(frames), 0.0f);
    for (int b = 0; b < beats; ++b) { const long p = b * beat;
        buf[static_cast<size_t>(p)] = 1.0f;
        if (p + 1 < frames) buf[static_cast<size_t>(p + 1)] = -0.9f; }
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, frames, sr));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    f.store.set_value(kOnsetSens, 1.0f);                   // CHOP: max slices
    f.proc->request_reanalyze();
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));
    f.proc->set_loop_bpm_for_test(bpm);
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, bpm, m, l, r); }   // publish
    REQUIRE(wait_for([&] { return std::abs(f.proc->published_frames() - frames) <= 8; }));
    const auto pub = static_cast<std::uint64_t>(f.proc->published_frames());

    const auto s0 = f.proc->region_range_for_note_test(kRoot);
    const auto s1 = f.proc->region_range_for_note_test(kRoot + 1);
    const auto soob = f.proc->region_range_for_note_test(kRoot + 99);
    CHECK(s0.first == 0u);
    CHECK(s0.second < pub);                 // slice 0 is a FRACTION, not the whole loop
    CHECK(s0.second == s1.first);           // contiguous: slice-1 start = slice-0 end
    CHECK(s1.second > s1.first);
    CHECK(s1.second <= pub);
    CHECK((soob.first == 0u && soob.second == 0u));   // out of range -> nothing
}

// Slicing quality: no sliver slices (incl. the first, which onset spacing doesn't
// guard) and every cut snapped to a zero-crossing so slice edges don't click.
TEST_CASE("slice boundaries respect a minimum length and land on zero-crossings",
          "[tempo-sampler][issue-slicing]") {
    Fixture f;
    auto loop = percussive_loop(48000, 8);  // dense onsets to stress short slices
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    f.store.set_value(kOnsetSens, 1.0f);     // max slices — where slivers appeared
    f.proc->request_reanalyze();
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 3; }));

    std::vector<float> mono; float sr = 0.0f; std::vector<long> slices;
    REQUIRE(f.proc->snapshot_for_view(mono, sr, slices));
    REQUIRE(slices.size() >= 4);             // 0, >=2 interior cuts, end

    // (a) every slice — including the first [0,cut) and last [cut,end) — is at least
    // ~30 ms long (no sliver). The internal floor is 40 ms; assert a safe lower bound.
    const long min_len = static_cast<long>(0.030 * sr);
    for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
        INFO("slice " << i << " [" << slices[i] << "," << slices[i + 1] << ")");
        CHECK(slices[i + 1] - slices[i] >= min_len);
    }
    // (b)+(c) each INTERIOR boundary is a zero-crossing (sign change), so both the
    // end of the previous slice and the start of the next begin at zero — no click.
    for (std::size_t i = 1; i + 1 < slices.size(); ++i) {
        const long b = slices[i];
        REQUIRE(b > 0); REQUIRE(b < static_cast<long>(mono.size()));
        const bool sign_change = (mono[static_cast<size_t>(b - 1)] <= 0.0f) !=
                                 (mono[static_cast<size_t>(b)] <= 0.0f);
        INFO("boundary " << b << " prev=" << mono[static_cast<size_t>(b - 1)]
                         << " cur=" << mono[static_cast<size_t>(b)]);
        CHECK(sign_change);
    }
}

// Item A: the footer TEMPO box drives a target-tempo override. Engaging it pins
// the render denominator R = loop_bpm / target, so the published (tempo-matched)
// buffer length is round(in * loop / target) — a faster target shortens it.
TEST_CASE("target-bpm override re-stretches the published buffer length", "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE_FALSE(f.proc->tempo_override_active());  // follows host until engaged

    // Pin the loop's source tempo so R is deterministic.
    f.proc->set_loop_bpm_for_test(120.0);

    // The store has 2 slots and is freed by audio-thread acks, so pump a block
    // per poll (as a real host would) to keep slots flowing across re-renders.
    std::vector<float> l(512), r(512);
    auto pump_until = [&](long target_len) {
        return wait_for([&] {
            process_block(*f.proc, 120.0, false, 0, l, r);
            return f.proc->published_frames() == target_len;
        });
    };

    f.proc->set_target_bpm(200.0);  // faster target -> shorter buffer
    REQUIRE(f.proc->tempo_override_active());
    REQUIRE(f.proc->effective_bpm() == 200.0);
    const long fast = static_cast<long>(std::llround(48000.0 * 120.0 / 200.0));  // 28800
    REQUIRE(pump_until(fast));

    f.proc->set_target_bpm(80.0);   // slower target -> longer buffer
    const long slow = static_cast<long>(std::llround(48000.0 * 120.0 / 80.0));   // 72000
    REQUIRE(pump_until(slow));
    REQUIRE(slow > fast);

    // Clamp to [20,400].
    f.proc->set_target_bpm(5000.0);
    REQUIRE(f.proc->effective_bpm() == 400.0);
}

// The per-block host-tempo read in process() must NOT clobber an engaged
// override back to the host's tempo (the bug the override guards against).
TEST_CASE("target-bpm override survives the per-block host tempo", "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    f.proc->set_loop_bpm_for_test(120.0);

    f.proc->set_target_bpm(200.0);
    // Drive several process() blocks at a DIFFERENT host tempo; the override wins.
    std::vector<float> l(512), r(512);
    for (int i = 0; i < 8; ++i) process_block(*f.proc, 90.0, false, 0, l, r);
    const long expected = static_cast<long>(std::llround(48000.0 * 120.0 / 200.0));  // 28800
    REQUIRE(wait_for([&] { return f.proc->published_frames() == expected; }));
    const long host_len = static_cast<long>(std::llround(48000.0 * 120.0 / 90.0)); // 64000
    REQUIRE(f.proc->published_frames() != host_len);
    REQUIRE(f.proc->effective_bpm() == 200.0);
}

// Item B: the framework SettingsPanel reports a natural height taller than the
// fixed sampler editor, so the standalone host can grow the window on the
// Settings tab (instead of squishing the device dropdowns into 372px).
TEST_CASE("settings panel natural height exceeds the editor height", "[tempo-sampler]") {
    PulpTempoSamplerProcessor p;
    const auto vs = p.view_size();
    REQUIRE(format::SettingsPanel::preferred_height() >
            static_cast<int>(vs.preferred_height));
    // Room for header + inner tab bar + every Audio-tab row at full size.
    REQUIRE(format::SettingsPanel::preferred_height() == 620);
}

TEST_CASE("plugin state round-trips the loaded sample (close/reopen)", "[tempo-sampler]") {
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};

    std::vector<std::uint8_t> blob;
    {
        Fixture a;
        REQUIRE(a.proc->load_loop(ch, 1, 48000, 48000.0));
        REQUIRE(wait_for([&] { return a.proc->has_sample(); }));
        blob = a.proc->serialize_plugin_state();
        REQUIRE(!blob.empty());
    }
    // Fresh processor instance (simulates close+reopen / DAW project reload).
    Fixture b;
    REQUIRE_FALSE(b.proc->has_sample());          // starts empty
    REQUIRE(b.proc->deserialize_plugin_state(blob));
    REQUIRE(wait_for([&] { return b.proc->has_sample(); }));  // restored

    std::vector<float> mono; float sr = 0; std::vector<long> sl;
    REQUIRE(b.proc->snapshot_for_view(mono, sr, sl));
    REQUIRE(mono.size() == 48000);                // same audio came back
    REQUIRE(sr == 48000.0f);
}

// The view-level drop-target test instantiates a WaveformDropView, which pulls
// the pulp::view link (SDL3/X11 on Linux). Gate to Apple so the advisory Linux
// test lane doesn't drag in the desktop windowing stack — matching the
// screenshot tool's CMake gate.
#if defined(__APPLE__)
TEST_CASE("WaveformDropView accepts audio drops, rejects others", "[tempo-sampler]") {
    WaveformDropView v;
    std::string dropped;
    v.on_file_dropped = [&](const std::string& p) { dropped = p; };

    view::DropData audio;
    audio.type = view::DropData::Type::files;
    audio.file_paths = {"/music/loop.WAV"};  // case-insensitive extension
    REQUIRE(v.accept_drag(audio, {}));
    REQUIRE(v.accept_drop(audio, {}));
    REQUIRE(dropped == "/music/loop.WAV");

    dropped.clear();
    view::DropData other;
    other.type = view::DropData::Type::files;
    other.file_paths = {"/docs/readme.txt"};
    REQUIRE_FALSE(v.accept_drag(other, {}));
    REQUIRE_FALSE(v.accept_drop(other, {}));
    REQUIRE(dropped.empty());
}

TEST_CASE("empty drop area click triggers browse", "[tempo-sampler]") {
    WaveformDropView v;  // no sample loaded -> empty state
    bool browsed = false;
    v.on_browse = [&] { browsed = true; };

    view::MouseEvent press;
    press.button = view::MouseButton::left;
    press.is_down = true;
    v.on_mouse_event(press);
    REQUIRE(browsed);  // a click in the empty area opens the file picker
}

// #race-2 (end-to-end): the literal M1 scenario — playing the sampler from the
// QWERTY musical-typing keyboard must KEEP producing audio across tempo and slice
// (sensitivity) changes, not go silent until you adjust again. Drives the real
// keyboard path: KeyEvent -> on_note_on -> Processor::keyboard_play_on ->
// ui_note_on -> process() drains the queue -> trigger_note -> voice.
TEST_CASE("QWERTY typing keeps triggering audio across tempo + slice changes",
          "[tempo-sampler][issue-race]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);

    // Wire the editor's QWERTY typing to the processor exactly like the running app.
    SamplerEditorRoot root;
    root.hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    root.current_root_note = [] { return kRoot; };          // 'a' = root note 60 = slice 0
    root.keyboard_window_visible = [] { return true; };  // typing only plays when shown
    root.typing.on_note_on  = [&](int n, float v) { f.proc->keyboard_play_on(n, v); };
    root.typing.on_note_off = [&](int n) { f.proc->keyboard_play_off(n); };
    auto key = [](view::KeyCode k, bool down) {
        view::KeyEvent e; e.key = k; e.is_down = down; return e;
    };

    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }   // kick the first render
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));

    // Press a QWERTY key, then run blocks at `tempo` (drains the ui-note queue +
    // re-renders on a tempo change) and sum the output energy of the keyed note.
    auto qwerty_energy = [&](view::KeyCode k, double tempo) {
        REQUIRE(root.on_key_event(key(k, true)));   // down -> keyboard_play_on
        double e = 0.0;
        for (int b = 0; b < 12; ++b) {
            midi::MidiBuffer m;
            process_midi(*f.proc, tempo, m, l, r);
            for (float v : l) e += static_cast<double>(v) * v;
        }
        root.on_key_event(key(k, false));            // up -> keyboard_play_off
        return e;
    };

    // Baseline: typing 'a' at the loaded tempo plays.
    CHECK(qwerty_energy(view::KeyCode::a, 120.0) > 1e-6);

    // Change tempo (host 90 -> R != 1, worker re-renders): typing 's' still plays.
    CHECK(qwerty_energy(view::KeyCode::s, 90.0) > 1e-6);
    // And again at a third tempo, typing 'd'.
    CHECK(qwerty_energy(view::KeyCode::d, 150.0) > 1e-6);

    // Change slice sensitivity (re-slice + re-render): typing 'a' still plays.
    f.store.set_value(kOnsetSens, 0.8f);
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));
    CHECK(qwerty_energy(view::KeyCode::a, 90.0) > 1e-6);
}

// The typing keyboard is a chromatic piano — the home row a,s,d,f,g,h,j,k plays the
// WHITE notes and the upper row w,e,t,y,u,o,p plays the SHARPS above them (gaps at
// E/F and B/C). Each key is a distinct semitone, so with slice-per-semitone every
// key triggers a UNIQUE consecutive slice (a=0, w=1, s=2, e=3, d=4, ...). This locks
// the chromatic 1:1 mapping: no two distinct keys share a slice (guarding against a
// remap that would collapse the black-key row onto the white slices and double them).
TEST_CASE("QWERTY typing maps every key to a unique chromatic slice (no doubling)",
          "[tempo-sampler][issue-typing-double]") {
    Fixture f;

    // Drive the REAL path that carried the doubling bug: SamplerEditorRoot key event
    // -> typing controller -> keyboard_play_on -> ui_note_on(trigger). Read the actual
    // trigger note keyboard_play_on fired (not the controller's note) so a remap that
    // collapsed two keys onto one slice would be caught.
    SamplerEditorRoot root;
    root.hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    root.current_root_note = [] { return kRoot; };
    root.keyboard_window_visible = [] { return true; };
    root.typing.on_note_on  = [&](int n, float v) { f.proc->keyboard_play_on(n, v); };
    root.typing.on_note_off = [&](int n) { f.proc->keyboard_play_off(n); };
    auto key = [](view::KeyCode k, bool down) { view::KeyEvent e; e.key = k; e.is_down = down; return e; };
    auto slice_of = [&](view::KeyCode k) {
        root.on_key_event(key(k, true));
        const auto trig = f.proc->held_trigger_notes_for_test();
        const int slice = trig.empty() ? -1 : f.proc->slice_index_for_note_test(trig.back());
        root.on_key_event(key(k, false));
        return slice;
    };

    // White row -> white notes: a,s,d,f,g,h,j,k = slices 0,2,4,5,7,9,11,12.
    CHECK(slice_of(view::KeyCode::a) == 0);
    CHECK(slice_of(view::KeyCode::s) == 2);
    CHECK(slice_of(view::KeyCode::d) == 4);
    CHECK(slice_of(view::KeyCode::f) == 5);
    CHECK(slice_of(view::KeyCode::g) == 7);
    CHECK(slice_of(view::KeyCode::h) == 9);
    CHECK(slice_of(view::KeyCode::j) == 11);
    CHECK(slice_of(view::KeyCode::k) == 12);
    // Black row -> sharps above the white keys (gaps where no black key exists):
    // w,e,t,y,u = slices 1,3,6,8,10.
    CHECK(slice_of(view::KeyCode::w) == 1);
    CHECK(slice_of(view::KeyCode::e) == 3);
    CHECK(slice_of(view::KeyCode::t) == 6);
    CHECK(slice_of(view::KeyCode::y) == 8);
    CHECK(slice_of(view::KeyCode::u) == 10);
    // The full interleaved run a,w,s,e,d,f,t,g is strictly 0..7 — no two keys collide.
    view::KeyCode run[] = {view::KeyCode::a, view::KeyCode::w, view::KeyCode::s, view::KeyCode::e,
                           view::KeyCode::d, view::KeyCode::f, view::KeyCode::t, view::KeyCode::g};
    std::set<int> seen;
    for (int i = 0; i < 8; ++i) {
        int s = slice_of(run[i]);
        INFO("run key index " << i);
        CHECK(s == i);              // consecutive
        CHECK(seen.insert(s).second);  // unique — the no-doubling guarantee
    }
}

// Lowering the tempo made the output sound "blown out / clipping." Voices sum into
// the output with no headroom, so overlapping slices can exceed full-scale — worst
// at slow tempos, where each slice is stretched LONGER and successive triggers
// overlap. The master soft-limiter bounds the summed mix to +/-1.0. Drive a hot loop
// at a slow tempo, trigger every slice at once (max polyphony — a sum that would far
// exceed full-scale unprotected), and assert the OUTPUT the device sees stays
// bounded while still being loud (the limiter engaged rather than silencing).
TEST_CASE("overlapping slices at slow tempo never clip the output",
          "[tempo-sampler][issue-stretch-clip]") {
    Fixture f;
    // Sharp full-scale clicks + a decaying body — hot, transient-rich material.
    std::vector<float> loop(48000, 0.0f);
    for (int b = 0; b < 8; ++b) {
        const int o = b * 6000;
        loop[o] = 0.99f; loop[o + 1] = -0.95f; loop[o + 2] = 0.9f;
        for (int i = 3; i < 3000 && o + i < 48000; ++i)
            loop[o + i] += 0.6f * std::exp(-6.0 * i / 3000.0) *
                           std::sin(2.0 * 3.14159265358979323846 * 110.0 * i / 48000.0);
    }
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.proc->set_loop_bpm_for_test(120.0);

    std::vector<float> l(512), r(512);
    const double slow = 60.0;                                  // R = 120/60 = 2.0, long slices
    f.proc->render_now(slow);
    { midi::MidiBuffer m; process_midi(*f.proc, slow, m, l, r); }
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 6; }));

    // Trigger every slice simultaneously at full velocity -> max overlapping voices.
    { midi::MidiBuffer m;
      for (int n = 0; n < 8; ++n) m.add(midi::MidiEvent::note_on(0, kRoot + n, 127));
      process_midi(*f.proc, slow, m, l, r); }

    float peak = 0.0f;
    for (int b = 0; b < 16; ++b) {
        midi::MidiBuffer m; process_midi(*f.proc, slow, m, l, r);
        for (float v : l) peak = std::max(peak, std::fabs(v));
        for (float v : r) peak = std::max(peak, std::fabs(v));
    }
    INFO("output peak with 8 overlapping voices: " << peak);
    CHECK(peak <= 1.0f);   // bounded — no digital clip on the device
    CHECK(peak > 0.9f);    // and the limiter engaged (loud), not silenced
}

// Changing the slice count (re-slice on the SAME sample) must preserve the user's
// tempo. The footer TEMPO poller re-detects BPM only when load_generation advances;
// a re-slice bumps raw_generation (so the waveform/slice display repaints) but must
// NOT bump load_generation (so the tempo override survives). A genuinely new load
// bumps BOTH (tempo legitimately re-detects). This locks the counter split.
TEST_CASE("re-slice preserves tempo; new load re-detects it", "[tempo-sampler][issue-slice-tempo]") {
    Fixture f;
    auto loop = percussive_loop(48000, 6);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    const std::uint64_t load0 = f.proc->load_generation();
    const std::uint64_t raw0  = f.proc->raw_generation();

    // Re-slice the SAME sample (more onsets) — the display generation must advance
    // while the tempo-poller generation stays put.
    f.store.set_value(kOnsetSens, 0.85f);
    f.proc->request_reanalyze();
    REQUIRE(wait_for([&] { return f.proc->raw_generation() != raw0; }));
    CHECK(f.proc->load_generation() == load0);   // tempo poller does NOT fire -> tempo preserved

    // A genuinely new sample must advance load_generation so the poller re-detects BPM.
    auto loop2 = percussive_loop(24000, 4);
    const float* ch2[1] = {loop2.data()};
    REQUIRE(f.proc->load_loop(ch2, 1, 24000, 48000.0));
    CHECK(f.proc->load_generation() != load0);
}

TEST_CASE("QWERTY note-off releases the original slice after root changes",
          "[tempo-sampler][issue-home-row]") {
    Fixture f;
    auto buf = sine(220.0, 48000.0, 48000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    f.store.set_value(kTempoLoop, 1.0f);      // loop so a missed note-off is audible
    f.store.set_value(kTempoRelease, 5.0f);   // fast decay once the right voice releases
    f.store.set_value(kTempoSustain, 100.0f);
    f.proc->set_loop_bpm_for_test(120.0);
    std::vector<float> l(512), r(512);
    { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    int root_note = kRoot;
    SamplerEditorRoot root;
    root.hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    root.current_root_note = [&] { return root_note; };
    root.keyboard_window_visible = [] { return true; };
    root.typing.on_note_on  = [&](int n, float v) { f.proc->keyboard_play_on(n, v); };
    root.typing.on_note_off = [&](int n) { f.proc->keyboard_play_off(n); };
    auto key = [](view::KeyCode k, bool down) {
        view::KeyEvent e; e.key = k; e.is_down = down; return e;
    };

    REQUIRE(root.on_key_event(key(view::KeyCode::a, true)));  // physical note 60 -> trigger 60
    double held = 0.0;
    for (int b = 0; b < 8; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); held += block_energy(l); }
    REQUIRE(held > 1e-3);

    // Note-off must release the trigger note captured on key-down, not recompute it
    // from the (now-changed) ROOT. The held set records each key's trigger at press
    // time so the release always matches the voice that was started.
    root_note = 72;
    f.store.set_value(kRootNote, 72.0f);
    REQUIRE(root.on_key_event(key(view::KeyCode::a, false)));
    CHECK(f.proc->held_notes_for_test().empty());

    for (int b = 0; b < 24; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
    double tail = 0.0;
    for (int b = 0; b < 8; ++b) { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); tail += block_energy(l); }
    CHECK(tail < held * 0.05);
}

TEST_CASE("musical typing maps QWERTY keys to slice notes (root-based)", "[tempo-sampler]") {
    SamplerEditorRoot root;
    root.hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    root.current_root_note = [] { return kRoot; };  // base 'a' = root note
    // Musical typing only plays while the keyboard window is shown — simulate the
    // window being on-screen so this exercises the QWERTY->note MAPPING itself.
    root.keyboard_window_visible = [] { return true; };
    std::vector<std::pair<int, bool>> notes;
    root.typing.on_note_on = [&](int n, float) { notes.emplace_back(n, true); };
    root.typing.on_note_off = [&](int n) { notes.emplace_back(n, false); };
    auto key = [](view::KeyCode k, bool down, bool rep = false) {
        view::KeyEvent e; e.key = k; e.is_down = down; e.is_repeat = rep; return e;
    };
    REQUIRE(root.on_key_event(key(view::KeyCode::a, true)));         // root + 0
    REQUIRE(root.on_key_event(key(view::KeyCode::a, true, true)));   // auto-repeat ignored
    REQUIRE(root.on_key_event(key(view::KeyCode::a, false)));        // note off root
    REQUIRE(root.on_key_event(key(view::KeyCode::s, true)));         // root + 2
    REQUIRE_FALSE(root.on_key_event(key(view::KeyCode::num1, true)));  // unmapped -> host

    REQUIRE(notes.size() == 3);
    CHECK(notes[0] == std::make_pair(kRoot, true));      // 'a' down  (slice 0 = root)
    CHECK(notes[1] == std::make_pair(kRoot, false));     // 'a' up
    CHECK(notes[2] == std::make_pair(kRoot + 2, true));  // 's' down  (slice 2)
}

namespace {
WaveformDropView* find_waveform(view::View* v) {
    if (auto* w = dynamic_cast<WaveformDropView*>(v)) return w;
    for (std::size_t i = 0; i < v->child_count(); ++i)
        if (auto* w = find_waveform(v->child_at(i))) return w;
    return nullptr;
}

view::TextButton* find_button(view::View* v, const std::string& label) {
    if (auto* b = dynamic_cast<view::TextButton*>(v); b && b->label() == label) return b;
    for (std::size_t i = 0; i < v->child_count(); ++i)
        if (auto* b = find_button(v->child_at(i), label)) return b;
    return nullptr;
}

view::ToggleButton* find_toggle(view::View* v, const std::string& label) {
    if (auto* t = dynamic_cast<view::ToggleButton*>(v); t && t->label() == label) return t;
    for (std::size_t i = 0; i < v->child_count(); ++i)
        if (auto* t = find_toggle(v->child_at(i), label)) return t;
    return nullptr;
}

// Find a ComboBox by its first item (e.g. the MODE dropdown's "1-Shot").
view::ComboBox* find_combo(view::View* v, const std::string& first_item) {
    if (auto* c = dynamic_cast<view::ComboBox*>(v);
        c && !c->items().empty() && c->items().front() == first_item)
        return c;
    for (std::size_t i = 0; i < v->child_count(); ++i)
        if (auto* c = find_combo(v->child_at(i), first_item)) return c;
    return nullptr;
}

// RAII fake FileDialog backend so the Open-button path is testable headlessly
// (no native panel). open_file() returns the given path.
struct FakeFileDialog {
    explicit FakeFileDialog(std::optional<std::string> result) {
        pulp::platform::FileDialog::Backend b;
        b.open_file = [result](const std::string&,
                               const std::vector<pulp::platform::FileFilter>&,
                               const std::string&) { return result; };
        pulp::platform::FileDialog::set_backend(std::move(b));
    }
    ~FakeFileDialog() { pulp::platform::FileDialog::clear_backend(); }
};
} // namespace

// THE regression guard for "looks done but doesn't play": drive the REAL user
// gesture through the REAL wired editor (not just the processor API) and assert
// audio comes out. If create_view() forgets to wire on_play_slice, or the click
// no longer maps to a slice, this fails.
TEST_CASE("end-to-end: clicking a slice in the editor produces audio", "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto editor = f.proc->create_view();  // builds + wires the real editor
    REQUIRE(editor);
    // One layout+paint pass so the WaveformDropView polls the processor and
    // populates its slice map (the same path the live host drives each frame).
    (void)view::render_to_png(*editor, 760, 372, 1.0f, view::ScreenshotBackend::skia);

    WaveformDropView* wf = find_waveform(editor.get());
    REQUIRE(wf != nullptr);
    const auto b = wf->local_bounds();  // on_mouse_event expects local coords
    REQUIRE(b.width > 0);

    // Click near the left edge → slice 0; the editor routes click -> play_slice.
    view::MouseEvent down;
    down.button = view::MouseButton::left;
    down.is_down = true;
    down.position = {b.x + b.width * 0.05f, b.y + b.height * 0.5f};
    wf->on_mouse_event(down);

    std::vector<float> l(512), r(512);
    process_block(*f.proc, 120.0, false, 0, l, r);
    REQUIRE(wait_for([&] { return f.proc->published_frames() > 0; }));
    double energy = 0.0;
    for (int blk = 0; blk < 8; ++blk) {
        process_block(*f.proc, 120.0, false, 0, l, r);
        for (int i = 0; i < 512; ++i) energy += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
    }
    CHECK(energy > 1e-6);  // the wired editor actually triggered the sample
}

// "fix the standalone to tap to open file": once a sample is loaded, a waveform
// tap auditions a slice, so loading/replacing must work through the always-
// visible Open button. Drive the REAL button -> do_browse -> FileDialog -> load.
TEST_CASE("Open button loads a sample even when one is already loaded", "[tempo-sampler]") {
    const std::string path = "/tmp/pulp_tempo_open_button.wav";
    REQUIRE(write_wav(path, percussive_loop(48000, 4), 48000));

    Fixture f;
    // Start WITH a sample loaded so a waveform tap would audition a slice
    // (the exact state where tap-to-open used to be impossible).
    auto first = percussive_loop(48000, 2);
    const float* ch[1] = {first.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto editor = f.proc->create_view();
    REQUIRE(editor);
    (void)view::render_to_png(*editor, 760, 372, 1.0f, view::ScreenshotBackend::skia);

    view::TextButton* btn = find_button(editor.get(), "Open…");  // "Open…"
    REQUIRE(btn != nullptr);

    {
        FakeFileDialog fake(path);  // RAII: the picker returns our WAV
        const auto b = btn->local_bounds();
        btn->on_mouse_down({b.width * 0.5f, b.height * 0.5f});  // -> on_click -> do_browse
    }
    // The Open button loaded + sliced the new sample off-thread.
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= 2; }));
}

// PLAYBACK / LOOP / DIRECTION are three SEPARATE dropdowns (Logic/PlunderTube
// 3-axis model): Playback = Classic/Slice/One-Shot, Loop = None/On, Direction =
// Forward/Reverse/Ping-Pong (Detail page). Each drives its own param.
TEST_CASE("Direction + Loop dropdowns drive their params", "[tempo-sampler][modes-ui]") {
    Fixture f;
    auto buf = sine(440.0, 48000.0, 24000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 24000, 48000.0));
    auto editor = f.proc->create_view();
    REQUIRE(editor);
    auto* dir = find_combo(editor.get(), "Forward");   // Direction: Forward/Reverse
    auto* loop = find_combo(editor.get(), "None");     // Loop: None/Forward/Reverse/Ping-Pong
    REQUIRE(dir != nullptr);
    REQUIRE(loop != nullptr);

    // Defaults: Direction Forward, Loop None.
    CHECK(dir->selected() == 0);
    CHECK(loop->selected() == 0);

    dir->set_selected(1); CHECK(f.store.get_value(kDirection) == 1.0f);          // Reverse
    dir->set_selected(0); CHECK(f.store.get_value(kDirection) == 0.0f);          // Forward
    loop->set_selected(1); CHECK(f.store.get_value(kTempoLoop) >= 0.5f);         // Forward loop
    CHECK(f.store.get_value(kLoopMode) == 0.0f);
    loop->set_selected(2); CHECK(f.store.get_value(kLoopMode) == 1.0f);          // Reverse loop
    loop->set_selected(3); CHECK(f.store.get_value(kLoopMode) == 2.0f);          // Ping-Pong loop
    loop->set_selected(0); CHECK(f.store.get_value(kTempoLoop) < 0.5f);          // None

    // Loop dropdown reflects external param changes (preset load).
    f.store.set_value(kTempoLoop, 1.0f); f.store.set_value(kLoopMode, 1.0f);
    CHECK(loop->selected() == 2);   // Reverse
}

// TEMPO field: tap-to-type an exact BPM. The footer readout IS a numeric TextEditor;
// on_return parses the leading number (ignoring the live " BPM" suffix), clamps to
// the engine's [20, 400] range, and engages it as the manual target (auto-unlinking).
TEST_CASE("TEMPO field types an exact BPM on Return (parse + clamp + unlink)",
          "[tempo-sampler][tempo-ui]") {
    Fixture f;
    auto buf = sine(440.0, 48000.0, 24000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 24000, 48000.0));
    auto editor = f.proc->create_view();
    REQUIRE(editor);
    auto* root = dynamic_cast<SamplerEditorRoot*>(editor.get());
    REQUIRE(root != nullptr);
    auto* fld = root->tempo_edit_;
    REQUIRE(fld != nullptr);

    // The affordance: a numeric field that highlights its value when tapped.
    CHECK(fld->numeric_only);
    CHECK(fld->select_on_focus);

    // Starts LINKED (no manual override); typing a value engages it as the target.
    REQUIRE_FALSE(f.proc->tempo_override_active());
    fld->on_return("128");
    CHECK(f.proc->tempo_override_active());
    CHECK(std::lround(f.proc->effective_bpm()) == 128);

    // One decimal is honored, and the readout's trailing " BPM" is ignored.
    fld->on_return("142.5");
    CHECK(std::abs(f.proc->effective_bpm() - 142.5) < 0.01);
    fld->on_return("96.0 BPM");
    CHECK(std::abs(f.proc->effective_bpm() - 96.0) < 0.01);

    // Out-of-range values clamp to the engine range, not rejected.
    fld->on_return("9999");
    CHECK(std::lround(f.proc->effective_bpm()) == 400);
    fld->on_return("1");
    CHECK(std::lround(f.proc->effective_bpm()) == 20);

    // Escape and empty/garbage input leave the engaged tempo untouched.
    f.proc->set_target_bpm(150.0);
    fld->on_escape();
    CHECK(std::lround(f.proc->effective_bpm()) == 150);
    fld->on_return("");
    CHECK(std::lround(f.proc->effective_bpm()) == 150);
}

// REVERSE direction: a whole-loop region played in Reverse reads end->start, so a
// STRONG impulse near the loop start and a WEAK impulse near the end come out in
// the OPPOSITE order vs Forward. A/B the first audible peak to prove direction.
TEST_CASE("Reverse mode plays the loop backwards", "[tempo-sampler][loop-mode]") {
    auto first_peak = [](int dir) -> float {
        Fixture f;
        const double sr = 48000.0, bpm = 120.0;
        const long frames = 96000;                          // 1 bar @120 (identity stretch)
        std::vector<float> buf(static_cast<size_t>(frames), 0.0f);
        buf[2000] = 1.0f;  buf[2001] = -0.9f;               // STRONG impulse near the start
        buf[static_cast<size_t>(frames - 2000)] = 0.5f;
        buf[static_cast<size_t>(frames - 1999)] = -0.45f;   // WEAK impulse near the end
        f.store.set_value(kOnsetSens, 0.0f);                // whole loop (1 region): set BEFORE load
        const float* ch[1] = {buf.data()};
        REQUIRE(f.proc->load_loop(ch, 1, frames, sr));
        REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
        REQUIRE(wait_for([&] { return f.proc->num_slices() == 1; }));
        f.proc->set_loop_bpm_for_test(bpm);
        // Direction (first-pass entry) is what plays the region backward. Loop=None
        // so it plays ONCE in the Direction — the FIRST peak proves the entry dir.
        f.store.set_value(kDirection, static_cast<float>(dir));  // 0 Forward, 1 Reverse
        std::vector<float> l(512), r(512);
        { midi::MidiBuffer m; process_midi(*f.proc, bpm, m, l, r); }
        REQUIRE(wait_for([&] { return std::abs(f.proc->published_frames() - frames) <= 8; }));
        std::vector<float> out; out.reserve(120000);
        { midi::MidiBuffer m; m.add(midi::MidiEvent::note_on(0, kRoot, 110));
          process_midi(*f.proc, bpm, m, l, r); out.insert(out.end(), l.begin(), l.end()); }
        for (int blk = 0; blk < 150; ++blk) {               // < 1 loop pass, so only the FIRST impulse
            midi::MidiBuffer m; process_midi(*f.proc, bpm, m, l, r);
            out.insert(out.end(), l.begin(), l.end());
        }
        for (long i = 1; i + 1 < static_cast<long>(out.size()); ++i) {
            const float a = std::abs(out[static_cast<size_t>(i)]);
            if (a > 0.2f && a >= std::abs(out[static_cast<size_t>(i - 1)]) &&
                a > std::abs(out[static_cast<size_t>(i + 1)])) return a;
        }
        return 0.0f;
    };
    const float fwd_first = first_peak(0);   // Forward: STRONG start impulse first => ~1.0
    const float rev_first = first_peak(1);   // Reverse: WEAK end impulse first    => ~0.5
    INFO("forward first peak=" << fwd_first << "  reverse first peak=" << rev_first);
    CHECK(fwd_first > 0.7f);
    CHECK(rev_first < 0.7f);
    CHECK(rev_first > 0.2f);                 // it IS the weak impulse, not silence
}

TEST_CASE("waveform scroll zooms in/out and pans", "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto editor = f.proc->create_view();
    REQUIRE(editor);
    (void)view::render_to_png(*editor, 760, 372, 1.0f, view::ScreenshotBackend::skia);
    WaveformDropView* wf = find_waveform(editor.get());
    REQUIRE(wf != nullptr);
    const auto b = wf->local_bounds();
    const int full = wf->visible_length();
    REQUIRE(full > 0);  // zoom-to-fit on load

    auto wheel = [&](float dy, float dx, float fx) {
        view::MouseEvent e;
        e.is_wheel = true;
        e.scroll_delta_y = dy;
        e.scroll_delta_x = dx;
        e.position = {b.x + b.width * fx, b.y + b.height * 0.5f};
        wf->on_mouse_event(e);
    };

    wheel(3.0f, 0.0f, 0.5f);                  // scroll up at center -> zoom IN
    const int zoomed = wf->visible_length();
    REQUIRE(zoomed < full);

    wheel(-6.0f, 0.0f, 0.5f);                 // scroll down -> zoom OUT (toward full)
    REQUIRE(wf->visible_length() > zoomed);

    wheel(3.0f, 0.0f, 0.5f);                  // zoom back in, then pan
    const int start_before = wf->visible_start();
    wheel(0.0f, 40.0f, 0.5f);                 // horizontal -> pan
    REQUIRE(wf->visible_start() != start_before);
}

// Standalone musical typing: the host calls root->on_global_key per keystroke.
// Prove the wired editor turns a typed 'a' into audio (the real standalone path).
TEST_CASE("standalone musical typing: on_global_key plays a slice", "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto editor = f.proc->create_view();
    REQUIRE(editor);
    auto* root = dynamic_cast<SamplerEditorRoot*>(editor.get());
    if (root) root->hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    REQUIRE(root != nullptr);
    REQUIRE(root->on_global_key);  // the hook the standalone host fires per key
    // Typing only plays while the keyboard window is on-screen — simulate it open
    // (a headless test cannot create the secondary GPU window).
    root->keyboard_window_visible = [] { return true; };

    view::KeyEvent a; a.key = view::KeyCode::a; a.is_down = true;
    REQUIRE(root->on_global_key(a));  // 'a' -> note (slice 0), consumed

    std::vector<float> l(512), r(512);
    process_block(*f.proc, 120.0, false, 0, l, r);
    REQUIRE(wait_for([&] { return f.proc->published_frames() > 0; }));
    double energy = 0.0;
    for (int blk = 0; blk < 8; ++blk) {
        process_block(*f.proc, 120.0, false, 0, l, r);
        for (int i = 0; i < 512; ++i) energy += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
    }
    CHECK(energy > 1e-6);  // typing produced audio through the wired editor
}

// Fix: musical typing only plays while the keyboard window is on-screen. With
// the keyboard HIDDEN, a typed key must NOT trigger a slice (returns false, no
// audio); with it SHOWN, the same key plays. Drive the REAL wired editor through
// on_global_key (the standalone host's per-key hook) and inject the visibility
// gate (a headless test cannot open the secondary GPU window). The key-UP must
// always pass through so a note held across a hide is released (no stuck note).
TEST_CASE("musical typing is gated on the keyboard window being visible",
          "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto editor = f.proc->create_view();
    auto* root = dynamic_cast<SamplerEditorRoot*>(editor.get());
    if (root) root->hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    REQUIRE(root != nullptr);
    REQUIRE(root->on_global_key);

    bool kb_visible = false;
    root->keyboard_window_visible = [&] { return kb_visible; };

    auto type_a_energy = [&] {
        view::KeyEvent down; down.key = view::KeyCode::a; down.is_down = true;
        const bool consumed = root->on_global_key(down);  // note held across the measure
        std::vector<float> l(512), r(512);
        process_block(*f.proc, 120.0, false, 0, l, r);
        wait_for([&] { return f.proc->published_frames() > 0; });
        double e = 0.0;
        for (int blk = 0; blk < 8; ++blk) {
            process_block(*f.proc, 120.0, false, 0, l, r);
            for (int i = 0; i < 512; ++i) e += l[(size_t)i] * l[(size_t)i];
        }
        // Key-UP always passes through (even gated) so a held note releases.
        view::KeyEvent up; up.key = view::KeyCode::a; up.is_down = false;
        root->on_global_key(up);
        return std::make_pair(consumed, e);
    };

    // Drain anything ringing, then type with the keyboard HIDDEN: silent.
    { std::vector<float> l(512), r(512);
      for (int b = 0; b < 80; ++b) process_block(*f.proc, 120.0, false, 0, l, r); }
    kb_visible = false;
    auto [hidden_consumed, hidden_e] = type_a_energy();
    CHECK_FALSE(hidden_consumed);   // key-down not consumed when hidden
    CHECK(hidden_e < 1e-9);         // no slice played

    // Drain again, then type with the keyboard SHOWN: plays.
    { std::vector<float> l(512), r(512);
      for (int b = 0; b < 80; ++b) process_block(*f.proc, 120.0, false, 0, l, r); }
    kb_visible = true;
    auto [shown_consumed, shown_e] = type_a_energy();
    CHECK(shown_consumed);          // key-down consumed when shown
    CHECK(shown_e > 1e-6);          // slice played
}

// Fix: ⌘K toggles the keyboard window regardless of its current visibility
// (hidden -> show, shown -> hide). The gate must NOT suppress ⌘K the way it
// suppresses plain typing. Intercept on_toggle_keyboard so no window opens.
TEST_CASE("Cmd+K toggles the keyboard window in both visibility states",
          "[tempo-sampler]") {
    Fixture f;
    auto editor = f.proc->create_view();
    auto* root = dynamic_cast<SamplerEditorRoot*>(editor.get());
    if (root) root->hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    REQUIRE(root != nullptr);
    int toggles = 0;
    root->on_toggle_keyboard = [&] { ++toggles; };

    view::KeyEvent k; k.key = view::KeyCode::k; k.is_down = true; k.modifiers = view::kModCmd;

    root->keyboard_window_visible = [] { return false; };  // hidden
    REQUIRE(root->on_global_key(k));   // ⌘K still toggles -> show
    REQUIRE(toggles == 1);

    root->keyboard_window_visible = [] { return true; };   // shown
    REQUIRE(root->on_global_key(k));   // ⌘K still toggles -> hide
    REQUIRE(toggles == 2);
}

// The SDK MusicalTypingKeyboard primitive (hosted in its own ⌘K window) feeds
// the SAME lock-free UI->audio path host MIDI / slice clicks use. Wire it exactly
// as toggle_keyboard_window() does and assert a note from on_note_on(rootMidi)
// reaches the sampler and produces audio — no pixel coords, no window needed.
TEST_CASE("musical-typing keyboard note routing reaches the sampler",
          "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    std::vector<float> l(512), r(512);
    process_block(*f.proc, 120.0, false, 0, l, r);  // publish at host tempo
    REQUIRE(wait_for([&] { return f.proc->published_frames() > 0; }));

    // base = ROOT so the root note maps to slice 0 (idx = note - root).
    const int root_note = static_cast<int>(f.store.get_value(kRootNote));
    auto kb = std::make_unique<view::MusicalTypingKeyboard>();
    kb->on_note_on  = [&](int note, float vel) { f.proc->sampler_note_on(note, vel); };
    kb->on_note_off = [&](int note) { f.proc->sampler_note_off(note); };
    kb->controller().set_base_note(root_note);

    kb->on_note_on(root_note, 0.8f);  // the keyboard emits the root note
    double energy = 0.0;
    for (int blk = 0; blk < 8; ++blk) {
        process_block(*f.proc, 120.0, false, 0, l, r);
        for (int i = 0; i < 512; ++i) energy += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
        for (float v : l) REQUIRE(std::isfinite(v));
    }
    kb->on_note_off(root_note);
    CHECK(energy > 1e-6);  // keyboard note reached the sampler and produced audio
}

// ⌘K (Ctrl+K on Win/Linux) toggles the MusicalTypingKeyboard's OWN window. The
// standalone host routes Cmd-chords via performKeyEquivalent: to on_global_key.
// Intercept on_toggle_keyboard so the test asserts the route WITHOUT opening a
// real GPU window (the live path is toggle_keyboard_window()).
TEST_CASE("Cmd+K routes to the keyboard-window toggle", "[tempo-sampler]") {
    Fixture f;
    auto editor = f.proc->create_view();
    REQUIRE(editor);
    auto* root = dynamic_cast<SamplerEditorRoot*>(editor.get());
    if (root) root->hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    REQUIRE(root != nullptr);
    REQUIRE(root->on_global_key);  // the hook the standalone host fires per key
    int toggles = 0;
    root->on_toggle_keyboard = [&] { ++toggles; };

    view::KeyEvent k; k.key = view::KeyCode::k; k.is_down = true; k.modifiers = view::kModCmd;
    REQUIRE(root->on_global_key(k));  // ⌘K consumed -> toggle
    REQUIRE(toggles == 1);
    REQUIRE(root->on_global_key(k));  // ⌘K again -> toggle
    REQUIRE(toggles == 2);
}

// Toolbar "Keyboard" button hits the SAME toggle as ⌘K (no key chord needed).
// Drive the REAL button; intercept on_toggle_keyboard to avoid opening a window.
TEST_CASE("toolbar Keyboard button routes to the keyboard-window toggle",
          "[tempo-sampler]") {
    Fixture f;
    auto editor = f.proc->create_view();
    auto* root = dynamic_cast<SamplerEditorRoot*>(editor.get());
    if (root) root->hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    REQUIRE(root != nullptr);
    int toggles = 0;
    root->on_toggle_keyboard = [&] { ++toggles; };

    // One layout+paint pass so the button has clickable geometry.
    (void)view::render_to_png(*editor, 760, 372, 1.0f, view::ScreenshotBackend::skia);
    view::TextButton* kbd_btn = find_button(editor.get(), "Keyboard");
    REQUIRE(kbd_btn != nullptr);

    const auto b = kbd_btn->local_bounds();
    kbd_btn->on_mouse_down({b.width * 0.5f, b.height * 0.5f});  // -> on_click -> toggle
    REQUIRE(toggles == 1);  // button hit the same toggle as ⌘K
}

// The "Settings" button is hidden in a plain (DAW) editor and revealed only when
// the editor is hosted in the standalone settings chrome (a TabPanel ancestor
// with an Audio/MIDI Settings tab). Clicking it switches the chrome to that tab.
// This mirrors what make_standalone_editor_chrome() builds when
// show_settings_tab=true, and what Processor::on_view_opened() drives live.
TEST_CASE("Settings button reveals + opens the Audio/MIDI panel in the chrome",
          "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto editor = f.proc->create_view();
    auto* root = dynamic_cast<SamplerEditorRoot*>(editor.get());
    if (root) root->hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    REQUIRE(root != nullptr);
    REQUIRE(root->settings_button != nullptr);
    REQUIRE_FALSE(root->settings_button->visible());  // hidden outside the chrome

    // Build the standalone-style chrome: tab-bar-less TabPanel, editor = tab 0,
    // an Audio/MIDI Settings panel = tab 1.
    auto chrome = std::make_unique<view::TabPanel>();
    chrome->set_bounds({0, 0, 760, 372});
    chrome->set_show_tab_bar(false);
    chrome->add_tab("Editor", std::move(editor));
    chrome->add_tab("Settings", std::make_unique<format::SettingsPanel>());

    // Attach-time gating — Processor::on_view_opened() calls this live once the
    // editor is parented into the chrome.
    root->update_standalone_chrome_affordances();
    REQUIRE(root->settings_button->visible());  // now reachable -> revealed

    REQUIRE(chrome->active_tab() == 0);              // editor shown first
    REQUIRE(root->settings_button->on_click);        // wired
    root->settings_button->on_click();               // -> open_standalone_settings
    REQUIRE(chrome->active_tab() == 1);              // switched to the Settings tab

    // Visual proof: render the chrome on its Settings tab so the Audio/MIDI panel
    // (device selectors, sample rate, buffer, meters, Done) is what's captured.
    auto png = view::render_to_png(*chrome, 760, 372, 1.0f, view::ScreenshotBackend::skia);
    REQUIRE(png.size() > 1000);
    std::ofstream("/tmp/sampler_settings_panel.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()),
               static_cast<std::streamsize>(png.size()));
}

// Item B visual proof: rendered at the SettingsPanel's natural height (the size
// the standalone host grows the window to on the Settings tab), the device
// dropdowns and meters lay out at full size instead of being squished into the
// editor's 372px. Writes both the squished (372) and full-height captures so the
// fix is visible side by side.
TEST_CASE("settings panel renders un-squished at its preferred height",
          "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto editor = f.proc->create_view();
    auto chrome = std::make_unique<view::TabPanel>();
    const int settings_h = format::SettingsPanel::preferred_height();
    chrome->set_bounds({0, 0, 760, static_cast<float>(settings_h)});
    chrome->set_show_tab_bar(false);
    chrome->add_tab("Editor", std::move(editor));
    chrome->add_tab("Settings", std::make_unique<format::SettingsPanel>());
    chrome->set_active_tab(1);
    REQUIRE(chrome->active_tab() == 1);

    auto full = view::render_to_png(*chrome, 760, static_cast<uint32_t>(settings_h),
                                    1.0f, view::ScreenshotBackend::skia);
    REQUIRE(full.size() > 1000);
    std::ofstream("/tmp/sampler_settings_full.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(full.data()),
               static_cast<std::streamsize>(full.size()));

    // For contrast, the squished render at the editor's height (the bug).
    auto squished = view::render_to_png(*chrome, 760, 372, 1.0f,
                                        view::ScreenshotBackend::skia);
    REQUIRE(squished.size() > 1000);
    std::ofstream("/tmp/sampler_settings_squished.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(squished.data()),
               static_cast<std::streamsize>(squished.size()));
}

// ── Single-input keyboard: held-note lighting + visibility gate ───────────────
// Original spec: the on-screen keyboard is DISPLAY-ONLY for QWERTY; the editor's
// typing is the single source that BOTH plays slices AND lights the keyboard via
// set_active_notes. While the keyboard is HIDDEN, typing does nothing (no note,
// no light). While SHOWN, a typed key is added to the held set and the matching
// key lights; releasing it removes it and unlights. Drive the REAL wired editor
// through on_global_key and observe the held set + the injected keyboard's lit
// keys (a headless test can't open the secondary GPU window).
namespace {
// element_value of the (typing-frame) key whose relative semitone == `semitone`.
bool typing_key_lit(view::MusicalTypingKeyboard& kb, int semitone) {
    // The SDK paints a momentary key lit only when value > 0.5 (default is 0.5,
    // i.e. neither lit nor explicitly cleared); apply_held_notes sets 1.0 / 0.0.
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == view::DesignFrameElement::Kind::momentary &&
            kb.element_note(i) == semitone)
            return kb.element_value(i) > 0.5f;
    return false;
}
}  // namespace

TEST_CASE("single-input: typed key lights the keyboard only while visible",
          "[tempo-sampler]") {
    Fixture f;
    auto loop = percussive_loop(48000, 4);
    const float* ch[1] = {loop.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
    REQUIRE(wait_for([&] { return f.proc->has_sample(); }));

    auto editor = f.proc->create_view();
    auto* root = dynamic_cast<SamplerEditorRoot*>(editor.get());
    if (root) root->hosted_in_standalone_ = true;  // typing+keyboard are standalone-only
    REQUIRE(root != nullptr);

    // Inject the SDK keyboard (display-only) so the held-note → set_active_notes
    // wiring is observable; the live path creates it in toggle_keyboard_window().
    auto kb = std::make_unique<view::MusicalTypingKeyboard>();
    kb->set_input_capture(false);
    const int root_note = static_cast<int>(f.store.get_value(kRootNote));
    kb->controller().set_base_note(root_note);
    auto* kb_ptr = kb.get();
    f.proc->set_keyboard_for_test(std::move(kb));

    bool kb_visible = false;
    root->keyboard_window_visible = [&] { return kb_visible; };
    auto type = [&](view::KeyCode k, bool down) {
        view::KeyEvent e; e.key = k; e.is_down = down; root->on_global_key(e);
    };

    // HIDDEN: a typed key adds nothing to the held set and lights nothing.
    kb_visible = false;
    type(view::KeyCode::a, true);
    CHECK(f.proc->held_notes_for_test().empty());
    CHECK_FALSE(typing_key_lit(*kb_ptr, 0));
    type(view::KeyCode::a, false);  // key-up always passes (no stuck note)

    // SHOWN: 'a' (slice 0 = root) is added and the matching key lights.
    kb_visible = true;
    type(view::KeyCode::a, true);
    REQUIRE(f.proc->held_notes_for_test().size() == 1);
    CHECK(f.proc->held_notes_for_test()[0] == root_note);
    CHECK(typing_key_lit(*kb_ptr, 0));         // semitone 0 key lit

    // A second held key ('s' = semitone 2) joins the set and lights live.
    type(view::KeyCode::s, true);
    REQUIRE(f.proc->held_notes_for_test().size() == 2);
    CHECK(typing_key_lit(*kb_ptr, 0));
    CHECK(typing_key_lit(*kb_ptr, 2));

    // Releasing 'a' removes it and unlights it; 's' stays lit.
    type(view::KeyCode::a, false);
    CHECK(f.proc->held_notes_for_test().size() == 1);
    CHECK_FALSE(typing_key_lit(*kb_ptr, 0));
    CHECK(typing_key_lit(*kb_ptr, 2));

    type(view::KeyCode::s, false);
    CHECK(f.proc->held_notes_for_test().empty());
    CHECK_FALSE(typing_key_lit(*kb_ptr, 2));
}

// ── Pixel proofs (Fix 2 + Fix 3): the SDK renders each frame faithfully at its
// exact panel dims, which is the live window's design viewport (the integration
// sizes the keyboard window to panel_width()×panel_height() and pins the design
// viewport to the same). So a render at panel dims IS a render at the live
// viewport, at unit scale — proving these are integration/host concerns, not SDK.
namespace {
struct Px { uint8_t r = 0, g = 0, b = 0, a = 0; };
Px pixel_at(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, int x, int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h)) return {};
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}
bool is_teal(Px p) {  // accent teal (22,218,194) over any baked button
    return p.g > 90 && p.b > 90 && p.g > p.r + 40 && p.b > p.r + 25;
}
bool is_light(Px p) { return p.r > 175 && p.g > 175 && p.b > 175; }  // white piano key
// Fraction of teal pixels inside a panel-coord rect (inset to dodge rounded
// corners). At unit scale, panel coords == pixel coords.
double teal_fraction(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h,
                     float x, float y, float rw, float rh) {
    const int inset = 7;
    int n = 0, t = 0;
    for (int yy = static_cast<int>(y) + inset; yy < static_cast<int>(y + rh) - inset; yy += 2)
        for (int xx = static_cast<int>(x) + inset; xx < static_cast<int>(x + rw) - inset; xx += 2) {
            ++n;
            if (is_teal(pixel_at(rgba, w, h, xx, yy))) ++t;
        }
    return n ? static_cast<double>(t) / n : 0.0;
}
void write_png(view::View& v, uint32_t w, uint32_t h, const char* path) {
    auto png = view::render_to_png(v, w, h, 1.0f, view::ScreenshotBackend::skia);
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()),
               static_cast<std::streamsize>(png.size()));
}
}  // namespace

// Regression: dragging an .m4a (or .aac/.alac/.caf) onto the editor used to be
// rejected by a hardcoded extension allow-list even though the macOS CoreAudio
// reader decodes them. The allow-list now derives from FormatRegistry, so the
// drop gate matches the decoders. .wav is accepted on every platform; the
// compressed containers are accepted where a reader for them is registered.
TEST_CASE("WaveformDropView accepts registry-decodable drops (incl. m4a on macOS)",
          "[tempo-sampler][drop]") {
    WaveformDropView v;
    auto accepts = [&](const char* path) {
        view::DropData d;
        d.type = view::DropData::Type::files;
        d.file_paths = {path};
        return v.accept_drag(d, {0.0f, 0.0f});
    };
    REQUIRE(accepts("/tmp/loop.wav"));            // built-in reader, all platforms
    REQUIRE_FALSE(accepts("/tmp/notes.txt"));     // not an audio format
    const bool m4a = accepts("/tmp/song.m4a");
    const auto exts = audio::FormatRegistry::instance().supported_read_extensions();
    const bool has_m4a = std::find(exts.begin(), exts.end(), ".m4a") != exts.end();
    REQUIRE(m4a == has_m4a);                      // accepted iff a reader is registered
#ifdef __APPLE__
    REQUIRE(m4a);                                 // CoreAudio reader decodes m4a on macOS
#endif
}

// Fix 2: with a non-"off" modulation latched, the teal highlight FULLY covers the
// modulation key cell (no black inset) when rendered at the keyboard's panel dims
// — the exact viewport the live window uses. Proven by sampling the lit cell
// (mostly teal) vs an unlit neighbour (not teal — the highlight doesn't overflow).
TEST_CASE("Fix 2: modulation-key highlight fully covers its cell at panel viewport",
          "[tempo-sampler]") {
    view::MusicalTypingKeyboard kb;
    const float w = kb.panel_width(), h = kb.panel_height();   // typing: 732×266
    kb.set_bounds({0, 0, w, h});

    // Latch modulation step 3 (key '6'): mod_3 lights, default mod_0 clears.
    view::KeyEvent six; six.key = view::KeyCode::num6; six.is_down = true;
    kb.on_key_event(six);

    uint32_t ow = 0, oh = 0;
    auto rgba = view::render_to_rgba(kb, static_cast<uint32_t>(w),
                                     static_cast<uint32_t>(h), 1.0f, &ow, &oh);
    if (rgba.empty()) SKIP("Skia raster screenshot backend unavailable");
    REQUIRE(ow == static_cast<uint32_t>(w));
    REQUIRE(oh == static_cast<uint32_t>(h));
    write_png(kb, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
              "/tmp/sampler_mtk_mod_highlight.png");

    // mod button rects (panel coords) from build_typing_frame's append_controls:
    // x = {200,242,284,326,368,410}, y=53, w=36, h=32. mod_3 → x=326 (latched).
    const double lit = teal_fraction(rgba, ow, oh, 326, 53, 36, 32);
    const double neighbour = teal_fraction(rgba, ow, oh, 200, 53, 36, 32);  // mod_0, cleared
    CHECK(lit > 0.6);        // highlight covers the cell — no black inset
    CHECK(neighbour < 0.2);  // unlit neighbour stays dark — no overflow / mis-scale
}

// Fix 3: clicking the baked 🎹/⌨ toggle switches the rendered frame AND drives the
// intrinsic-size callback the integration wires to resize the window (mirrors
// mtk-demo). Proven by clicking the toggle rects and asserting the callback fires
// with the new frame's panel dims + the active frame changes; plus a render of the
// shorter piano frame at its own panel dims shows the full keybed (not clipped).
TEST_CASE("Fix 3: piano toggle swaps frame + fires the resize callback",
          "[tempo-sampler]") {
    using MTK = view::MusicalTypingKeyboard;
    MTK kb;
    REQUIRE(kb.mode() == MTK::Mode::typing);
    kb.set_bounds({0, 0, kb.panel_width(), kb.panel_height()});  // 732×266, unit scale

    float cb_w = 0, cb_h = 0; int fires = 0;
    kb.on_intrinsic_size_changed = [&](float nw, float nh) { cb_w = nw; cb_h = nh; ++fires; };

    // Baked toggles (append_toggle): piano icon swap at x=24, keyboard icon at
    // x=62, both y=22 w=36 h=26. Click the piano icon → piano frame (732×176).
    kb.on_mouse_down({24 + 18, 22 + 13});
    CHECK(fires == 1);
    CHECK(cb_w == 732.0f);
    CHECK(cb_h == 176.0f);                 // shorter piano frame
    CHECK(kb.mode() == MTK::Mode::piano);

    // Render the piano frame at ITS panel dims (the size the window resizes to):
    // the full keybed fills — white keys present down the lower band (no clip).
    kb.set_bounds({0, 0, kb.panel_width(), kb.panel_height()});  // now 732×176
    uint32_t ow = 0, oh = 0;
    auto rgba = view::render_to_rgba(kb, 732, 176, 1.0f, &ow, &oh);
    if (rgba.empty()) SKIP("Skia raster screenshot backend unavailable");
    REQUIRE(oh == 176);
    write_png(kb, 732, 176, "/tmp/sampler_mtk_piano_frame.png");
    // White-key centres along the lower keybed (piano keys y≈62..140).
    int light = 0;
    for (int x : {44, 76, 141, 269, 365}) if (is_light(pixel_at(rgba, ow, oh, x, 120))) ++light;
    CHECK(light >= 3);   // the full piano keybed rendered (not truncated typing)

    // Click the keyboard icon → back to the taller typing frame (732×266).
    kb.on_mouse_down({62 + 18, 22 + 13});
    CHECK(fires == 2);
    CHECK(cb_h == 266.0f);
    CHECK(kb.mode() == MTK::Mode::typing);

    kb.set_bounds({0, 0, 732, 266});
    write_png(kb, 732, 266, "/tmp/sampler_mtk_typing_frame.png");
    auto typing = view::render_to_rgba(kb, 732, 266, 1.0f, &ow, &oh);
    if (typing.empty()) SKIP("Skia raster screenshot backend unavailable");
    REQUIRE(oh == 266);   // full typing frame, no clipping at its panel dims
}
#endif  // __APPLE__

// FLEX SPEED (Logic): playing 2x faster halves the stretched (published) length;
// 1/2x doubles it. The loop still tempo-matches — speed just scales the ratio.
TEST_CASE("Flex Speed scales the stretched length", "[tempo-sampler][flex-speed]") {
    // Fresh fixture per speed (the offline render runs on a live worker; reusing
    // one fixture across speeds races the worker against the param change).
    auto pub_at_flex = [](int flex_idx) -> long {
        Fixture f;
        f.store.set_value(kOnsetSens, 0.0f);             // whole loop (1 region)
        auto loop = percussive_loop(48000, 8);           // 48000-frame source
        const float* ch[1] = {loop.data()};
        REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
        REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
        REQUIRE(wait_for([&] { return f.proc->num_slices() == 1; }));
        f.proc->set_loop_bpm_for_test(120.0);
        f.store.set_value(kFlexSpeed, static_cast<float>(flex_idx));
        f.proc->render_now(120.0);   // force a re-stretch at the new Flex Speed (the UI
                                     // listener does this in the app; no listener here)
        long prev = -1; int stable = 0; std::vector<float> l(512), r(512);
        for (int i = 0; i < 400 && stable < 25; ++i) {   // process() acks the new publish
            midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
            const long p = f.proc->published_frames();
            if (p > 0 && p == prev) ++stable; else { stable = 0; prev = p; }
        }
        return f.proc->published_frames();
    };
    const long base = pub_at_flex(2);   // 1x
    const long fast = pub_at_flex(3);   // 2x   -> half the length
    const long slow = pub_at_flex(1);   // 1/2x -> double the length
    INFO("flex base(1x)=" << base << " fast(2x)=" << fast << " slow(0.5x)=" << slow);
    CHECK(std::llabs(static_cast<long long>(fast) - base / 2) <= base / 20);   // ~half
    CHECK(std::llabs(static_cast<long long>(slow) - base * 2) <= base / 10);   // ~double
}

// GATE: off = One-Shot (key-up ignored, the slice plays through); on = key-up
// releases the envelope, so the same note-off goes quiet far sooner.
TEST_CASE("Gate off plays through note-off; Gate on releases", "[tempo-sampler][gate]") {
    auto energy_after_off = [](float gate) -> double {
        Fixture f;
        f.store.set_value(kOnsetSens, 0.0f);             // whole loop (1 region, ~1 s one-shot)
        f.store.set_value(kTempoLoop, 0.0f);             // one-shot (not looping)
        f.store.set_value(kGate, gate);
        auto loop = percussive_loop(48000, 6);
        const float* ch[1] = {loop.data()};
        REQUIRE(f.proc->load_loop(ch, 1, 48000, 48000.0));
        REQUIRE(wait_for([&] { return f.proc->has_sample(); }));
        REQUIRE(wait_for([&] { return f.proc->num_slices() == 1; }));
        f.proc->set_loop_bpm_for_test(120.0);
        std::vector<float> l(512), r(512);
        { midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r); }
        // note-on, one block, then note-off ~10 ms in.
        { midi::MidiBuffer m; m.add(midi::MidiEvent::note_on(0, kRoot, 110));
          process_midi(*f.proc, 120.0, m, l, r); }
        { midi::MidiBuffer m; m.add(midi::MidiEvent::note_off(0, kRoot));
          process_midi(*f.proc, 120.0, m, l, r); }
        double e = 0.0;
        for (int b = 0; b < 28; ++b) {                   // ~300 ms after note-off
            midi::MidiBuffer m; process_midi(*f.proc, 120.0, m, l, r);
            e += block_energy(l);
        }
        return e;
    };
    const double e_off = energy_after_off(0.0f);         // plays through
    const double e_on  = energy_after_off(1.0f);         // releases
    INFO("gate-off energy=" << e_off << "  gate-on energy=" << e_on);
    CHECK(e_off > e_on * 1.5);
}
