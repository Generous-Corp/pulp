// PulpTempoSampler — headless integration tests (Phase 4.11).
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

// Each slice maps to its own chromatic note (idx = note - root). A note off the
// map must be SILENT — it must NOT fall back to playing the whole sample (the
// Reaper/host "MIDI triggers the entire sample across the keyboard" bug).
TEST_CASE("MIDI note outside the slice map is silent (no whole-sample fallback)",
          "[tempo-sampler]") {
    Fixture f;
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
        process_block(*f.proc, 120.0, b == 0, 60, l, r);
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

    const int root = 60;  // default C3

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

TEST_CASE("musical typing maps QWERTY keys to slice notes (root-based)", "[tempo-sampler]") {
    SamplerEditorRoot root;
    root.current_root_note = [] { return 60; };  // base 'a' = root note
    std::vector<std::pair<int, bool>> notes;
    root.typing.on_note_on = [&](int n, float) { notes.emplace_back(n, true); };
    root.typing.on_note_off = [&](int n) { notes.emplace_back(n, false); };
    auto key = [](view::KeyCode k, bool down, bool rep = false) {
        view::KeyEvent e; e.key = k; e.is_down = down; e.is_repeat = rep; return e;
    };
    REQUIRE(root.on_key_event(key(view::KeyCode::a, true)));         // root + 0 -> note 60
    REQUIRE(root.on_key_event(key(view::KeyCode::a, true, true)));   // auto-repeat ignored
    REQUIRE(root.on_key_event(key(view::KeyCode::a, false)));        // note off 60
    REQUIRE(root.on_key_event(key(view::KeyCode::s, true)));         // root + 2 -> note 62
    REQUIRE_FALSE(root.on_key_event(key(view::KeyCode::num1, true)));  // unmapped -> host

    REQUIRE(notes.size() == 3);
    CHECK(notes[0] == std::make_pair(60, true));   // 'a' down  (slice 0 = root)
    CHECK(notes[1] == std::make_pair(60, false));  // 'a' up
    CHECK(notes[2] == std::make_pair(62, true));   // 's' down  (slice 2)
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

// LOOP toggle: enable/disable Forward looping (default one-shot). Drive the REAL
// toggle -> on_toggle -> bound kTempoLoop param.
TEST_CASE("LOOP toggle flips the loop parameter (default one-shot)", "[tempo-sampler]") {
    Fixture f;
    auto buf = sine(440.0, 48000.0, 24000);
    const float* ch[1] = {buf.data()};
    REQUIRE(f.proc->load_loop(ch, 1, 24000, 48000.0));

    auto editor = f.proc->create_view();
    REQUIRE(editor);
    auto* loop = find_toggle(editor.get(), "LOOP");
    REQUIRE(loop != nullptr);

    CHECK(f.store.get_value(kTempoLoop) < 0.5f);   // default: one-shot
    const auto b = loop->local_bounds();
    loop->on_mouse_down({b.width * 0.5f, b.height * 0.5f});  // -> on_toggle(true) -> set_value
    CHECK(f.store.get_value(kTempoLoop) >= 0.5f);  // looping enabled
    loop->on_mouse_down({b.width * 0.5f, b.height * 0.5f});  // toggle back off
    CHECK(f.store.get_value(kTempoLoop) < 0.5f);
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
    REQUIRE(root != nullptr);
    REQUIRE(root->on_global_key);  // the hook the standalone host fires per key

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
#endif  // __APPLE__
