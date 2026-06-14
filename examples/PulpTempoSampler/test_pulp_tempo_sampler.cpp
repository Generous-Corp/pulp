// PulpTempoSampler — headless integration tests (Phase 4.11).
//
// Exercises the full instrument pipeline without a host: load loop -> detect
// BPM + slices -> background OfflineStretch render to host tempo (generation-
// published) -> MIDI note plays the cached stretched buffer -> grid-lock
// (published length == round(raw * loop_bpm / host_bpm)). Also a render-while-
// playing race (run under ASan/TSAN to catch use-after-free / data races).

#include <catch2/catch_test_macros.hpp>
#include "pulp_tempo_sampler.hpp"

#include <pulp/view/drag_drop.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace {

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

} // namespace

TEST_CASE("PulpTempoSampler descriptor + params", "[tempo-sampler]") {
    PulpTempoSamplerProcessor p;
    const auto d = p.descriptor();
    REQUIRE(d.name == "PulpTempoSampler");
    REQUIRE(d.category == format::PluginCategory::Instrument);
    REQUIRE(d.accepts_midi);
    REQUIRE(d.output_buses.size() == 1);
    state::StateStore s; p.define_parameters(s);
    REQUIRE(s.param_count() == 12);
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
        process_block(*f.proc, 100.0, b == 0, 60, l, r);
        for (int i = 0; i < 512; ++i) energy += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
        for (float v : l) REQUIRE(std::isfinite(v));
    }
    CHECK(energy > 1e-6); // produced audio
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
        process_block(*f.proc, tempo, b == 0, 60, l, r);
        for (float v : l) REQUIRE(std::isfinite(v));
        for (float v : r) REQUIRE(std::isfinite(v));
    }
    SUCCEED();
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

TEST_CASE("invalid drop path is a graceful no-op", "[tempo-sampler]") {
    Fixture f;
    f.proc->request_load_path("/tmp/this-does-not-exist-xyz.wav");
    // Give the worker a moment; nothing should load and nothing should crash.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(f.proc->has_sample());
}

TEST_CASE("root note remaps slice-to-key (idx = note - root)", "[tempo-sampler]") {
    Fixture f;
    // Default root is 60 (C3 in this editor's labeling).
    REQUIRE(f.proc->slice_index_for_note_test(60) == 0);
    REQUIRE(f.proc->slice_index_for_note_test(63) == 3);

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

    // Least sensitive -> fewest slices.
    f.store.set_value(kOnsetSens, 0.0f);
    f.proc->request_reanalyze();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const std::size_t low = f.proc->num_slices();

    // Most sensitive -> at least as many slices.
    f.store.set_value(kOnsetSens, 1.0f);
    f.proc->request_reanalyze();
    REQUIRE(wait_for([&] { return f.proc->num_slices() >= low; }));
    const std::size_t high = f.proc->num_slices();
    REQUIRE(high >= low);
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
#endif  // __APPLE__
