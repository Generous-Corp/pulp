// Tests for the assembled PulpKit drum-machine Processor.
//
// The kit is one polyphonic Processor that routes a MIDI note to the voice on
// that note and sums every ringing voice. These tests drive the real Processor
// through its MIDI + audio interface and measure numbers off the render, not a
// smoke test:
//   * Different notes select different voices: note 36 renders low and tonal
//     (a bass fundamental under ~80 Hz), note 42 renders short and bright (a
//     high spectral centroid with a sub-200 ms tail), note 49 renders long and
//     bright (a high centroid with a multi-second tail). The three are ordered
//     by both duration and brightness, which is the whole point of a kit.
//   * Two voices played on the same sample do not cut each other: the kick +
//     cymbal sum carries at least as much energy in every band as either alone.
//   * The closed hat chokes the open hat: an open hat rung and then struck with
//     a closed hat collapses to near silence in the window after the closed
//     hat's own tail has died, where an un-choked open hat still rings.
//   * The audio-thread path (process, with note-ons dispatched inside the
//     block) allocates nothing.
//
// Spectral centroid is measured with a small self-contained radix-2 FFT so the
// test depends only on the Processor stack and pulp::signal, no analysis lib.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include "pulp-kit/pulp_kit.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace {

using pulp::examples::PulpKit;
namespace audio = pulp::audio;
namespace format = pulp::format;
namespace midi = pulp::midi;
namespace state = pulp::state;

constexpr double kFs = 48000.0;
constexpr int kBlock = 128;
constexpr double kPi = 3.14159265358979323846;

/// One note-on scheduled at an absolute sample index in the render.
struct Hit {
    int sample;
    int note;
    int velocity;
};

/// A prepared PulpKit driven through its real process() interface. Owns the
/// StateStore the Processor reads its parameters from.
class KitRig {
public:
    KitRig() {
        kit_.set_state_store(&store_);
        kit_.define_parameters(store_);
        format::PrepareContext ctx;
        ctx.sample_rate = kFs;
        ctx.max_buffer_size = kBlock;
        kit_.prepare(ctx);
    }

    PulpKit& kit() noexcept { return kit_; }
    state::StateStore& store() noexcept { return store_; }

    /// Render a single hit with one parameter overridden to `value` first, so a
    /// control's effect can be measured against the default render.
    std::vector<float> render_with(state::ParamID id, float value,
                                   std::vector<Hit> hits, double seconds) {
        store_.set_value(id, value);
        return render(std::move(hits), seconds);
    }

    /// Render `seconds` of the kit's left channel with the given note-ons.
    std::vector<float> render(std::vector<Hit> hits, double seconds) {
        const int total = static_cast<int>(seconds * kFs);
        std::vector<float> out(static_cast<std::size_t>(total), 0.0f);
        std::vector<float> lbuf(kBlock, 0.0f), rbuf(kBlock, 0.0f);
        std::vector<float> zero(kBlock, 0.0f);

        int pos = 0;
        std::size_t hi = 0;
        while (pos < total) {
            const int n = std::min(kBlock, total - pos);
            float* op[2] = {lbuf.data(), rbuf.data()};
            const float* ip[2] = {zero.data(), zero.data()};
            audio::BufferView<float> ov(op, 2, n);
            audio::BufferView<const float> iv(ip, 2, n);
            midi::MidiBuffer min, mout;
            while (hi < hits.size() && hits[hi].sample < pos + n) {
                auto e = midi::MidiEvent::note_on(
                    0, static_cast<uint8_t>(hits[hi].note),
                    static_cast<uint8_t>(hits[hi].velocity));
                e.sample_offset = hits[hi].sample - pos;
                min.add(e);
                ++hi;
            }
            format::ProcessContext pc;
            pc.sample_rate = kFs;
            pc.num_samples = n;
            kit_.process(ov, iv, min, mout, pc);
            for (int i = 0; i < n; ++i)
                out[static_cast<std::size_t>(pos + i)] = lbuf[static_cast<std::size_t>(i)];
            pos += n;
        }
        return out;
    }

private:
    PulpKit kit_;
    state::StateStore store_;
};

double peak_abs(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, std::abs(static_cast<double>(v)));
    return p;
}

double energy(const std::vector<float>& x, int a, int b) {
    double e = 0.0;
    a = std::max(0, a);
    b = std::min(static_cast<int>(x.size()), b);
    for (int i = a; i < b; ++i)
        e += static_cast<double>(x[static_cast<std::size_t>(i)]) *
             static_cast<double>(x[static_cast<std::size_t>(i)]);
    return e;
}

/// -60 dB decay time: last sample above peak/1000, in seconds.
double t60(const std::vector<float>& x) {
    const double pk = peak_abs(x);
    if (pk <= 0.0) return 0.0;
    int last = 0;
    for (int i = 0; i < static_cast<int>(x.size()); ++i)
        if (std::abs(static_cast<double>(x[static_cast<std::size_t>(i)])) > pk / 1000.0)
            last = i;
    return last / kFs;
}

/// Zero-crossing-rate fundamental over the sustained body (10..210 ms).
double fundamental_zc(const std::vector<float>& x) {
    const int a = static_cast<int>(0.010 * kFs);
    const int b = std::min(static_cast<int>(x.size()), static_cast<int>(0.210 * kFs));
    int cross = 0;
    for (int i = a + 1; i < b; ++i) {
        const bool prev = x[static_cast<std::size_t>(i) - 1] <= 0.0f;
        const bool cur = x[static_cast<std::size_t>(i)] <= 0.0f;
        if (prev != cur) ++cross;
    }
    const double secs = static_cast<double>(b - a) / kFs;
    return secs > 0.0 ? cross / (2.0 * secs) : 0.0;
}

/// In-place iterative radix-2 FFT.
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

/// Hann-windowed magnitude spectral centroid over the render's first frames.
double centroid(const std::vector<float>& x) {
    const std::size_t N = 8192;
    std::vector<std::complex<double>> buf(N, {0.0, 0.0});
    const std::size_t m = std::min(N, x.size());
    for (std::size_t n = 0; n < m; ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                              static_cast<double>(N - 1));
        buf[n] = {x[n] * w, 0.0};
    }
    fft(buf);
    double num = 0.0, den = 0.0;
    for (std::size_t k = 1; k < N / 2; ++k) {
        const double mag = std::abs(buf[k]);
        const double f = static_cast<double>(k) * kFs / static_cast<double>(N);
        num += f * mag;
        den += mag;
    }
    return den > 0.0 ? num / den : 0.0;
}

}  // namespace

TEST_CASE("PulpKit routes different notes to different voices", "[kit][pulp-kit][routing]") {
    KitRig rig;
    const auto kick = rig.render({{0, 36, 120}}, 1.0);    // note 36 -> bass drum
    const auto closed = rig.render({{0, 42, 120}}, 0.6);  // note 42 -> closed hat
    const auto cymbal = rig.render({{0, 49, 120}}, 2.5);  // note 49 -> cymbal

    // All three actually made sound.
    REQUIRE(peak_abs(kick) > 0.05);
    REQUIRE(peak_abs(closed) > 0.02);
    REQUIRE(peak_abs(cymbal) > 0.02);

    const double kick_f0 = fundamental_zc(kick);
    const double closed_cent = centroid(closed);
    const double cymbal_cent = centroid(cymbal);
    const double closed_t60 = t60(closed);
    const double cymbal_t60 = t60(cymbal);

    // Note 36 is low and tonal: a bass fundamental well under 80 Hz.
    CHECK(kick_f0 < 80.0);
    CHECK(kick_f0 > 30.0);

    // Note 42 is short and bright: a high centroid and a tail under 200 ms.
    CHECK(closed_cent > 6000.0);
    CHECK(closed_t60 < 0.2);

    // Note 49 is long and bright: a high centroid and a multi-second tail.
    CHECK(cymbal_cent > 6000.0);
    CHECK(cymbal_t60 > 1.0);

    // The kit is discriminated on both axes it claims: the cymbal rings far
    // longer than the closed hat, and both are far brighter than the kick.
    CHECK(cymbal_t60 > closed_t60 * 3.0);
    CHECK(closed_cent > kick_f0 * 20.0);
}

TEST_CASE("PulpKit voices sum without cutting each other", "[kit][pulp-kit][polyphony]") {
    KitRig rig;
    const auto kick = rig.render({{0, 36, 120}}, 2.0);
    const auto cymbal = rig.render({{0, 49, 120}}, 2.0);
    const auto both = rig.render({{0, 36, 120}, {0, 49, 120}}, 2.0);

    // The kick dominates the first 100 ms; the cymbal dominates a late window.
    const int early_b = static_cast<int>(0.1 * kFs);
    const int late_a = static_cast<int>(1.0 * kFs);
    const int late_b = static_cast<int>(1.5 * kFs);

    // Summing the two voices keeps (at least) each voice's own energy in the
    // band it owns -- neither voice is stolen or gated by the other.
    CHECK(energy(both, 0, early_b) >= energy(kick, 0, early_b) * 0.98);
    CHECK(energy(both, late_a, late_b) >= energy(cymbal, late_a, late_b) * 0.98);

    // And the mix is louder than either alone, so they genuinely coexist.
    CHECK(peak_abs(both) > peak_abs(cymbal));
}

TEST_CASE("PulpKit closed hat chokes the open hat", "[kit][pulp-kit][choke]") {
    KitRig rig_open;
    KitRig rig_choked;
    const int choke_at = static_cast<int>(0.2 * kFs);

    const auto open_only = rig_open.render({{0, 46, 120}}, 0.6);  // 46 -> open hat
    const auto choked =
        rig_choked.render({{0, 46, 120}, {choke_at, 42, 120}}, 0.6);  // 42 chokes it

    // Measure a window that starts after the choking closed hat's own tail
    // (T60 ~ 130 ms) has died, so only the open hat's ring is left to compare.
    const int a = choke_at + static_cast<int>(0.14 * kFs);
    const int b = choke_at + static_cast<int>(0.24 * kFs);
    const double e_open = energy(open_only, a, b);
    const double e_choked = energy(choked, a, b);

    REQUIRE(e_open > 0.0);
    // The un-choked open hat is still ringing here; the choked one has collapsed.
    CHECK(e_choked < e_open * 0.1);
}

TEST_CASE("PulpKit audio path allocates nothing", "[kit][pulp-kit][rt-safety]") {
    PulpKit kit;
    state::StateStore store;
    kit.set_state_store(&store);
    kit.define_parameters(store);
    format::PrepareContext ctx;
    ctx.sample_rate = kFs;
    ctx.max_buffer_size = kBlock;
    kit.prepare(ctx);

    // Pre-allocate every buffer the audio path touches, outside the probe.
    std::vector<float> lbuf(kBlock, 0.0f), rbuf(kBlock, 0.0f), zero(kBlock, 0.0f);
    float* op[2] = {lbuf.data(), rbuf.data()};
    const float* ip[2] = {zero.data(), zero.data()};
    audio::BufferView<float> ov(op, 2, kBlock);
    audio::BufferView<const float> iv(ip, 2, kBlock);

    // Two pre-built MIDI buffers: one carrying a rotating note-on, one empty.
    // Reusing them means the probe scope never touches the heap.
    std::vector<midi::MidiBuffer> hit_buffers;
    const int notes[] = {36, 38, 42, 46, 49, 41, 50, 56, 39, 37, 70, 75};
    for (int note : notes) {
        midi::MidiBuffer b;
        b.add(midi::MidiEvent::note_on(0, static_cast<uint8_t>(note), 110));
        hit_buffers.push_back(std::move(b));
    }
    midi::MidiBuffer empty;
    midi::MidiBuffer mout;

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 300; ++block) {
            format::ProcessContext pc;
            pc.sample_rate = kFs;
            pc.num_samples = kBlock;
            midi::MidiBuffer& min =
                (block % 8 == 0) ? hit_buffers[static_cast<std::size_t>((block / 8) %
                                                                        std::size(notes))]
                                 : empty;
            mout.clear();
            kit.process(ov, iv, min, mout, pc);
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

// -- Control surface: every voice exposes four uniform params ----------------

namespace {

using pulp::examples::PulpKitParams;

/// The thirteen voices the kit instantiates, in parameter order, each paired
/// with the id of its Level param (the first of its four). Tune/Decay/Tone
/// follow at +1/+2/+3.
struct VoiceParams {
    const char* name;
    state::ParamID level;
};

const VoiceParams kVoices[] = {
    {"Kick", pulp::examples::kPulpKitKickLevel},
    {"Snare", pulp::examples::kPulpKitSnareLevel},
    {"Low Tom", pulp::examples::kPulpKitLowTomLevel},
    {"Mid Tom", pulp::examples::kPulpKitMidTomLevel},
    {"Hi Tom", pulp::examples::kPulpKitHiTomLevel},
    {"Rim", pulp::examples::kPulpKitRimLevel},
    {"Clave", pulp::examples::kPulpKitClaveLevel},
    {"Clap", pulp::examples::kPulpKitClapLevel},
    {"Closed Hat", pulp::examples::kPulpKitClosedHatLevel},
    {"Open Hat", pulp::examples::kPulpKitOpenHatLevel},
    {"Cymbal", pulp::examples::kPulpKitCymbalLevel},
    {"Cowbell", pulp::examples::kPulpKitCowbellLevel},
    {"Maracas", pulp::examples::kPulpKitMaracasLevel},
};

}  // namespace

TEST_CASE("PulpKit exposes Level/Tune/Decay/Tone for every voice",
          "[kit][pulp-kit][params]") {
    KitRig rig;
    const auto& store = rig.store();

    // Exactly four params per voice (thirteen voices) plus the snare's one extra.
    REQUIRE(store.param_count() ==
            static_cast<std::size_t>(pulp::examples::kPulpKitParamCount));
    REQUIRE(store.param_count() == 13 * 4 + 1);

    for (const auto& v : kVoices) {
        INFO("voice " << v.name);
        const state::ParamInfo* level = store.info(v.level);
        const state::ParamInfo* tune = store.info(v.level + 1);
        const state::ParamInfo* decay = store.info(v.level + 2);
        const state::ParamInfo* tone = store.info(v.level + 3);
        REQUIRE(level);
        REQUIRE(tune);
        REQUIRE(decay);
        REQUIRE(tone);

        // Consistent naming: "<Voice> Level/Tune/Decay/Tone".
        CHECK(level->name == std::string(v.name) + " Level");
        CHECK(tune->name == std::string(v.name) + " Tune");
        CHECK(decay->name == std::string(v.name) + " Decay");
        CHECK(tone->name == std::string(v.name) + " Tone");

        // The four sit in one host group so a host shows them together.
        CHECK(tune->group_id == level->group_id);
        CHECK(decay->group_id == level->group_id);
        CHECK(tone->group_id == level->group_id);

        // Defaults that reproduce today's calibrated sound: Level 100%, Tune 1.0,
        // Decay 50%, Tone 50%.
        CHECK(level->range.default_value == Catch::Approx(100.0f));
        CHECK(tune->range.default_value == Catch::Approx(1.0f));
        CHECK(decay->range.default_value == Catch::Approx(50.0f));
        CHECK(tone->range.default_value == Catch::Approx(50.0f));

        CHECK(level->unit == "%");
        CHECK(tune->unit == "x");
        CHECK(decay->unit == "%");
        CHECK(tone->unit == "%");
    }

    // The snare keeps its tone/snap Balance as a fifth, voice-specific control,
    // inside the snare's group.
    const state::ParamInfo* balance = store.info(pulp::examples::kPulpKitSnareBalance);
    REQUIRE(balance);
    CHECK(balance->name == "Snare Balance");
    CHECK(balance->group_id == store.info(pulp::examples::kPulpKitSnareLevel)->group_id);
}

TEST_CASE("PulpKit per-voice controls each move their own axis",
          "[kit][pulp-kit][controls]") {
    using P = pulp::examples::PulpKitParams;
    const int cymbal = 49;  // note 49 -> cymbal, a long bright voice

    // Tune shifts pitch: the spectral centroid rises with the ratio.
    const double cent_lo =
        centroid(KitRig{}.render_with(P::kPulpKitCymbalTune, 0.7f, {{0, cymbal, 120}}, 2.5));
    const double cent_mid = centroid(KitRig{}.render({{0, cymbal, 120}}, 2.5));
    const double cent_hi =
        centroid(KitRig{}.render_with(P::kPulpKitCymbalTune, 1.5f, {{0, cymbal, 120}}, 2.5));
    CHECK(cent_hi > cent_mid * 1.05);
    CHECK(cent_mid > cent_lo * 1.02);

    // Decay changes the tail: T60 grows monotonically with the knob.
    const double t_short =
        t60(KitRig{}.render_with(P::kPulpKitCymbalDecay, 20.0f, {{0, cymbal, 120}}, 4.0));
    const double t_mid = t60(KitRig{}.render({{0, cymbal, 120}}, 4.0));
    const double t_long =
        t60(KitRig{}.render_with(P::kPulpKitCymbalDecay, 90.0f, {{0, cymbal, 120}}, 4.0));
    CHECK(t_long > t_mid);
    CHECK(t_mid > t_short * 1.3);

    // Tone changes brightness: a higher Tone lifts the centroid, a lower one
    // darkens it -- without moving the default (Tone 50%).
    const double tone_dark =
        centroid(KitRig{}.render_with(P::kPulpKitCymbalTone, 10.0f, {{0, cymbal, 120}}, 2.5));
    const double tone_bright =
        centroid(KitRig{}.render_with(P::kPulpKitCymbalTone, 90.0f, {{0, cymbal, 120}}, 2.5));
    CHECK(tone_bright > cent_mid * 1.05);
    CHECK(tone_dark < tone_bright);

    // Level scales the output peak linearly around 100%.
    const double p_quiet =
        peak_abs(KitRig{}.render_with(P::kPulpKitCymbalLevel, 50.0f, {{0, cymbal, 120}}, 2.5));
    const double p_base = peak_abs(KitRig{}.render({{0, cymbal, 120}}, 2.5));
    const double p_loud =
        peak_abs(KitRig{}.render_with(P::kPulpKitCymbalLevel, 150.0f, {{0, cymbal, 120}}, 2.5));
    CHECK(p_loud > p_base);
    CHECK(p_base > p_quiet);
    // 50% is half the peak of 100%.
    CHECK(p_quiet == Catch::Approx(0.5 * p_base).epsilon(0.02));
}

TEST_CASE("PulpKit tom Decay rides the calibrated tail, not a fixed centre",
          "[kit][pulp-kit][controls]") {
    using P = pulp::examples::PulpKitParams;
    const int low_tom = 41;  // note 41 -> low tom

    // The tom pad is calibrated to a long (~1 s) ring at the default 50% knob.
    // The knob must lengthen and shorten *around* that calibrated tail.
    const double t_default = t60(KitRig{}.render({{0, low_tom, 120}}, 4.0));
    const double t_short =
        t60(KitRig{}.render_with(P::kPulpKitLowTomDecay, 10.0f, {{0, low_tom, 120}}, 4.0));
    const double t_long =
        t60(KitRig{}.render_with(P::kPulpKitLowTomDecay, 95.0f, {{0, low_tom, 120}}, 4.0));

    CHECK(t_default > 0.6);          // default keeps the calibrated long ring
    CHECK(t_short < t_default * 0.6);  // knob down clearly shortens it
    CHECK(t_long > t_default);        // knob up lengthens it
}

TEST_CASE("PulpKit clave keeps its bright ring at the default tune",
          "[kit][pulp-kit][controls]") {
    using P = pulp::examples::PulpKitParams;

    // The clave and rim are one voice; the clave is lifted to a woodier ring by
    // a fixed tune ratio. At the default Clave Tune (1.0) that lift must survive,
    // so the clave (note 75) is clearly brighter than the rim (note 37).
    const double rim_cent = centroid(KitRig{}.render({{0, 37, 120}}, 0.3));
    const double clave_cent = centroid(KitRig{}.render({{0, 75, 120}}, 0.3));
    CHECK(clave_cent > rim_cent * 1.2);

    // And the Clave Tune knob still moves the ring around that lifted default.
    const double lo =
        centroid(KitRig{}.render_with(P::kPulpKitClaveTune, 0.7f, {{0, 75, 120}}, 0.3));
    const double hi =
        centroid(KitRig{}.render_with(P::kPulpKitClaveTune, 1.4f, {{0, 75, 120}}, 0.3));
    CHECK(hi > clave_cent);
    CHECK(clave_cent > lo);
}

TEST_CASE("PulpKit snare default holds its calibrated noise-dominated balance",
          "[kit][pulp-kit][controls]") {
    using P = pulp::examples::PulpKitParams;
    const int snare = 38;

    // The snare is calibrated noise-dominated (balance 0.70), and the Balance
    // knob base-centres on that so 50% reproduces it. If the knob instead
    // centred on 0.50 -- the defect this guards -- the default would render
    // more tonal. Under base-centring, knob 30% maps to balance 0.50, so the
    // default (knob 50%) must be clearly noisier, i.e. brighter, than knob 30%.
    const double c_default = centroid(KitRig{}.render({{0, snare, 120}}, 0.4));
    const double c_middled =
        centroid(KitRig{}.render_with(P::kPulpKitSnareBalance, 30.0f, {{0, snare, 120}}, 0.4));
    CHECK(c_default > c_middled * 1.05);
}
