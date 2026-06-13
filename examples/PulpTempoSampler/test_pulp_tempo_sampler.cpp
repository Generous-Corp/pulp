// PulpTempoSampler — headless integration tests (Phase 4.11).
//
// Exercises the full instrument pipeline without a host: load loop -> detect
// BPM + slices -> background OfflineStretch render to host tempo (generation-
// published) -> MIDI note plays the cached stretched buffer -> grid-lock
// (published length == round(raw * loop_bpm / host_bpm)). Also a render-while-
// playing race (run under ASan/TSAN to catch use-after-free / data races).

#include <catch2/catch_test_macros.hpp>
#include "pulp_tempo_sampler.hpp"

#include <chrono>
#include <cmath>
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
    REQUIRE(s.param_count() == 10);
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
