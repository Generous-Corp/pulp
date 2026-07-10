// LFO — golden shapes, the eight sync modes, and what a bounce can promise.
//
// The properties worth guarding are the ones a user would only notice after
// bouncing twice: that the phase is a pure function of the host's position, so the
// same beat always yields the same voltage regardless of block size; that the two
// non-deterministic modes are exactly the two that say they are; and that the
// quadrature lock really is a quarter cycle, because that is the only thing making
// the pair usable as an (X, Y) modulation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "lfo_processor.hpp"

#include <pulp/format/headless.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTempo = 120.0;

state::ParamID lpid(state::ParamID id, std::size_t channel) {
    return static_cast<state::ParamID>(param_for(id, channel));
}

float as_param(SyncMode m) { return static_cast<float>(static_cast<int>(m)); }
float as_param(NoteUnit u) { return static_cast<float>(static_cast<int>(u)); }
float as_param(InputMode m) { return static_cast<float>(static_cast<int>(m)); }

/// The two channels are independent, so a test that means "the LFO does X" has to
/// say it of both — otherwise it is a test of the left channel wearing a general
/// name.
void set_both(format::HeadlessHost& host, state::ParamID id, float v) {
    for (std::size_t ch = 0; ch < kChannelCount; ++ch)
        host.state().set_value(lpid(id, ch), v);
}

void set_sync(format::HeadlessHost& host, SyncMode m) {
    set_both(host, LfoProcessor::kSyncMode, as_param(m));
}

/// A block of context. Everything a mode could read.
struct Ctx {
    double position_beats = 0.0;
    std::int64_t position_samples = 0;
    bool playing = true;
    bool started = false;
    double tempo = kTempo;
};

/// Render one block and return channel `ch`. Passing `midi` goes straight at the
/// processor, because HeadlessHost's convenience overload has no MIDI port.
std::vector<float> render(format::HeadlessHost& host, const Ctx& c, int frames, int ch,
                          midi::MidiBuffer* midi = nullptr) {
    const auto n = static_cast<std::size_t>(frames);
    audio::Buffer<float> in(2, n), out(2, n);
    in.clear();
    out.clear();
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, n);
    auto ov = out.view();

    format::ProcessContext ctx;
    ctx.sample_rate = kSampleRate;
    ctx.num_samples = frames;
    ctx.is_playing = c.playing;
    ctx.transport_started = c.started;
    ctx.tempo_bpm = c.tempo;
    ctx.position_beats = c.position_beats;
    ctx.position_samples = c.position_samples;

    if (midi != nullptr) {
        midi::MidiBuffer out_midi;
        host.processor()->process(ov, iv, *midi, out_midi, ctx);
    } else {
        host.process(ov, iv, ctx);
    }

    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = out.channel(static_cast<std::size_t>(ch))[i];
    return v;
}

/// The common case: a beat position, transport rolling.
std::vector<float> at_beat(format::HeadlessHost& host, double beats, int frames,
                           int ch = 0, bool playing = true) {
    Ctx c;
    c.position_beats = beats;
    c.playing = playing;
    c.position_samples =
        static_cast<std::int64_t>(beats / beats_per_sample(kTempo, kSampleRate));
    return render(host, c, frames, ch);
}

/// An absolute sample position, which is what the hertz modes read. `tempo` varies
/// so a test can prove they ignore it.
std::vector<float> at_sample(format::HeadlessHost& host, std::int64_t pos, int frames,
                             int ch = 0, double tempo = kTempo, bool playing = true) {
    Ctx c;
    c.position_samples = pos;
    c.playing = playing;
    c.tempo = tempo;
    c.position_beats = static_cast<double>(pos) / kSampleRate * (tempo / 60.0);
    return render(host, c, frames, ch);
}

/// Select one shape by soloing its depth, on both channels. The mixer subsumes the
/// selector this replaced, and every test that wants a pure shape says so.
void solo(format::HeadlessHost& host, state::ParamID shape) {
    for (auto id : {LfoProcessor::kSine, LfoProcessor::kTriangle, LfoProcessor::kSaw,
                    LfoProcessor::kSquare})
        set_both(host, id, id == shape ? 1.0f : 0.0f);
}

/// A prepared host. `HeadlessHost` is neither copyable nor movable — it hands the
/// processor a pointer to its own StateStore — so this is a subclass rather than a
/// factory returning one by value.
struct Lfo : format::HeadlessHost {
    Lfo() : format::HeadlessHost(create_lfo) { prepare(kSampleRate, 4096, 2, 2); }
};

const LfoProcessor& lfo_of(const format::HeadlessHost& host) {
    return *static_cast<const LfoProcessor*>(host.processor());
}

}  // namespace

TEST_CASE("LFO descriptor is a bipolar modulation pair", "[brew][lfo]") {
    auto proc = create_lfo();
    const auto d = proc->descriptor();
    REQUIRE(d.name == "LFO");
    REQUIRE(d.manufacturer == "Bitches Brew");
    // `Reset By Note` needs note-ons, and an AU host only routes MIDI to an effect
    // packaged `aumf` — which this flag is what selects.
    REQUIRE(d.accepts_midi);
}

TEST_CASE("every control is registered on both channels", "[brew][lfo][stereo]") {
    // A control missing from `controls()` compiles, registers nothing, and then
    // reads back zero — which for `Output Scale` is a silent LFO and for `Sync` is a
    // mode nobody chose. Only a test notices.
    Lfo host;
    for (const auto& c : LfoProcessor::controls())
        for (std::size_t ch = 0; ch < kChannelCount; ++ch)
            REQUIRE(host.state().info(lpid(c.id, ch)) != nullptr);
    REQUIRE(host.state().all_params().size() ==
            LfoProcessor::controls().size() * kChannelCount);
}

// ------------------------------------------------------------------- shapes

TEST_CASE("LFO shapes hit their golden values", "[brew][lfo][golden]") {
    SECTION("sine crosses zero rising at phase 0") {
        REQUIRE_THAT(lfo_shape(Waveform::sine, 0.0), WithinAbs(0.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::sine, 0.25), WithinAbs(1.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::sine, 0.5), WithinAbs(0.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::sine, 0.75), WithinAbs(-1.0, 1e-6));
    }

    SECTION("triangle starts at its trough and peaks at half a cycle") {
        REQUIRE_THAT(lfo_shape(Waveform::triangle, 0.0), WithinAbs(-1.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::triangle, 0.25), WithinAbs(0.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::triangle, 0.5), WithinAbs(1.0, 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::triangle, 0.75), WithinAbs(0.0, 1e-6));
    }

    SECTION("saws are each other's mirror") {
        for (double p : {0.0, 0.1, 0.5, 0.9}) {
            REQUIRE_THAT(lfo_shape(Waveform::saw_up, p),
                         WithinAbs(-lfo_shape(Waveform::saw_down, p), 1e-6));
        }
        REQUIRE_THAT(lfo_shape(Waveform::saw_up, 0.0), WithinAbs(-1.0, 1e-6));
    }

    SECTION("square starts high and flips at half a cycle") {
        REQUIRE(lfo_shape(Waveform::square, 0.0) == 1.0f);
        REQUIRE(lfo_shape(Waveform::square, 0.499) == 1.0f);
        REQUIRE(lfo_shape(Waveform::square, 0.5) == -1.0f);
        REQUIRE(lfo_shape(Waveform::square, 0.999) == -1.0f);
    }

    // A phase of 1.0 is the same instant as 0.0, and a negative phase must index the
    // end of the cycle rather than the wrong half of it.
    SECTION("phase wraps in both directions") {
        REQUIRE_THAT(lfo_shape(Waveform::saw_up, 1.0),
                     WithinAbs(lfo_shape(Waveform::saw_up, 0.0), 1e-6));
        REQUIRE_THAT(lfo_shape(Waveform::saw_up, -0.25),
                     WithinAbs(lfo_shape(Waveform::saw_up, 0.75), 1e-6));
        REQUIRE(wrap_phase(-0.25) == 0.75);
        REQUIRE(wrap_phase(2.25) == 0.25);
    }
}

TEST_CASE("waveform parameter clamps rather than wraps", "[brew][lfo]") {
    REQUIRE(waveform_from_param(0.0f) == Waveform::sine);
    REQUIRE(waveform_from_param(4.0f) == Waveform::square);
    // A host handing back an out-of-range value must not select a different shape.
    REQUIRE(waveform_from_param(-3.0f) == Waveform::sine);
    REQUIRE(waveform_from_param(99.0f) == Waveform::square);
    // Rounding, not truncation: 1.6 is nearer triangle's neighbour than triangle.
    REQUIRE(waveform_from_param(1.6f) == Waveform::saw_up);
}

TEST_CASE("unipolar mapping folds the negative half up", "[brew][lfo]") {
    REQUIRE(to_unipolar(-1.0f) == 0.0f);
    REQUIRE(to_unipolar(0.0f) == 0.5f);
    REQUIRE(to_unipolar(1.0f) == 1.0f);
}

// --------------------------------------------------------------- phase lock

TEST_CASE("LFO phase is locked to the host position", "[brew][lfo][phase]") {
    // One cycle per beat: the phase is the fractional part of the beat count.
    REQUIRE_THAT(lfo_phase(0.0, 1.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(lfo_phase(2.25, 1.0), WithinAbs(0.25, 1e-12));
    REQUIRE_THAT(lfo_phase(57.5, 1.0), WithinAbs(0.5, 1e-12));

    SECTION("one cycle per bar means phase 0 on every downbeat") {
        for (double bar = 0.0; bar < 5.0; bar += 1.0)
            REQUIRE_THAT(lfo_phase(bar * 4.0, 4.0), WithinAbs(0.0, 1e-12));
    }

    SECTION("a degenerate rate yields a still LFO rather than a division by zero") {
        REQUIRE(lfo_phase(3.0, 0.0) == 0.0);
        REQUIRE(cycles_from_beats(3.0, 0.0, 0.0) == 0.0);
    }
}

// The reason for deriving phase from position instead of accumulating it: the same
// beat yields the same voltage no matter how the host chopped the timeline.
TEST_CASE("LFO output is independent of block size", "[brew][lfo][phase]") {
    Lfo a, b;

    // `a` renders one 2048-sample block; `b` renders it as four 512s.
    const auto whole = at_beat(a, 0.0, 2048);

    std::vector<float> pieces;
    const double step = beats_per_sample(kTempo, kSampleRate) * 512.0;
    for (int i = 0; i < 4; ++i) {
        const auto part = at_beat(b, step * i, 512);
        pieces.insert(pieces.end(), part.begin(), part.end());
    }

    REQUIRE(pieces.size() == whole.size());
    for (std::size_t n = 0; n < whole.size(); ++n) {
        CAPTURE(n);
        REQUIRE_THAT(pieces[n], WithinAbs(whole[n], 1e-6));
    }
}

TEST_CASE("LFO advances within a block rather than stepping per block",
          "[brew][lfo][phase]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);

    const auto block = at_beat(host, 0.0, 512);
    // A block-rate LFO would hold one value for all 512 samples; a stepped CV is an
    // audible zipper on whatever it drives.
    REQUIRE(block.front() != block.back());
    for (std::size_t n = 1; n < block.size(); ++n) REQUIRE(block[n] > block[n - 1]);
}

// ------------------------------------------------------------- the sync modes

TEST_CASE("the sync taxonomy answers two independent questions", "[brew][lfo][sync]") {
    // Which knob sets the frequency...
    REQUIRE(sync_uses_hertz(SyncMode::free));
    REQUIRE(sync_uses_hertz(SyncMode::free2));
    REQUIRE(sync_uses_hertz(SyncMode::free3));
    REQUIRE(sync_uses_beats(SyncMode::tempo));
    REQUIRE(sync_uses_beats(SyncMode::transport));
    REQUIRE(sync_uses_beats(SyncMode::transport2));
    // ...and what the transport does to it.
    REQUIRE(sync_runs_when_stopped(SyncMode::free));
    REQUIRE(sync_runs_when_stopped(SyncMode::tempo));
    REQUIRE(sync_runs_when_stopped(SyncMode::transport));
    REQUIRE_FALSE(sync_runs_when_stopped(SyncMode::transport2));
    REQUIRE_FALSE(sync_runs_when_stopped(SyncMode::free2));
    REQUIRE_FALSE(sync_runs_when_stopped(SyncMode::free3));

    // Exactly two modes cannot be reproduced by a bounce, and they are exactly the
    // two that keep oscillating against a clock the timeline knows nothing about.
    for (int i = 0; i < kSyncModeCount; ++i) {
        const auto m = static_cast<SyncMode>(i);
        CAPTURE(i);
        const bool wall_clock_only = m == SyncMode::free || m == SyncMode::tempo;
        REQUIRE(sync_is_deterministic(m) == !wall_clock_only);
    }

    // Only Free2 snaps its phase back on the play edge. That is the whole of what
    // separates it from Free3.
    REQUIRE(sync_resets_on_play(SyncMode::free2));
    REQUIRE_FALSE(sync_resets_on_play(SyncMode::free3));
}

TEST_CASE("Transport locks to the timeline and keeps running when parked",
          "[brew][lfo][sync]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    set_sync(host, SyncMode::transport);   // the default, spelled out

    SECTION("while rolling it is exactly the position") {
        const auto block = at_beat(host, 0.25, 8);
        REQUIRE_THAT(block.front(), WithinAbs(lfo_of(host).value_at(0.25), 1e-6f));
    }

    SECTION("while parked it drifts on, which is the only thing Transport2 does not") {
        Ctx c;
        c.position_beats = 0.3;
        c.playing = false;
        const auto first = render(host, c, 256, 0);
        const auto second = render(host, c, 256, 0);
        // Same frozen position, and yet the voltage advanced: the wall clock ran.
        REQUIRE(first.front() != second.front());
        REQUIRE(first.back() != first.front());
    }
}

TEST_CASE("Transport2 holds the LFO at the playhead", "[brew][lfo][sync]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    set_sync(host, SyncMode::transport2);

    const auto block = at_beat(host, 0.3, 256, 0, /*playing=*/false);
    for (float v : block) REQUIRE(v == block.front());

    // ...and a second block at the same parked position repeats it exactly.
    REQUIRE(at_beat(host, 0.3, 256, 0, false) == block);
}

TEST_CASE("the drift a parked Transport accumulated is discarded on the play edge",
          "[brew][lfo][sync][safety]") {
    // Otherwise the first bar of a bounce would depend on how long the user left the
    // transport stopped before pressing play — the loudest possible way to break the
    // promise the rest of this suite makes.
    Lfo rolled, parked;
    solo(rolled, LfoProcessor::kSaw);
    solo(parked, LfoProcessor::kSaw);

    // One host sits parked for a while first.
    Ctx stop;
    stop.playing = false;
    for (int i = 0; i < 8; ++i) render(parked, stop, 512, 0);

    Ctx go;
    go.playing = true;
    go.started = true;
    REQUIRE(render(parked, go, 512, 0) == render(rolled, go, 512, 0));
}

TEST_CASE("Tempo free-runs at the tempo's rate and never locks", "[brew][lfo][sync]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    set_sync(host, SyncMode::tempo);

    // Rendering the same position twice gives different voltages, because there is
    // no position in this mode at all — only an accumulator.
    const auto first = at_beat(host, 0.0, 512);
    const auto second = at_beat(host, 0.0, 512);
    REQUIRE(first.front() != second.front());

    // And it keeps going with the playhead parked.
    Ctx c;
    c.playing = false;
    const auto a = render(host, c, 512, 0);
    const auto b = render(host, c, 512, 0);
    REQUIRE(a.front() != b.front());
}

TEST_CASE("Start/Stop is the transport, as a very slow square", "[brew][lfo][sync]") {
    Lfo host;
    set_sync(host, SyncMode::start_stop);

    for (float v : at_beat(host, 3.0, 32, 0, /*playing=*/true)) REQUIRE(v == 1.0f);
    for (float v : at_beat(host, 3.0, 32, 0, /*playing=*/false)) REQUIRE(v == -1.0f);

    SECTION("the waveform mix is ignored — it is not really an LFO") {
        set_both(host, LfoProcessor::kSine, 0.0f);
        set_both(host, LfoProcessor::kSquare, 1.0f);
        for (float v : at_beat(host, 3.0, 32)) REQUIRE(v == 1.0f);
    }

    SECTION("but Offset still applies, so it can drive a unipolar input") {
        set_both(host, LfoProcessor::kOffset, -1.0f);
        for (float v : at_beat(host, 3.0, 32, 0, /*playing=*/true)) REQUIRE(v == 0.0f);
    }

    SECTION("Smooth turns the play edge into a fade-in") {
        set_both(host, LfoProcessor::kSmoothMs, 500.0f);
        const auto rising = at_beat(host, 0.0, 512, 0, /*playing=*/true);
        REQUIRE(rising.front() < rising.back());
        REQUIRE(rising.back() < 1.0f);   // 512 samples is nowhere near 500 ms
    }
}

TEST_CASE("Free3 is what a position-derived free run actually is",
          "[brew][lfo][sync][free]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    set_sync(host, SyncMode::free3);
    set_both(host, LfoProcessor::kSpeedHz, 2.0f);

    SECTION("it ignores the tempo") {
        // The whole point of the mode. Same sample position, different tempo, same
        // voltage — otherwise it is not free of the transport at all.
        REQUIRE(at_sample(host, 12345, 256, 0, 120.0) ==
                at_sample(host, 12345, 256, 0, 71.5));
    }

    SECTION("it is derived from the position, never accumulated") {
        // Play into a block, versus locate straight to it: the same samples, or a
        // bounce is not reproducible and a locate lands in the wrong place.
        Lfo located;
        solo(located, LfoProcessor::kSaw);
        set_sync(located, SyncMode::free3);
        set_both(located, LfoProcessor::kSpeedHz, 2.0f);
        set_both(host, LfoProcessor::kRandom, 0.6f);
        set_both(located, LfoProcessor::kRandom, 0.6f);

        constexpr int kFrames = 256;
        std::vector<float> from_playing;
        for (std::int64_t b = 0; b <= 20; ++b)
            from_playing = at_sample(host, b * kFrames, kFrames);
        REQUIRE(from_playing == at_sample(located, 20 * kFrames, kFrames));
    }

    SECTION("a stopped transport holds it still") {
        const auto out = at_sample(host, 4321, 128, 0, kTempo, /*playing=*/false);
        for (float v : out) REQUIRE(v == out.front());
    }

    SECTION("one cycle is exactly one over the rate, in seconds") {
        const auto& lfo = lfo_of(host);
        // At 2 Hz a cycle is half a second. Points one cycle apart agree.
        REQUIRE_THAT(lfo.value_at_time(0.125), WithinAbs(lfo.value_at_time(0.625), 1e-6));
        // And a quarter cycle apart, they do not.
        REQUIRE(lfo.value_at_time(0.125) != lfo.value_at_time(0.25));
    }
}

TEST_CASE("Free keeps oscillating with the playhead parked", "[brew][lfo][sync][free]") {
    // The one mode with no timeline in it at all — and, with Tempo, the reason
    // `sync_is_deterministic` exists.
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    set_sync(host, SyncMode::free);
    set_both(host, LfoProcessor::kSpeedHz, 5.0f);

    Ctx c;
    c.playing = false;
    const auto a = render(host, c, 256, 0);
    const auto b = render(host, c, 256, 0);
    REQUIRE(a.front() != b.front());
    REQUIRE(a.back() != a.front());

    // ...and a locate does not put it back. That is exactly what the mode costs.
    REQUIRE(at_sample(host, 0, 256) != at_sample(host, 0, 256));
}

TEST_CASE("Free2 snaps its phase to the play edge and Free3 does not",
          "[brew][lfo][sync][free]") {
    // The only difference between the two modes, and it is invisible unless the
    // transport starts somewhere other than a whole number of cycles in.
    Lfo two, three;
    for (auto* h : {&two, &three}) {
        solo(*h, LfoProcessor::kSaw);
        set_both(*h, LfoProcessor::kSpeedHz, 2.0f);
    }
    set_sync(two, SyncMode::free2);
    set_sync(three, SyncMode::free3);

    Ctx start;
    start.position_samples = 33333;   // 1.389 s in: not a whole number of cycles
    start.position_beats = 1.5;
    start.playing = true;
    start.started = true;

    // Free2 restarts its cycle here, so a saw begins at its trough.
    REQUIRE_THAT(render(two, start, 8, 0).front(), WithinAbs(-1.0f, 1e-5f));

    // Free3 carries on from where the timeline says it is: 33333/48000 s at 2 Hz is
    // 1.38888 cycles, so a saw sits at 2(0.38888) − 1. Pinned, not merely "not the
    // trough" — a mode that reset to some *other* phase would pass a loose bound.
    const double cycles = 33333.0 / kSampleRate * 2.0;
    const auto expected = static_cast<float>(2.0 * (cycles - std::floor(cycles)) - 1.0);
    REQUIRE_THAT(render(three, start, 8, 0).front(), WithinAbs(expected, 1e-4f));
}

// --------------------------------------------------------------- quadrature

TEST_CASE("a channel locked with Quad leads its leader by its own Phase",
          "[brew][lfo][quadrature]") {
    Lfo host;
    solo(host, LfoProcessor::kSine);
    set_both(host, LfoProcessor::kBeats, 1.0f);
    host.state().set_value(lpid(LfoProcessor::kSyncMode, 1), as_param(SyncMode::quadrature));
    host.state().set_value(lpid(LfoProcessor::kPhaseDegrees, 1), 90.0f);

    const auto& lfo = lfo_of(host);
    for (double cycles : {0.0, 0.1, 0.37, 0.9}) {
        CAPTURE(cycles);
        REQUIRE_THAT(lfo.value_at_cycles(1, cycles),
                     WithinAbs(lfo.value_at_cycles(0, cycles + kQuadratureOffset), 1e-6f));
    }

    // And sine plus its quadrature trace a unit circle: sin² + cos² = 1. Both
    // channels come out of one block, because rendering twice would advance nothing
    // but would still be two calls to prove one claim.
    audio::Buffer<float> in(2, 512), out(2, 512);
    in.clear();
    out.clear();
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, 512);
    auto ov = out.view();
    format::ProcessContext ctx;
    ctx.sample_rate = kSampleRate;
    ctx.num_samples = 512;
    ctx.is_playing = true;
    ctx.tempo_bpm = kTempo;
    host.process(ov, iv, ctx);

    for (std::size_t n = 0; n < 512; n += 37) {
        CAPTURE(n);
        const float x = out.channel(0)[n], y = out.channel(1)[n];
        REQUIRE_THAT(x * x + y * y, WithinAbs(1.0, 1e-5));
    }
}

TEST_CASE("a follower reads the leader's clock, not its own knobs",
          "[brew][lfo][quadrature]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    host.state().set_value(lpid(LfoProcessor::kSyncMode, 1), as_param(SyncMode::quadrature));
    // Give the follower a wildly different rate. It must be ignored: the lock is a
    // lock, not a suggestion.
    host.state().set_value(lpid(LfoProcessor::kBeats, 0), 1.0f);
    host.state().set_value(lpid(LfoProcessor::kBeats, 1), 16.0f);

    // Both channels' cycle counts come from the leader, so at the same beat the
    // follower's phase is the leader's plus its own (zero) offset.
    REQUIRE_THAT(at_beat(host, 0.25, 4, 1).front(),
                 WithinAbs(lfo_of(host).value_at_cycles(0, 0.25), 1e-6f));
}

TEST_CASE("Quad on the left channel falls back rather than locking to itself",
          "[brew][lfo][quadrature]") {
    // There is no other channel to follow. A silent no-op would look like a broken
    // mode; falling back to the mode Quad is a phase-shifted copy of does not.
    Lfo host;
    host.state().set_value(lpid(LfoProcessor::kSyncMode, 0), as_param(SyncMode::quadrature));
    REQUIRE(lfo_of(host).sync_mode(0) == SyncMode::transport);
    REQUIRE(lfo_of(host).sync_mode(1) == SyncMode::transport);
}

// ------------------------------------------------------------- output stage

TEST_CASE("LFO honors the suite's output stage", "[brew][lfo]") {
    Lfo host;
    solo(host, LfoProcessor::kSquare);

    REQUIRE(at_beat(host, 0.0, 8).front() == 1.0f);

    SECTION("scale attenuates") {
        set_both(host, LfoProcessor::kOutputScale, 0.25f);
        REQUIRE(at_beat(host, 0.0, 8).front() == 0.25f);
    }

    SECTION("invert flips polarity") {
        set_both(host, LfoProcessor::kInvert, 1.0f);
        REQUIRE(at_beat(host, 0.0, 8).front() == -1.0f);
    }

    // `Unipolar` is gone: `Offset` subsumes it, and generalizes. Half the depth plus
    // half an offset is the old unipolar square, and unlike a toggle it also reaches
    // every partial offset in between.
    SECTION("a depth and an offset keep the whole cycle positive") {
        set_both(host, LfoProcessor::kSquare, 0.5f);
        set_both(host, LfoProcessor::kOffset, 0.5f);
        REQUIRE(at_beat(host, 0.0, 8).front() == 1.0f);   // square high  → 1.0
        REQUIRE(at_beat(host, 0.5, 8).front() == 0.0f);   // square low   → 0.0
    }

    SECTION("phase offset rotates the cycle") {
        // A square offset by 180° is high exactly where it was low.
        set_both(host, LfoProcessor::kPhaseDegrees, 180.0f);
        REQUIRE(at_beat(host, 0.0, 8).front() == -1.0f);
    }
}

// Bypass means stop driving the patch — not freeze at whatever voltage the cycle
// happened to reach.
TEST_CASE("LFO emits nothing while bypassed", "[brew][lfo][safety][bypass]") {
    Lfo host;
    solo(host, LfoProcessor::kSquare);

    audio::Buffer<float> in(2, 512), out(2, 512);
    in.clear();
    out.clear();
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, 512);
    auto ov = out.view();

    format::ProcessContext ctx;
    ctx.sample_rate = kSampleRate;
    ctx.num_samples = 512;
    ctx.is_playing = true;
    ctx.tempo_bpm = kTempo;
    ctx.is_bypassed = true;
    host.process(ov, iv, ctx);

    for (int n = 0; n < 512; ++n) {
        REQUIRE(out.channel(0)[static_cast<std::size_t>(n)] == 0.0f);
        REQUIRE(out.channel(1)[static_cast<std::size_t>(n)] == 0.0f);
    }
}

// ------------------------------------------------------------------ the mixer

// The mixer subsumes the selector it replaced: one depth at full and the rest at
// zero is exactly the old single-shape behaviour.
TEST_CASE("depths sum, and a solo depth is the pure shape", "[brew][lfo][mix]") {
    LfoMix m{};
    m.sine = 1.0f;
    m.triangle = 0.0f;
    for (double p = 0.0; p < 1.0; p += 0.05) {
        CAPTURE(p);
        REQUIRE_THAT(lfo_mix_value(m, p, 0), WithinAbs(lfo_shape(Waveform::sine, p), 1e-6));
    }

    // Two shapes at once is their sum, sample for sample.
    LfoMix both = m;
    both.saw = 1.0f;
    for (double p = 0.0; p < 1.0; p += 0.05) {
        CAPTURE(p);
        REQUIRE_THAT(lfo_mix_value(both, p, 0),
                     WithinAbs(lfo_shape(Waveform::sine, p) + lfo_shape(Waveform::saw_up, p),
                               1e-6));
    }
}

// Depths are bipolar. A negative depth subtracts the shape, which is how the
// down-saw of the old enum is reached without a fifth entry.
TEST_CASE("a negative depth subtracts its shape", "[brew][lfo][mix]") {
    LfoMix up{};
    up.sine = 0.0f;
    up.saw = 1.0f;
    LfoMix down = up;
    down.saw = -1.0f;
    for (double p = 0.05; p < 1.0; p += 0.1) {
        CAPTURE(p);
        REQUIRE_THAT(lfo_mix_value(down, p, 0), WithinAbs(-lfo_mix_value(up, p, 0), 1e-6));
    }
}

// The sum is deliberately not clamped mid-chain: the depths at full reach past the
// rail, and flattening that before `offset` and the output scale have spoken would
// silently discard a mix the user asked for. `resolve_output` clamps once, at the
// jack.
TEST_CASE("the mix is clamped at the jack, not inside the mixer", "[brew][lfo][mix]") {
    LfoMix loud{};
    loud.sine = 1.0f;
    loud.triangle = 1.0f;
    loud.square = 1.0f;

    float peak = 0.0f;
    for (int i = 0; i < 512; ++i)
        peak = std::max(peak, lfo_mix_value(loud, static_cast<double>(i) / 512, 0));
    REQUIRE(peak > 1.0f);

    // The jack sees it clamped, and exactly at the rail — proof the clamp is the
    // output stage's and not a quiet rescale inside the mixer.
    Lfo host;
    for (auto id : {LfoProcessor::kTriangle, LfoProcessor::kSquare})
        set_both(host, id, 1.0f);
    bool saw_rail = false;
    for (int i = 0; i < 64; ++i) {
        for (float v : at_beat(host, static_cast<double>(i) / 64.0, 4)) {
            REQUIRE(v <= 1.0f);
            REQUIRE(v >= -1.0f);
            if (v == 1.0f) saw_rail = true;
        }
    }
    REQUIRE(saw_rail);
}

// ------------------------------------------------------- asymmetry / pulse width

TEST_CASE("asymmetry moves the waveform's centre", "[brew][lfo][mix]") {
    REQUIRE_THAT(warp_phase(0.3, 0.5), WithinAbs(0.3, 1e-12));    // 0.5 is identity
    REQUIRE_THAT(warp_phase(0.25, 0.25), WithinAbs(0.5, 1e-12));  // centre pulled early

    LfoMix m{};
    m.sine = 0.0f;
    m.square = 1.0f;
    m.asymmetry = 0.25f;
    int high = 0;
    const int n = 1000;
    for (int i = 0; i < n; ++i)
        if (lfo_mix_value(m, static_cast<double>(i) / n, 0) > 0.0f) ++high;
    REQUIRE(std::abs(high - 250) <= 2);
}

TEST_CASE("pulse width sets the square's duty cycle", "[brew][lfo][mix]") {
    LfoMix m{};
    m.sine = 0.0f;
    m.square = 1.0f;
    m.pulse_width = 0.8f;
    int high = 0;
    const int n = 1000;
    for (int i = 0; i < n; ++i)
        if (lfo_mix_value(m, static_cast<double>(i) / n, 0) > 0.0f) ++high;
    REQUIRE(std::abs(high - 800) <= 2);
}

// ---------------------------------------------------------------- sample & hold

// One level per cycle, held flat across it. That is what a noise source feeding a
// hardware sample-and-hold does, and a level that moved within the cycle would be
// noise, not S&H.
TEST_CASE("random holds one level for a whole cycle", "[brew][lfo][random]") {
    LfoMix m{};
    m.sine = 0.0f;
    m.random = 1.0f;
    const float held = lfo_mix_value(m, 0.0, 7);
    for (double p = 0.0; p < 1.0; p += 0.05) {
        CAPTURE(p);
        REQUIRE(lfo_mix_value(m, p, 7) == held);
    }
    // And it is a different level in the next cycle.
    REQUIRE(lfo_mix_value(m, 0.0, 8) != held);
}

// The property that separates this from every other CV utility: render twice, get
// the same samples. A generator advanced once per cycle could not promise it.
TEST_CASE("random is a pure function of the cycle and the seed", "[brew][lfo][random]") {
    LfoMix a{};
    a.sine = 0.0f;
    a.random = 1.0f;
    a.seed = 0;
    LfoMix b = a;

    for (std::int64_t cycle : {std::int64_t{0}, std::int64_t{12345}, std::int64_t{-7}}) {
        CAPTURE(cycle);
        REQUIRE(lfo_mix_value(a, 0.3, cycle) == lfo_mix_value(b, 0.3, cycle));
    }

    // A different seed rerolls the whole sequence.
    b.seed = 1;
    int differing = 0;
    for (std::int64_t cycle = 0; cycle < 32; ++cycle)
        if (lfo_mix_value(a, 0.0, cycle) != lfo_mix_value(b, 0.0, cycle)) ++differing;
    REQUIRE(differing >= 30);

    // And a negative cycle index — the timeline before the project's origin, which a
    // host will happily ask for — is as valid as any other.
    REQUIRE(std::isfinite(lfo_mix_value(a, 0.0, -99)));
}

// ---------------------------------------------------------------------- noise

TEST_CASE("noise steps every sample, and survives a bounce", "[brew][lfo][noise]") {
    LfoMix m{};
    m.sine = 0.0f;
    m.noise = 1.0f;

    SECTION("a new value every sample, not once per cycle") {
        const float a = lfo_mix_value(m, 0.0, 0, 100);
        REQUIRE(lfo_mix_value(m, 0.0, 0, 101) != a);
        REQUIRE(lfo_mix_value(m, 0.0, 0, 102) != a);
        // ...and the phase does not enter into it.
        REQUIRE(lfo_mix_value(m, 0.7, 3, 100) == a);
    }

    SECTION("it is a hash, so the same sample index is the same voltage") {
        REQUIRE(lfo_mix_value(m, 0.0, 0, 987654) == lfo_mix_value(m, 0.0, 0, 987654));
    }

    SECTION("noise and the sample-and-hold do not move together") {
        // A cycle index and a sample index that collided would otherwise hand both
        // hashes the same number, and the two would track each other.
        LfoMix held{};
        held.sine = 0.0f;
        held.random = 1.0f;
        REQUIRE(lfo_mix_value(held, 0.0, 42, 0) != lfo_mix_value(m, 0.0, 0, 42));
    }
}

TEST_CASE("a noisy LFO still bounces identically", "[brew][lfo][noise][safety]") {
    // The reason the noise is a hash of the sample index rather than a generator.
    // Play into a block, versus locate straight to it.
    Lfo played, located;
    for (auto* h : {&played, &located}) {
        set_both(*h, LfoProcessor::kSine, 0.0f);
        set_both(*h, LfoProcessor::kNoise, 1.0f);
    }

    constexpr int kFrames = 256;
    std::vector<float> from_playing;
    for (std::int64_t b = 0; b <= 12; ++b)
        from_playing = at_sample(played, b * kFrames, kFrames);
    REQUIRE(from_playing == at_sample(located, 12 * kFrames, kFrames));
    // ...and it really is noise, not a held level.
    REQUIRE(from_playing.front() != from_playing.back());
}

TEST_CASE("the noise index follows the timeline while the transport rolls",
          "[brew][lfo][noise][safety]") {
    // The claim the bounce test cannot make on its own. An implementation that never
    // re-pinned the index to `position_samples` would replay the same 256 hashes at
    // the top of every block — and would pass a locate-versus-play comparison,
    // because both sides would be replaying them.
    Lfo host;
    set_both(host, LfoProcessor::kSine, 0.0f);
    set_both(host, LfoProcessor::kNoise, 1.0f);
    REQUIRE(at_sample(host, 0, 64) != at_sample(host, 5000, 64));
    // ...and the same position twice is the same noise, which is the other half.
    REQUIRE(at_sample(host, 5000, 64) == at_sample(host, 5000, 64));
}

TEST_CASE("noise keeps hissing with the transport parked", "[brew][lfo][noise]") {
    // A noise source that froze with the playhead would look broken. The index runs
    // free while stopped, and is re-pinned to the timeline the moment it rolls.
    Lfo host;
    set_both(host, LfoProcessor::kSine, 0.0f);
    set_both(host, LfoProcessor::kNoise, 1.0f);
    set_sync(host, SyncMode::transport2);   // isolate the noise from any drift

    Ctx c;
    c.playing = false;
    const auto a = render(host, c, 64, 0);
    const auto b = render(host, c, 64, 0);
    REQUIRE(a != b);
}

TEST_CASE("the two channels do not share a noise stream", "[brew][lfo][noise][stereo]") {
    // Two LFOs with the same Seed are still two LFOs. A hash keyed only on the sample
    // index would put the identical hiss on both cables.
    Lfo host;
    set_both(host, LfoProcessor::kSine, 0.0f);
    set_both(host, LfoProcessor::kNoise, 1.0f);

    audio::Buffer<float> in(2, 64), out(2, 64);
    in.clear();
    out.clear();
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, 64);
    auto ov = out.view();
    format::ProcessContext ctx;
    ctx.sample_rate = kSampleRate;
    ctx.num_samples = 64;
    ctx.is_playing = true;
    ctx.tempo_bpm = kTempo;
    ctx.position_samples = 1000;
    host.process(ov, iv, ctx);

    bool differs = false;
    for (std::size_t n = 0; n < 64; ++n)
        if (out.channel(0)[n] != out.channel(1)[n]) differs = true;
    REQUIRE(differs);
}

// ---------------------------------------------------------------------- offset

TEST_CASE("offset shifts the whole waveform", "[brew][lfo][mix]") {
    LfoMix m{};
    m.sine = 1.0f;
    LfoMix shifted = m;
    shifted.offset = 0.25f;
    for (double p = 0.0; p < 1.0; p += 0.1) {
        CAPTURE(p);
        REQUIRE_THAT(lfo_mix_value(shifted, p, 0),
                     WithinAbs(lfo_mix_value(m, p, 0) + 0.25f, 1e-6));
    }
}

// ------------------------------------------------------------------ frequency

TEST_CASE("Speed times Multiplier is the free rate", "[brew][lfo][free]") {
    REQUIRE(free_hz(2.0, Multiplier::one) == 2.0);
    REQUIRE_THAT(free_hz(2.0, Multiplier::tenth), WithinAbs(0.2, 1e-12));
    REQUIRE(free_hz(2.0, Multiplier::ten) == 20.0);
    REQUIRE(free_hz(2.0, Multiplier::hundred) == 200.0);

    // Clamped at both ends. Zero hertz is a stopped LFO the user cannot restart from
    // the knob; a megahertz LFO is a stepped voltage pretending to be a waveform.
    REQUIRE(free_hz(0.0, Multiplier::one) == kMinFreeHz);
    REQUIRE(free_hz(-5.0, Multiplier::one) == kMinFreeHz);
    REQUIRE(free_hz(40.0, Multiplier::thousand) == kMaxFreeHz);
}

TEST_CASE("the Multiplier knob reaches the DSP", "[brew][lfo][free]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    set_sync(host, SyncMode::free3);
    set_both(host, LfoProcessor::kSpeedHz, 1.0f);

    // At 1 Hz, a quarter of a second in, a saw is a quarter of the way up: -0.5.
    const auto quarter = static_cast<std::int64_t>(kSampleRate / 4);
    REQUIRE_THAT(at_sample(host, quarter, 4).front(), WithinAbs(-0.5f, 1e-4f));

    // ×10 makes that two and a half cycles: the same fraction of a cycle, but only
    // because 2.5 wraps to 0.5. Pick a tenth of a second instead, where ×10 is one
    // whole cycle and ×1 is a tenth of one.
    const auto tenth = static_cast<std::int64_t>(kSampleRate / 10);
    const float at_one = at_sample(host, tenth, 4).front();
    set_both(host, LfoProcessor::kMultiplier,
             static_cast<float>(static_cast<int>(Multiplier::ten)));
    REQUIRE_THAT(at_sample(host, tenth, 4).front(), WithinAbs(-1.0f, 1e-4f));
    REQUIRE(at_one != at_sample(host, tenth, 4).front());
}

TEST_CASE("Beats times Divisor times Triplet is one cycle", "[brew][lfo][tempo]") {
    // The manual's own example: Divisor 1/8, Beats 3 → three eighth notes.
    REQUIRE(cycle_beats(3.0, NoteUnit::eighth, false) == 1.5);
    REQUIRE(cycle_beats(1.0, NoteUnit::quarter, false) == 1.0);
    REQUIRE(cycle_beats(1.0, NoteUnit::whole, false) == 4.0);
    REQUIRE(cycle_beats(1.0, NoteUnit::sixteenth, false) == 0.25);

    // A triplet fits three in the space of two.
    REQUIRE_THAT(cycle_beats(1.0, NoteUnit::quarter, true), WithinAbs(2.0 / 3.0, 1e-12));
    REQUIRE_THAT(cycle_beats(3.0, NoteUnit::quarter, true), WithinAbs(2.0, 1e-12));

    // Fractional Beats is deliberate: it is how a rate is automated through a sweep
    // rather than a staircase.
    REQUIRE(cycle_beats(0.5, NoteUnit::quarter, false) == 0.5);
}

TEST_CASE("the Divisor and Triplet knobs reach the DSP", "[brew][lfo][tempo]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    set_both(host, LfoProcessor::kBeats, 1.0f);

    // One cycle per quarter note: a saw at beat 0.5 is halfway up, i.e. zero.
    REQUIRE_THAT(at_beat(host, 0.5, 4).front(), WithinAbs(0.0f, 1e-4f));

    // One cycle per half note: beat 0.5 is only a quarter of the way up.
    set_both(host, LfoProcessor::kDivisor, as_param(NoteUnit::half));
    REQUIRE_THAT(at_beat(host, 0.5, 4).front(), WithinAbs(-0.5f, 1e-4f));

    // ...and a triplet shortens the cycle to two thirds, so it is further along:
    // 0.5 beats into a 4/3-beat cycle is 0.375 of the way up → 2(0.375) − 1.
    set_both(host, LfoProcessor::kTriplet, 1.0f);
    REQUIRE_THAT(at_beat(host, 0.5, 4).front(), WithinAbs(-0.25f, 1e-4f));
}

// ----------------------------------------------------- phase and the hold agree

TEST_CASE("the sample-and-hold steps where the waveform wraps, not elsewhere",
          "[brew][lfo][random]") {
    // The phase offset shifts the shape. If the hold's cycle index does not take the
    // same offset, the held random value steps in the middle of the visible cycle —
    // the scope shows a waveform whose random component jumps at a point that
    // corresponds to nothing.
    Lfo host;
    set_both(host, LfoProcessor::kSine, 0.0f);
    set_both(host, LfoProcessor::kRandom, 1.0f);
    set_both(host, LfoProcessor::kPhaseDegrees, 180.0f);

    const auto& lfo = lfo_of(host);
    // With a half-cycle offset, the wrap sits at cycles = 0.5. Either side of it the
    // hold must differ; within a half-cycle either side it must not.
    const float before = lfo.value_at_cycles(0, 0.49);
    const float after = lfo.value_at_cycles(0, 0.51);
    REQUIRE(before != after);
    REQUIRE(lfo.value_at_cycles(0, 0.1) == before);
    REQUIRE(lfo.value_at_cycles(0, 0.9) == after);
    // ...and it does *not* step at cycles = 0, where an un-offset index would.
    REQUIRE(lfo.value_at_cycles(0, -0.1) == lfo.value_at_cycles(0, 0.1));
}

// ------------------------------------------------------------------------ swing

TEST_CASE("straight swing leaves the LFO bit-identical", "[brew][lfo][swing]") {
    Lfo host;

    // Swing has a defined identity, and it must be *exact*: a user who never touches
    // the control, and a user who sets it to 50 and back, must render the same file.
    // An `Approx` here would hide a warp that is merely small.
    const auto straight = at_beat(host, 0.37, 512);

    set_both(host, LfoProcessor::kSwingPercent, 50.0f);
    REQUIRE(at_beat(host, 0.37, 512) == straight);

    set_both(host, LfoProcessor::kSwingUnit, 1.0f);
    REQUIRE(at_beat(host, 0.37, 512) == straight);
}

TEST_CASE("swing delays the LFO's second eighth", "[brew][lfo][swing]") {
    Lfo host;
    set_both(host, LfoProcessor::kBeats, 1.0f);

    // The phase the LFO reaches at a sounding beat is the phase the straight LFO
    // would have reached at the *straight* beat that sounding beat stands for. At 66%
    // swing on eighths the swing pair is one beat long, and its midpoint — straight
    // beat 0.5 — is pushed out to sounding beat 0.66.
    const float straight_mid = at_beat(host, 0.5, 1)[0];

    set_both(host, LfoProcessor::kSwingPercent, 66.0f);
    const float swung_early = at_beat(host, 0.5, 1)[0];
    REQUIRE_THAT(at_beat(host, 0.66, 1)[0], WithinAbs(straight_mid, 1e-5f));

    // ...and it is genuinely warped, not merely offset: the sounding beat that used
    // to carry the midpoint no longer does.
    REQUIRE(swung_early != straight_mid);

    // Downbeats are fixed points of the warp — a swung LFO still starts its cycle on
    // the beat, or it would drift against everything else in the project.
    set_both(host, LfoProcessor::kSwingPercent, 50.0f);
    const float straight_downbeat = at_beat(host, 1.0, 1)[0];
    set_both(host, LfoProcessor::kSwingPercent, 66.0f);
    REQUIRE(at_beat(host, 1.0, 1)[0] == straight_downbeat);
}

TEST_CASE("swing moves the sixteenth when asked", "[brew][lfo][swing]") {
    Lfo host;
    set_both(host, LfoProcessor::kBeats, 1.0f);
    set_both(host, LfoProcessor::kSwingPercent, 66.0f);

    // Eighth-swing and sixteenth-swing are different warps of the same timeline. A
    // control that quietly ignored its unit would pass every test above.
    //
    // Beat 0.4 specifically. The two warps agree exactly wherever a position is in
    // the *first* half of both swing pairs — at 0.3 both map through the same
    // `p / 2a` — so a test there passes whether or not the unit is read. 0.4 is past
    // the sixteenth pair's midpoint (0.33) and short of the eighth's (0.66), which is
    // the only region where the two disagree.
    const auto eighths = at_beat(host, 0.4, 256);
    set_both(host, LfoProcessor::kSwingUnit, 1.0f);
    REQUIRE(at_beat(host, 0.4, 256) != eighths);
}

TEST_CASE("swing does nothing while the transport is parked", "[brew][lfo][swing]") {
    // Swing pushes a note late. With the playhead parked there is no note going past,
    // so warping the frozen position would only move the voltage sitting on the
    // cable — a control that changed the output of a stopped transport.
    Lfo swung, straight;
    for (auto* h : {&swung, &straight}) {
        solo(*h, LfoProcessor::kSaw);
        set_sync(*h, SyncMode::transport2);   // parked means parked, with no drift
        set_both(*h, LfoProcessor::kBeats, 1.0f);
    }
    set_both(swung, LfoProcessor::kSwingPercent, 66.0f);

    // Beat 0.4: past the eighth pair's midpoint, so a swung *rolling* LFO would read
    // a different voltage here. Parked, the two must agree exactly.
    REQUIRE(at_beat(swung, 0.4, 64, 0, /*playing=*/false) ==
            at_beat(straight, 0.4, 64, 0, /*playing=*/false));
    // ...and the same position while rolling really does differ, so the assertion
    // above is about the transport rather than about a swing that never applies.
    REQUIRE(at_beat(swung, 0.4, 64, 0, /*playing=*/true) !=
            at_beat(straight, 0.4, 64, 0, /*playing=*/true));
}

TEST_CASE("a hertz-mode LFO ignores swing", "[brew][lfo][swing][free]") {
    Lfo host;
    set_sync(host, SyncMode::free3);
    set_both(host, LfoProcessor::kSpeedHz, 3.0f);

    // Swing subdivides a beat. A plug-in running in hertz has no beats, so a swung
    // hertz rate is just a wrong hertz rate. Ignored, not approximated.
    const auto free = at_sample(host, 12345, 256);
    set_both(host, LfoProcessor::kSwingPercent, 66.0f);
    REQUIRE(at_sample(host, 12345, 256) == free);
    set_both(host, LfoProcessor::kSwingUnit, 1.0f);
    REQUIRE(at_sample(host, 12345, 256) == free);
}

TEST_CASE("a swung LFO still locates and bounces identically", "[brew][lfo][swing]") {
    Lfo host;
    set_both(host, LfoProcessor::kSwingPercent, 62.5f);

    // The whole point of warping the position rather than accumulating a phase: swing
    // costs nothing in determinism. Reaching beat 2.25 by playing through it and by
    // dropping the playhead on it must give the same samples.
    at_beat(host, 1.0, 512);   // play through the region first
    at_beat(host, 2.0, 512);
    const auto played = at_beat(host, 2.25, 128);

    Lfo fresh;
    set_both(fresh, LfoProcessor::kSwingPercent, 62.5f);
    REQUIRE(at_beat(fresh, 2.25, 128) == played);

    // And block size must not matter: two 64-frame blocks reproduce one 128-frame
    // one. Not bit-exactly — the host hands the second block a beat position it
    // computed as `2.25 + bps*64`, and `bps*64 + bps*n` is not the same double as
    // `bps*(64+n)`. That is the host's rounding, not state carried across the
    // boundary, so the claim is agreement to within it.
    Lfo split;
    set_both(split, LfoProcessor::kSwingPercent, 62.5f);
    const double bps = beats_per_sample(kTempo, kSampleRate);
    auto a = at_beat(split, 2.25, 64);
    const auto b = at_beat(split, 2.25 + bps * 64.0, 64);
    a.insert(a.end(), b.begin(), b.end());
    REQUIRE(a.size() == played.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE_THAT(a[i], WithinAbs(played[i], 1e-6f));
}

// ------------------------------------------------------------------------ input

TEST_CASE("the input bus is ignored until a mode asks for it", "[brew][lfo][input]") {
    // Off by default, for the same reason a bypassed generator is silent: a
    // modulation source that read its input by default would scream the first time it
    // was dropped on an audio track.
    REQUIRE(apply_input(InputMode::off, 0.25f, 0.9f) == 0.25f);
    REQUIRE(apply_input(InputMode::add, 0.25f, 0.5f) == 0.75f);
    REQUIRE(apply_input(InputMode::multiply, 0.5f, 0.5f) == 0.25f);
    // Combine is the sum of what add and multiply would each have produced.
    REQUIRE(apply_input(InputMode::combine, 0.5f, 0.5f) == 1.25f);
}

TEST_CASE("the input modes reach the jack", "[brew][lfo][input]") {
    Lfo host;
    // A square at +1 for the first half cycle, so the arithmetic is visible.
    set_both(host, LfoProcessor::kSine, 0.0f);
    set_both(host, LfoProcessor::kSquare, 1.0f);

    auto with_input = [&](InputMode mode, float level) {
        set_both(host, LfoProcessor::kInputMode, as_param(mode));
        audio::Buffer<float> in(2, 8), out(2, 8);
        in.clear();
        out.clear();
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t n = 0; n < 8; ++n) in.channel(c)[n] = level;
        const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, 8);
        auto ov = out.view();
        format::ProcessContext ctx;
        ctx.sample_rate = kSampleRate;
        ctx.num_samples = 8;
        ctx.is_playing = true;
        ctx.tempo_bpm = kTempo;
        host.process(ov, iv, ctx);
        return out.channel(0)[0];
    };

    REQUIRE(with_input(InputMode::off, 0.5f) == 1.0f);        // input ignored
    REQUIRE(with_input(InputMode::multiply, 0.5f) == 0.5f);   // 1 × 0.5
    REQUIRE(with_input(InputMode::add, -0.5f) == 0.5f);       // 1 + (−0.5)
    // Combine: (1 + −0.5) + (1 × −0.5) = 0. The mode that is not just one of the
    // other two wearing a different name.
    REQUIRE(with_input(InputMode::combine, -0.5f) == 0.0f);
}

// ------------------------------------------------------------------- reset by note

TEST_CASE("a note-on restarts the cycle at Phase", "[brew][lfo][midi]") {
    Lfo host;
    solo(host, LfoProcessor::kSaw);
    set_both(host, LfoProcessor::kBeats, 1.0f);
    set_both(host, LfoProcessor::kResetByNote, 1.0f);

    // Without a note the saw at beat 0.5 is halfway up, i.e. zero.
    REQUIRE_THAT(at_beat(host, 0.5, 4).front(), WithinAbs(0.0f, 1e-4f));

    SECTION("the reset lands on the note's own sample, not the block's first") {
        constexpr int kAt = 64;
        midi::MidiBuffer midi;
        auto note = midi::MidiEvent::note_on(0, 60, 100);
        note.sample_offset = kAt;
        midi.add(note);

        Ctx c;
        c.position_beats = 0.5;
        c.position_samples =
            static_cast<std::int64_t>(0.5 / beats_per_sample(kTempo, kSampleRate));
        const auto block = render(host, c, 128, 0, &midi);

        // Before the note the saw is where the timeline put it.
        REQUIRE_THAT(block[0], WithinAbs(0.0f, 1e-4f));
        // At the note it snaps to the trough, and climbs from there.
        REQUIRE_THAT(block[kAt], WithinAbs(-1.0f, 1e-4f));
        REQUIRE(block[kAt + 1] > block[kAt]);
        // ...and it genuinely jumped rather than merely continued.
        REQUIRE(block[kAt] < block[kAt - 1]);
    }

    SECTION("Phase is where it restarts, not zero") {
        set_both(host, LfoProcessor::kPhaseDegrees, 180.0f);
        midi::MidiBuffer midi;
        midi.add(midi::MidiEvent::note_on(0, 60, 100));

        Ctx c;
        c.position_beats = 0.5;
        // A saw restarted at half a cycle sits at zero, rising.
        REQUIRE_THAT(render(host, c, 8, 0, &midi).front(), WithinAbs(0.0f, 1e-4f));
    }

    SECTION("a channel with the toggle off is not retriggered") {
        Lfo only_left;
        solo(only_left, LfoProcessor::kSaw);
        set_both(only_left, LfoProcessor::kBeats, 1.0f);
        only_left.state().set_value(lpid(LfoProcessor::kResetByNote, 0), 1.0f);

        midi::MidiBuffer midi;
        midi.add(midi::MidiEvent::note_on(0, 60, 100));
        Ctx c;
        c.position_beats = 0.5;

        audio::Buffer<float> in(2, 8), out(2, 8);
        in.clear();
        out.clear();
        const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, 8);
        auto ov = out.view();
        format::ProcessContext ctx;
        ctx.sample_rate = kSampleRate;
        ctx.num_samples = 8;
        ctx.is_playing = true;
        ctx.tempo_bpm = kTempo;
        ctx.position_beats = 0.5;
        midi::MidiBuffer out_midi;
        only_left.processor()->process(ov, iv, midi, out_midi, ctx);

        REQUIRE_THAT(out.channel(0)[0], WithinAbs(-1.0f, 1e-4f));   // reset
        REQUIRE_THAT(out.channel(1)[0], WithinAbs(0.0f, 1e-4f));    // untouched
    }

    SECTION("a note-off does not retrigger") {
        midi::MidiBuffer midi;
        midi.add(midi::MidiEvent::note_off(0, 60));
        Ctx c;
        c.position_beats = 0.5;
        REQUIRE_THAT(render(host, c, 8, 0, &midi).front(), WithinAbs(0.0f, 1e-4f));
    }
}

// ----------------------------------------------------------------------- smooth

TEST_CASE("Smooth at zero is a wire", "[brew][lfo][smooth]") {
    Lfo host;
    set_both(host, LfoProcessor::kSquare, 1.0f);
    set_both(host, LfoProcessor::kSine, 0.0f);

    // The default must be bit-exact, not merely close. Everything else in this suite
    // promises an identical bounce; a smoother that leaked a fraction of a sample at
    // zero would quietly cost that promise for every user who never touched it.
    const auto raw = at_beat(host, 0.0, 512);
    set_both(host, LfoProcessor::kSmoothMs, 0.0f);
    REQUIRE(at_beat(host, 0.0, 512) == raw);

    // And the pure accessor agrees with what the block emitted, sample for sample.
    const auto& lfo = lfo_of(host);
    const double bps = beats_per_sample(kTempo, kSampleRate);
    for (int n = 0; n < 512; n += 37)
        REQUIRE(lfo.value_at(bps * n) == raw[static_cast<std::size_t>(n)]);
}

TEST_CASE("positive Smooth limits the slew rate", "[brew][lfo][smooth]") {
    Lfo host;
    set_both(host, LfoProcessor::kSine, 0.0f);
    set_both(host, LfoProcessor::kSquare, 1.0f);   // hard edges to limit
    set_both(host, LfoProcessor::kBeats, 1.0f);
    set_both(host, LfoProcessor::kSmoothMs, 100.0f);

    const auto block = at_beat(host, 0.0, 512);
    // A 100 ms full swing is 2/4800 per sample at most. The unsmoothed square would
    // step 2.0 in one sample.
    for (std::size_t n = 1; n < block.size(); ++n) {
        CAPTURE(n);
        REQUIRE(std::abs(block[n] - block[n - 1]) <=
                2.0f / static_cast<float>(0.1 * kSampleRate) + 1e-6f);
    }
}

TEST_CASE("the two channels smooth independently", "[brew][lfo][smooth][stereo]") {
    // Sharing one filter would make the right channel filter the left's samples.
    Lfo host;
    set_both(host, LfoProcessor::kSine, 0.0f);
    set_both(host, LfoProcessor::kSquare, 1.0f);
    host.state().set_value(lpid(LfoProcessor::kSmoothMs, 0), 500.0f);
    host.state().set_value(lpid(LfoProcessor::kSmoothMs, 1), 0.0f);

    audio::Buffer<float> in(2, 64), out(2, 64);
    in.clear();
    out.clear();
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, 64);
    auto ov = out.view();
    format::ProcessContext ctx;
    ctx.sample_rate = kSampleRate;
    ctx.num_samples = 64;
    ctx.is_playing = true;
    ctx.tempo_bpm = kTempo;
    host.process(ov, iv, ctx);

    REQUIRE(out.channel(1)[0] == 1.0f);   // unsmoothed: straight to the rail
    REQUIRE(out.channel(0)[0] < 1.0f);    // smoothed: still climbing
}

TEST_CASE("a non-zero Smooth makes a locate converge rather than diverge",
          "[brew][lfo][safety]") {
    // `Smooth` is the one control on this plug-in that makes the output depend on
    // how the playhead arrived rather than only on where it is: a smoother carries
    // state between blocks. The README says the dependence is bounded by the
    // smoother's own settling time. This is what says it in code.
    constexpr float kMs = -5.0f;   // a one-pole, ~5 ms to settle a full swing

    Lfo played, located;
    for (Lfo* h : {&played, &located}) {
        set_sync(*h, SyncMode::transport2);
        set_both(*h, LfoProcessor::kBeats, 1.0f);
        set_both(*h, LfoProcessor::kSmoothMs, kMs);
    }

    // One arrives at beat 8 by playing there; the other drops in cold.
    for (int b = 0; b < 8; ++b) (void)at_beat(played, static_cast<double>(b), 512);

    // Twenty settling times' worth of samples, rendered from beat 8 in both hosts.
    constexpr int kFrames = 4800;   // 100 ms at 48 kHz
    const auto a = at_beat(played, 8.0, kFrames);
    const auto b = at_beat(located, 8.0, kFrames);

    // They start apart — the smoother's state is the whole point.
    const float first = std::abs(a[0] - b[0]);
    CHECK(first > 1e-4f);
    // ...and they converge, rather than staying apart or drifting further. The gap
    // is bounded by the smoother's settling time, which is the whole claim.
    const float last = std::abs(a[kFrames - 1] - b[kFrames - 1]);
    CHECK(last < first * 1e-3f);
    CHECK(last < 1e-6f);

    // At zero it is a wire, so the same locate is bit-identical.
    Lfo exact_played, exact_located;
    for (Lfo* h : {&exact_played, &exact_located}) {
        set_sync(*h, SyncMode::transport2);
        set_both(*h, LfoProcessor::kBeats, 1.0f);
    }
    for (int i = 0; i < 8; ++i) (void)at_beat(exact_played, static_cast<double>(i), 512);
    CHECK(at_beat(exact_played, 8.0, 64) == at_beat(exact_located, 8.0, 64));
}
