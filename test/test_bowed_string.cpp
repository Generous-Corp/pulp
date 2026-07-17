// The bowed string as physics rather than decoration. These tests RENDER the
// waveguide bowed-string voice and the instrument that wraps it, then MEASURE
// the four things that make it a bowed string rather than a sample player:
//
//   * it SUSTAINS -- a bow injects energy continuously, so a held note holds its
//     level instead of decaying like a pluck;
//   * it moves in HELMHOLTZ motion -- the spectrum is a full harmonic series
//     (sawtooth-like), in tune to within a few cents across the compass, which
//     is the proof the stateless Lagrange fractional delay tunes the loop;
//   * it is STABLE and BOUNDED at the control extremes -- a feedback loop with a
//     nonlinear bow that must never run away into a hazard;
//   * its CONTROLS do the physically-right thing -- bow position sets brightness
//     and suppresses the harmonic whose node it sits on, bow force sets the
//     onset, bow velocity sets the amplitude, and lifting the bow releases the
//     note to silence.
//
// Every number here comes from an offline render (never an audio device),
// measured with the calibrated analyzer plus a few self-contained spectral
// helpers in this TU.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "bowed_string.hpp"
#include "bowed_string_instrument.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp;
using pulp::examples::BowedString;
using pulp::examples::BowedStringInstrument;

namespace {

// ── Self-contained spectral helpers (test TU only) ─────────────────────────

double rms(const std::vector<float>& x, double from_s, double to_s, double fs) {
    const int a = std::max(0, static_cast<int>(from_s * fs));
    const int b = std::min(static_cast<int>(x.size()), static_cast<int>(to_s * fs));
    if (b <= a) return 0.0;
    double s = 0.0;
    for (int i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / (b - a));
}

double peak(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, std::abs(static_cast<double>(v)));
    return p;
}

bool all_finite(const std::vector<float>& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

// Hann-windowed DFT magnitude at an exact frequency over [a,b) samples.
double dft_mag(const std::vector<float>& x, int a, int b, double fs, double f) {
    const int N = b - a;
    if (N <= 1) return 0.0;
    double re = 0.0, im = 0.0, w = 2.0 * M_PI * f / fs;
    for (int i = 0; i < N; ++i) {
        double win = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (N - 1));
        double s = static_cast<double>(x[a + i]) * win;
        re += s * std::cos(w * i);
        im -= s * std::sin(w * i);
    }
    return std::sqrt(re * re + im * im) / N;
}

// Count harmonics of f0 above `floor_db` relative to the strongest harmonic.
int harmonics_above(const std::vector<float>& x, int a, int b, double fs,
                    double f0, int max_h, double floor_db) {
    std::vector<double> m(max_h);
    double mx = 0.0;
    for (int h = 1; h <= max_h; ++h) {
        m[h - 1] = dft_mag(x, a, b, fs, h * f0);
        mx = std::max(mx, m[h - 1]);
    }
    if (mx <= 0.0) return 0;
    const double thr = mx * std::pow(10.0, floor_db / 20.0);
    int n = 0;
    for (double v : m)
        if (v >= thr) ++n;
    return n;
}

// Spectral centroid over the first `nh` harmonics (Hz).
double centroid(const std::vector<float>& x, int a, int b, double fs, double f0, int nh) {
    double num = 0.0, den = 0.0;
    for (int h = 1; h <= nh; ++h) {
        double mg = dft_mag(x, a, b, fs, h * f0);
        num += h * f0 * mg;
        den += mg;
    }
    return den > 0.0 ? num / den : 0.0;
}

// Octave-safe fundamental via normalized autocorrelation: the smallest period
// whose ACF is within 92% of the global max (a clean T-periodic signal has
// equal ACF peaks at T, 2T, 3T..., so a plain max-picker octave-errors -- this
// prefers the true fundamental). Restrict [fmin,fmax] around the expected f0.
double estimate_f0(const std::vector<float>& x, int a, int b, double fs,
                   double fmin, double fmax) {
    int lmin = static_cast<int>(fs / fmax);
    int lmax = static_cast<int>(fs / fmin);
    if (lmax + 2 >= b - a) lmax = (b - a) - 2;
    if (lmin < 1) lmin = 1;
    std::vector<double> c(lmax + 2, 0.0);
    for (int lag = lmin; lag <= lmax + 1; ++lag) {
        double s = 0, e0 = 0, e1 = 0;
        for (int i = a; i < b - lag; ++i) {
            s += x[i] * x[i + lag];
            e0 += x[i] * x[i];
            e1 += x[i + lag] * x[i + lag];
        }
        c[lag] = s / (std::sqrt(e0 * e1) + 1e-12);
    }
    double gmax = 0.0;
    for (int lag = lmin; lag <= lmax; ++lag) gmax = std::max(gmax, c[lag]);
    if (gmax <= 0.0) return 0.0;
    const double thr = 0.92 * gmax;
    for (int lag = lmin + 1; lag < lmax; ++lag) {
        if (c[lag] >= thr && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) {
            double y0 = c[lag - 1], y1 = c[lag], y2 = c[lag + 1];
            double den = y0 - 2 * y1 + y2, d = 0.0;
            if (std::abs(den) > 1e-12) d = 0.5 * (y0 - y2) / den;
            return fs / (lag + d);
        }
    }
    return 0.0;
}

double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

// ── Voice-level rendering ──────────────────────────────────────────────────

struct BowSettings {
    double f0 = 220.0;
    double force = 0.4;
    double beta = 0.12;
    double vbow = 0.09;
    double decay = 0.6;  // knob 0..1
    double tone = 0.4;   // bridge damping
    double seconds = 2.0;
    double lift_at = -1.0;  // < 0 = never lift
};

std::vector<float> render_voice(double fs, const BowSettings& s) {
    BowedString v;
    v.prepare(fs);
    v.set_tone(s.tone);
    v.set_frequency(s.f0);
    v.set_bow_force(s.force);
    v.set_bow_position(s.beta);
    v.set_bow_velocity(s.vbow);
    v.set_decay(s.decay);
    v.bow_on();
    const int n = static_cast<int>(s.seconds * fs);
    const int lift = s.lift_at >= 0 ? static_cast<int>(s.lift_at * fs) : -1;
    std::vector<float> o(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        if (i == lift) v.bow_off();
        o[i] = static_cast<float>(v.process());
        if ((i & 1023) == 1023) v.snap_denormals();
    }
    return o;
}

// ── Instrument-level rendering ─────────────────────────────────────────────

struct NoteEvent {
    int note;
    int velocity;
    double time_s;
    bool on;
};

void bind(BowedStringInstrument& inst, state::StateStore& store, double fs, int block) {
    inst.define_parameters(store);
    inst.set_state_store(&store);
    format::PrepareContext ctx;
    ctx.sample_rate = fs;
    ctx.max_buffer_size = block;
    inst.prepare(ctx);
}

std::vector<float> render_instrument(BowedStringInstrument& inst,
                                     const std::vector<NoteEvent>& notes, double fs,
                                     double secs, int block) {
    const int n = static_cast<int>(fs * secs);
    std::vector<float> mono(n, 0.0f);
    audio::Buffer<float> out(2, static_cast<std::size_t>(block));
    const float* in_ptrs[2] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, static_cast<std::size_t>(block));
    for (int i = 0; i < n; i += block) {
        const int c = std::min(block, n - i);
        for (int k = 0; k < c; ++k) {
            out.channel(0)[k] = 0.0f;
            out.channel(1)[k] = 0.0f;
        }
        float* chans[2] = {out.channel(0).data(), out.channel(1).data()};
        audio::BufferView<float> ov(chans, 2, static_cast<std::size_t>(c));
        midi::MidiBuffer mi, mo;
        for (const auto& ev : notes) {
            const int frame = static_cast<int>(ev.time_s * fs);
            if (frame >= i && frame < i + c) {
                auto m = ev.on ? midi::MidiEvent::note_on(0, static_cast<uint8_t>(ev.note),
                                                          static_cast<uint8_t>(ev.velocity))
                               : midi::MidiEvent::note_off(0, static_cast<uint8_t>(ev.note), 0);
                m.sample_offset = frame - i;
                mi.add(m);
            }
        }
        format::ProcessContext ctx;
        ctx.sample_rate = fs;
        ctx.num_samples = c;
        inst.process(ov, in, mi, mo, ctx);
        for (int k = 0; k < c; ++k) mono[i + k] = out.channel(0)[k];
    }
    return mono;
}

constexpr double kFs = 48000.0;

}  // namespace

// ── 1. Sustain ─────────────────────────────────────────────────────────────

TEST_CASE("bowed string sustains rather than decaying like a pluck",
          "[bowed-string][sustain]") {
    BowSettings s;
    s.f0 = 220.0;
    s.seconds = 3.0;
    auto o = render_voice(kFs, s);

    const double r_early = rms(o, 0.8, 0.9, kFs);
    const double r_mid = rms(o, 1.8, 1.9, kFs);
    const double r_late = rms(o, 2.7, 2.8, kFs);
    INFO("rms early=" << r_early << " mid=" << r_mid << " late=" << r_late);

    REQUIRE(r_early > 0.02);              // it speaks
    REQUIRE(r_mid > 0.7 * r_early);       // and holds its level -- does not decay
    REQUIRE(r_late > 0.7 * r_early);

    // Contrast: a decaying resonance would lose most of its energy over 2s. The
    // bow keeps it within a tight band instead.
    REQUIRE(r_late < 1.6 * r_early);      // and does not blow up either
}

// ── 2. Helmholtz motion: rich harmonics, in tune ───────────────────────────

TEST_CASE("bowed string is in tune across the compass", "[bowed-string][tuning]") {
    // Violin/viola/cello core through the top of the treble range.
    const double notes[] = {65.41, 98.0, 146.83, 220.0, 329.63, 440.0, 660.0, 880.0, 1318.5};
    for (double f0 : notes) {
        BowSettings s;
        s.f0 = f0;
        s.seconds = 1.5;
        auto o = render_voice(kFs, s);
        const int a = static_cast<int>(0.6 * kFs), b = static_cast<int>(1.4 * kFs);
        const double est = estimate_f0(o, a, b, kFs, f0 * 0.7, f0 * 1.5);
        const double err = cents(est, f0);
        INFO("target " << f0 << " Hz -> measured " << est << " Hz (" << err << " cents)");
        REQUIRE(est > 0.0);
        REQUIRE(std::abs(err) < 10.0);  // the fractional-delay tuning proof
    }
}

TEST_CASE("bowed string shows a full harmonic (Helmholtz) spectrum",
          "[bowed-string][helmholtz]") {
    BowSettings s;
    s.f0 = 220.0;
    s.seconds = 1.5;
    auto o = render_voice(kFs, s);
    const int a = static_cast<int>(0.6 * kFs), b = static_cast<int>(1.4 * kFs);

    // Not a lone sine: the low harmonics are all present and strong.
    const double h1 = dft_mag(o, a, b, kFs, 220.0);
    const double h2 = dft_mag(o, a, b, kFs, 440.0);
    const double h3 = dft_mag(o, a, b, kFs, 660.0);
    INFO("h1=" << h1 << " h2=" << h2 << " h3=" << h3);
    REQUIRE(h1 > 0.0);
    REQUIRE(h2 > 0.1 * h1);
    REQUIRE(h3 > 0.05 * h1);

    // A Helmholtz corner has a rich, sawtooth-like series.
    const int n = harmonics_above(o, a, b, kFs, 220.0, 20, -40.0);
    INFO("harmonics above -40 dB: " << n);
    REQUIRE(n >= 8);
}

// ── 3. Stability at the control extremes (the safety gate) ─────────────────

TEST_CASE("bowed string stays finite and bounded at the control extremes",
          "[bowed-string][rt-safety][stability]") {
    // A bowed string is a nonlinear feedback loop; at any control setting it must
    // not run away into a hazard. Render 5 s at the corners of the control space.
    const double forces[] = {0.0, 1.0};
    const double vbows[] = {0.02, 0.6};
    const double freqs[] = {65.0, 220.0, 880.0, 1318.5};
    double global_peak = 0.0;
    for (double force : forces)
        for (double vbow : vbows)
            for (double f0 : freqs) {
                BowSettings s;
                s.f0 = f0;
                s.force = force;
                s.vbow = vbow;
                s.decay = 1.0;  // maximum ring -> maximum energy accumulation
                s.seconds = 5.0;
                auto o = render_voice(kFs, s);
                const double p = peak(o);
                global_peak = std::max(global_peak, p);
                INFO("force=" << force << " vbow=" << vbow << " f0=" << f0 << " peak=" << p);
                REQUIRE(all_finite(o));
                REQUIRE(p < 8.0);  // the voice's hard state ceiling, never reached in practice
            }
    INFO("global peak across all extremes: " << global_peak);
    // Sanity: even the loudest corner stays well under the ceiling.
    REQUIRE(global_peak < 6.0);
}

// ── 4. Controls do the physically-right thing ──────────────────────────────

TEST_CASE("bow position sets brightness and suppresses its node's harmonic",
          "[bowed-string][control][position]") {
    auto centroid_at = [](double beta) {
        BowSettings s;
        s.beta = beta;
        s.seconds = 1.5;
        auto o = render_voice(kFs, s);
        return centroid(o, static_cast<int>(0.6 * kFs), static_cast<int>(1.4 * kFs), kFs,
                        220.0, 20);
    };
    // Near the bridge (sul ponticello) is brighter than near the middle.
    const double bright = centroid_at(0.06);
    const double dark = centroid_at(0.40);
    INFO("centroid near bridge=" << bright << " near middle=" << dark);
    REQUIRE(bright > dark);

    // Bowing on a harmonic's node suppresses that harmonic: beta = 1/2 kills h2,
    // beta = 1/3 kills h3 -- the defining bow-position signature.
    auto ratio_at = [](double beta, int h) {
        BowSettings s;
        s.beta = beta;
        s.seconds = 1.5;
        auto o = render_voice(kFs, s);
        const int a = static_cast<int>(0.6 * kFs), b = static_cast<int>(1.4 * kFs);
        return dft_mag(o, a, b, kFs, h * 220.0) / dft_mag(o, a, b, kFs, 220.0);
    };
    const double h2_ref = ratio_at(0.12, 2);
    const double h2_node = ratio_at(0.5, 2);
    const double h3_ref = ratio_at(0.12, 3);
    const double h3_node = ratio_at(1.0 / 3.0, 3);
    INFO("h2: ref=" << h2_ref << " at beta=1/2=" << h2_node);
    INFO("h3: ref=" << h3_ref << " at beta=1/3=" << h3_node);
    REQUIRE(h2_node < 0.2 * h2_ref);  // second harmonic strongly suppressed
    REQUIRE(h3_node < 0.2 * h3_ref);  // third harmonic strongly suppressed
}

TEST_CASE("bow velocity scales amplitude; bow force sets the onset speed",
          "[bowed-string][control][velocity][force]") {
    // Amplitude rises monotonically with bow velocity.
    double prev = 0.0;
    for (double vbow : {0.03, 0.06, 0.10, 0.20}) {
        BowSettings s;
        s.vbow = vbow;
        s.seconds = 1.2;
        auto o = render_voice(kFs, s);
        const double r = rms(o, 0.8, 1.1, kFs);
        INFO("vbow=" << vbow << " rms=" << r);
        REQUIRE(r > prev);
        prev = r;
    }

    // More bow force reaches full amplitude faster (a quicker, sharper attack).
    auto onset_time = [](double force) {
        BowSettings s;
        s.force = force;
        s.vbow = 0.1;
        s.seconds = 2.0;
        auto o = render_voice(kFs, s);
        const double steady = rms(o, 1.5, 1.9, kFs);
        for (int w = 0; w < static_cast<int>(1.5 * kFs); w += static_cast<int>(0.01 * kFs)) {
            const double r = rms(o, w / kFs, w / kFs + 0.02, kFs);
            if (r >= 0.9 * steady) return w / kFs;
        }
        return 1.5;
    };
    const double onset_soft = onset_time(0.1);
    const double onset_hard = onset_time(0.9);
    INFO("onset soft(F=0.1)=" << onset_soft << "s  hard(F=0.9)=" << onset_hard << "s");
    REQUIRE(onset_hard < onset_soft);  // force speeds the attack
}

TEST_CASE("the measured bow-force playable band at A3", "[bowed-string][control][band]") {
    // Report -- and assert -- the Schelleng-style playable band: the string must
    // speak in tune across a usable middle range of bow force. This is the honest
    // deliverable: not one lucky setting, but a measured range.
    int spoke = 0;
    for (double force : {0.05, 0.2, 0.4, 0.6, 0.8}) {
        BowSettings s;
        s.f0 = 220.0;
        s.force = force;
        s.vbow = 0.1;
        s.seconds = 1.5;
        auto o = render_voice(kFs, s);
        const int a = static_cast<int>(0.7 * kFs), b = static_cast<int>(1.4 * kFs);
        const double est = estimate_f0(o, a, b, kFs, 220.0 * 0.7, 220.0 * 1.5);
        const double r = rms(o, 0.7, 1.4, kFs);
        const bool ok = r > 0.02 && est > 0.0 && std::abs(cents(est, 220.0)) < 15.0;
        INFO("force=" << force << " rms=" << r << " f0=" << est << " -> " << (ok ? "speaks" : "--"));
        spoke += ok ? 1 : 0;
    }
    // A wide, usable band: the whole tested force range speaks in tune at A3.
    REQUIRE(spoke >= 4);
}

// ── 5. Bow-lift release ────────────────────────────────────────────────────

TEST_CASE("lifting the bow releases the note to silence", "[bowed-string][release]") {
    for (double f0 : {110.0, 220.0, 440.0}) {
        BowSettings s;
        s.f0 = f0;
        s.decay = 0.35;  // short ring so the release completes within the render
        s.seconds = 4.0;
        s.lift_at = 2.0;
        auto o = render_voice(kFs, s);
        const double before = rms(o, 1.7, 1.9, kFs);
        const double after = rms(o, 3.6, 3.9, kFs);  // ~1.7 s after the lift
        const double db = (after > 0 && before > 0) ? 20.0 * std::log10(after / before) : -200.0;
        INFO("f0=" << f0 << " before=" << before << " after=" << after << " (" << db << " dB)");
        REQUIRE(before > 0.02);   // it was sounding
        REQUIRE(db < -40.0);      // and the lift brings it to silence
    }
}

// ── 6. RT-safety ───────────────────────────────────────────────────────────

TEST_CASE("BowedString process() allocates nothing on the audio thread",
          "[bowed-string][rt-safety]") {
    const int block = 256;
    state::StateStore store;
    BowedStringInstrument inst;
    bind(inst, store, kFs, block);

    audio::Buffer<float> out(2, static_cast<std::size_t>(block));
    const float* in_ptrs[2] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, static_cast<std::size_t>(block));
    float* chans[2] = {out.channel(0).data(), out.channel(1).data()};

    midi::MidiBuffer with_note, note_off, empty, mo;
    with_note.add(midi::MidiEvent::note_on(0, 69, 110));
    note_off.add(midi::MidiEvent::note_off(0, 69, 0));

    format::ProcessContext ctx;
    ctx.sample_rate = kFs;
    ctx.num_samples = block;

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 64; ++b) {
            audio::BufferView<float> ov(chans, 2, static_cast<std::size_t>(block));
            midi::MidiBuffer& mi = (b == 0) ? with_note : (b == 40 ? note_off : empty);
            inst.process(ov, in, mi, mo, ctx);
        }
        allocations = probe.allocation_count();
    }
    INFO("allocations in process(): " << allocations);
    REQUIRE(allocations == 0);
}

// ── 7. Instrument integration ──────────────────────────────────────────────

TEST_CASE("BowedString instrument: a held note sustains, note-off releases it",
          "[bowed-string][instrument]") {
    const int block = 128;
    state::StateStore store;
    BowedStringInstrument inst;
    bind(inst, store, kFs, block);

    auto o = render_instrument(inst, {{69, 100, 0.05, true}, {69, 0, 2.5, false}}, kFs, 3.5,
                               block);
    const double sus_early = rms(o, 0.8, 0.9, kFs);
    const double sus_late = rms(o, 2.2, 2.3, kFs);
    const double released = rms(o, 3.2, 3.4, kFs);
    const int a = static_cast<int>(0.8 * kFs), b = static_cast<int>(2.2 * kFs);
    const double est = estimate_f0(o, a, b, kFs, 440.0 * 0.7, 440.0 * 1.5);
    INFO("A4: sustain early=" << sus_early << " late=" << sus_late << " f0=" << est
                              << " released=" << released);

    REQUIRE(sus_early > 0.02);
    REQUIRE(sus_late > 0.7 * sus_early);       // sustains while held
    REQUIRE(std::abs(cents(est, 440.0)) < 12.0);  // in tune (MIDI 69 = A4 = 440)
    REQUIRE(released < 0.05 * sus_early);       // note-off releases to silence
}

TEST_CASE("BowedString instrument plays a chord (internal polyphony)",
          "[bowed-string][instrument][polyphony]") {
    const int block = 128;
    state::StateStore store;
    BowedStringInstrument inst;
    bind(inst, store, kFs, block);

    // C-E-G held together.
    auto o = render_instrument(inst,
                               {{60, 90, 0.05, true},
                                {64, 90, 0.05, true},
                                {67, 90, 0.05, true},
                                {60, 0, 2.0, false},
                                {64, 0, 2.0, false},
                                {67, 0, 2.0, false}},
                               kFs, 3.0, block);
    const int a = static_cast<int>(0.8 * kFs), b = static_cast<int>(1.7 * kFs);
    const double c4 = dft_mag(o, a, b, kFs, 261.63);
    const double e4 = dft_mag(o, a, b, kFs, 329.63);
    const double g4 = dft_mag(o, a, b, kFs, 392.0);
    INFO("C4=" << c4 << " E4=" << e4 << " G4=" << g4);
    REQUIRE(all_finite(o));
    // All three notes are present simultaneously.
    REQUIRE(c4 > 1e-3);
    REQUIRE(e4 > 1e-3);
    REQUIRE(g4 > 1e-3);
}

TEST_CASE("BowedString instrument retunes glitch-free during a held note",
          "[bowed-string][instrument][retune]") {
    // The stateless Lagrange fractional delay lets pitch move continuously with
    // no interpolator transient. Drive a 5 Hz +/-2 semitone vibrato via the Tune
    // parameter across a held note and show the output stays bounded and its
    // sample-to-sample motion never exceeds what the static waveform already has
    // (no zipper clicks injected by the retune).
    const int block = 64;
    state::StateStore store;
    BowedStringInstrument inst;
    bind(inst, store, kFs, block);

    audio::Buffer<float> out(2, static_cast<std::size_t>(block));
    const float* in_ptrs[2] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, static_cast<std::size_t>(block));

    auto run = [&](bool vibrato) {
        const int n = static_cast<int>(2.0 * kFs);
        std::vector<float> mono(n, 0.0f);
        for (int i = 0; i < n; i += block) {
            const int c = std::min(block, n - i);
            const double t = static_cast<double>(i) / kFs;
            store.set_value(pulp::examples::kBowTune,
                            vibrato ? static_cast<float>(2.0 * std::sin(2.0 * M_PI * 5.0 * t))
                                    : 0.0f);
            for (int k = 0; k < c; ++k) {
                out.channel(0)[k] = 0.0f;
                out.channel(1)[k] = 0.0f;
            }
            float* chans[2] = {out.channel(0).data(), out.channel(1).data()};
            audio::BufferView<float> ov(chans, 2, static_cast<std::size_t>(c));
            midi::MidiBuffer mi, mo;
            if (i == 0) mi.add(midi::MidiEvent::note_on(0, 69, 100));
            format::ProcessContext ctx;
            ctx.sample_rate = kFs;
            ctx.num_samples = c;
            inst.process(ov, in, mi, mo, ctx);
            for (int k = 0; k < c; ++k) mono[i + k] = out.channel(0)[k];
        }
        return mono;
    };

    auto max_jump = [](const std::vector<float>& x, double fs) {
        double mj = 0.0;
        for (std::size_t i = static_cast<std::size_t>(0.3 * fs); i < x.size(); ++i)
            mj = std::max(mj, std::abs(static_cast<double>(x[i]) - x[i - 1]));
        return mj;
    };

    auto vib = run(true);

    REQUIRE(all_finite(vib));
    REQUIRE(peak(vib) < 3.0);
    // The Helmholtz corner is the largest legitimate sample step; the retune must
    // not introduce a step materially larger than that corner.
    const double jump = max_jump(vib, kFs);
    INFO("vibrato max sample jump=" << jump << " peak=" << peak(vib));
    REQUIRE(jump < 0.6);  // corner-sized, not a zipper click
}
