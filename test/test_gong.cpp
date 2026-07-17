// Gong / tam-tam: a dense inharmonic thin-plate modal cloud plus a bounded,
// amplitude-dependent nonlinear bloom. These tests RENDER and MEASURE the model
// and settle the section-4.3 GPU question with a CPU coupling-cost bench.
//
// The claims each test pins down:
//   * The linear base is a DENSE INHARMONIC cloud (hundreds of modes, not a
//     harmonic series) that rings for seconds.
//   * The bloom is REAL and AMPLITUDE-DEPENDENT: a hard strike's spectral
//     centroid lifts far more than a soft strike's, and the linear control
//     (bloom off) shows the lift is the nonlinearity, not the mode set. The
//     amplitude dependence is the FFT-test signature no spectral pipeline holds.
//   * The bloom is BOUNDED: the coupling conserves modal energy exactly and the
//     T60 dissipates, so a max-everything strike stays finite over 10 s.
//   * The bloom=0 render matches ModalBankT mode-for-mode.
//   * A note lands at the pitch it names; process() is allocation-free.
//   * The coupling cost bench reports how many coupled modes / triads run at
//     what fraction of one core -- the data that settles the GPU question.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "support/modal_analysis.hpp"

#include "gong_instrument.hpp"
#include "gong_plate.hpp"
#include "gong_voice.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/modal_bank.hpp>
#include <pulp/state/store.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace pulp;
using pulp::examples::generate_plate_spectrum;
using pulp::examples::GongPlate;
using pulp::examples::PlateSpectrum;
using namespace pulp::test::audio;

namespace {

constexpr double kFs = 48000.0;

// Drive a GongPlate with an area-normalized raised-cosine strike and render
// `secs` seconds. `amp` is the pre-normalization strike gain (loudness), so the
// injected energy is set by amp and independent of contact length.
std::vector<float> strike_plate(GongPlate& plate, double secs, float amp, int contact) {
    const auto total = static_cast<std::size_t>(secs * kFs);
    std::vector<float> out(total, 0.0f), in(total, 0.0f);
    const float area = contact > 1 ? 2.0f / static_cast<float>(contact) : 1.0f;
    for (int i = 0; i < contact && i < static_cast<int>(total); ++i)
        in[static_cast<std::size_t>(i)] =
            amp * area * 0.5f * (1.0f - std::cos(6.283185307f * i / contact));
    plate.reset();
    const int block = 512;
    for (std::size_t i = 0; i < total; i += block) {
        const int n = static_cast<int>(std::min<std::size_t>(block, total - i));
        plate.render(in.data() + i, out.data() + i, n);
    }
    return out;
}

// Spectral centroid of a Hann-windowed frame centered at time `t`.
double centroid_at(const std::vector<float>& x, double t, signal::FftT<float>& fft, int N) {
    std::vector<float> win(static_cast<std::size_t>(N), 0.0f);
    const long half = N / 2;
    for (int i = 0; i < N; ++i) {
        const long idx = static_cast<long>(t * kFs) - half + i;
        const float s = (idx >= 0 && idx < static_cast<long>(x.size()))
                            ? x[static_cast<std::size_t>(idx)] : 0.0f;
        win[static_cast<std::size_t>(i)] = s * 0.5f * (1.0f - std::cos(6.283185307f * i / N));
    }
    std::vector<std::complex<float>> freq(static_cast<std::size_t>(N));
    fft.forward_real(win.data(), freq.data());
    std::vector<float> mag(static_cast<std::size_t>(N / 2));
    fft.magnitude(freq.data(), mag.data(), N / 2);
    double num = 0.0, den = 0.0;
    for (int k = 1; k < N / 2; ++k) {
        const double f = static_cast<double>(k) * kFs / N;
        num += f * mag[static_cast<std::size_t>(k)];
        den += mag[static_cast<std::size_t>(k)];
    }
    return den > 0.0 ? num / den : 0.0;
}

double windowed_rms(const std::vector<float>& x, std::size_t start, int W) {
    double s = 0.0;
    int n = 0;
    for (int k = 0; k < W && start + k < x.size(); ++k) {
        const double v = x[start + static_cast<std::size_t>(k)];
        s += v * v;
        ++n;
    }
    return n > 0 ? std::sqrt(s / n) : 0.0;
}

}  // namespace

TEST_CASE("gong linear base is a dense inharmonic cloud that rings for seconds",
          "[signal][gong]") {
    const PlateSpectrum spec = generate_plate_spectrum(55.0);

    // A dense cloud: far more than the ~150-mode floor for a convincing shimmer.
    INFO("generated modes: " << spec.modes.size());
    REQUIRE(spec.modes.size() >= 150);

    // Inharmonic, not a harmonic series: the low free-plate ratios are the
    // dispersive thin-plate values (1 : 1.73 : 2.33 : ...), never integer
    // multiples of the fundamental.
    const double f0 = spec.modes.front().freq_hz;
    REQUIRE(f0 == Catch::Approx(55.0).margin(1.0));
    const double r1 = spec.modes[1].freq_hz / f0;
    const double r2 = spec.modes[2].freq_hz / f0;
    INFO("first ratios: " << r1 << ", " << r2);
    REQUIRE(r1 == Catch::Approx(1.73).margin(0.15));   // (0,1)
    REQUIRE(r2 == Catch::Approx(2.33).margin(0.20));   // (3,0)
    // Explicitly NOT harmonic.
    REQUIRE(std::abs(r1 - 2.0) > 0.15);

    // Rings for seconds: render the linear cloud (bloom off) and confirm the
    // fundamental is present and the tail is still audible at 2 s.
    GongPlate plate;
    plate.prepare(kFs, static_cast<int>(spec.modes.size()), 16, 512);
    plate.set_modes(spec.modes, spec.out_weights);
    plate.set_bloom(0.0);
    const std::vector<float> y = strike_plate(plate, 4.0, 4.0f, 24);

    const MeasuredMode fund = measure_mode(y, kFs, 55.0);
    INFO(summarize(fund));
    REQUIRE(fund.freq_hz == Catch::Approx(55.0).margin(3.0));

    const double rms_early = windowed_rms(y, static_cast<std::size_t>(0.2 * kFs), 4800);
    const double rms_2s = windowed_rms(y, static_cast<std::size_t>(2.0 * kFs), 4800);
    INFO("rms 0.2s=" << rms_early << " 2.0s=" << rms_2s);
    REQUIRE(rms_2s > 0.05 * rms_early);   // still ringing, not decayed away
}

TEST_CASE("gong bloom is real and amplitude-dependent", "[signal][gong]") {
    const PlateSpectrum spec = generate_plate_spectrum(55.0);
    GongPlate plate;
    plate.prepare(kFs, static_cast<int>(spec.modes.size()), 16, 512);
    plate.set_modes(spec.modes, spec.out_weights);
    signal::FftT<float> fft(8192);

    // Hard and soft strikes, bloom on and off. The linear control (bloom off) is
    // amplitude-INDEPENDENT (a linear system's centroid does not depend on drive
    // level), so any hard/soft centroid difference is the nonlinearity.
    plate.set_bloom(1.0);
    const std::vector<float> hard_on = strike_plate(plate, 3.0, 6.0f, 24);
    const std::vector<float> soft_on = strike_plate(plate, 3.0, 0.4f, 24);
    plate.set_bloom(0.0);
    const std::vector<float> hard_off = strike_plate(plate, 3.0, 6.0f, 24);
    const std::vector<float> soft_off = strike_plate(plate, 3.0, 0.4f, 24);

    const double c_hard_on = centroid_at(hard_on, 0.1, fft, 8192);
    const double c_soft_on = centroid_at(soft_on, 0.1, fft, 8192);
    const double c_hard_off = centroid_at(hard_off, 0.1, fft, 8192);
    const double c_soft_off = centroid_at(soft_off, 0.1, fft, 8192);

    INFO("centroid @0.1s  hard_on=" << c_hard_on << " soft_on=" << c_soft_on
         << " hard_off=" << c_hard_off << " soft_off=" << c_soft_off);

    // Linear control is amplitude-independent: hard_off ~ soft_off.
    REQUIRE(c_hard_off == Catch::Approx(c_soft_off).epsilon(0.10));

    // The bloom lifts the centroid; the lift is far larger for the hard strike
    // (F ~ n_l n_m ~ amplitude^4). This ratio is the amplitude-dependence
    // headline -- the FFT-test signature.
    const double lift_hard = c_hard_on - c_hard_off;
    const double lift_soft = c_soft_on - c_soft_off;
    INFO("bloom lift  hard=" << lift_hard << " soft=" << lift_soft
         << "  ratio=" << (lift_soft != 0.0 ? lift_hard / lift_soft : 0.0));
    REQUIRE(lift_hard > 200.0);              // a real, large bloom
    REQUIRE(lift_hard >= 3.0 * lift_soft);   // strongly amplitude-dependent

    // The hard-strike bloom BUILDS after the attack (centroid rises to a peak);
    // the soft strike does not rise.
    const double h_early = centroid_at(hard_on, 0.05, fft, 8192);
    const double h_peak = centroid_at(hard_on, 0.35, fft, 8192);
    const double s_early = centroid_at(soft_on, 0.05, fft, 8192);
    const double s_peak = centroid_at(soft_on, 0.35, fft, 8192);
    INFO("hard rise " << h_early << "->" << h_peak << "  soft " << s_early << "->" << s_peak);
    REQUIRE(h_peak > h_early);          // hard blooms upward
    REQUIRE(s_peak <= s_early * 1.05);  // soft is flat / decaying
}

TEST_CASE("gong bloom is bounded under a max-everything strike", "[signal][gong]") {
    const PlateSpectrum spec = generate_plate_spectrum(55.0);
    GongPlate plate;
    plate.prepare(kFs, static_cast<int>(spec.modes.size()), 16, 512);
    plate.set_modes(spec.modes, spec.out_weights);
    plate.set_bloom(1.0);

    // Max velocity, hardest contact, max bloom, 10 s.
    const std::vector<float> y = strike_plate(plate, 10.0, 12.0f, 8);

    double peak = 0.0;
    for (float s : y) {
        REQUIRE(std::isfinite(s));
        peak = std::max(peak, static_cast<double>(std::fabs(s)));
    }
    INFO("10 s peak = " << peak);
    REQUIRE(peak < 1.0e4);   // finite, no runaway

    // Total modal energy is monotone non-increasing (energy-conserving coupling
    // + dissipative T60). Windowed RMS decreases after the strike window.
    const int W = 4800;
    std::vector<double> rms;
    for (std::size_t i = 0; i + W < y.size(); i += W)
        rms.push_back(windowed_rms(y, i, W));
    std::size_t pk = 0;
    for (std::size_t i = 1; i < rms.size(); ++i)
        if (rms[i] > rms[pk]) pk = i;
    for (std::size_t i = pk + 2; i < rms.size(); ++i)
        REQUIRE(rms[i] <= rms[i - 1] * 1.05);   // no re-growth after the peak

    // The energy proxy the invariant tracks stays finite the whole time.
    REQUIRE(std::isfinite(plate.total_energy()));
}

TEST_CASE("gong linear control (bloom=0) matches ModalBankT", "[signal][gong]") {
    const PlateSpectrum spec = generate_plate_spectrum(110.0);
    const int nmodes = static_cast<int>(spec.modes.size());

    GongPlate plate;
    plate.prepare(kFs, nmodes, 16, 256);
    plate.set_modes(spec.modes, spec.out_weights);
    plate.set_bloom(0.0);

    signal::ModalBank bank;
    bank.prepare(kFs, nmodes);
    bank.set_modes(spec.modes);

    // Drive both with the same impulse in matched 256-sample blocks so the
    // block-boundary denormal snaps line up. With bloom off, GongPlate's ring is
    // ModalBankT's coupled-form loop with a unit pickup, so the outputs agree.
    const std::size_t total = static_cast<std::size_t>(2.0 * kFs);
    std::vector<float> in(total, 0.0f), gp(total, 0.0f), mb(total, 0.0f);
    in[0] = 1.0f;
    const int block = 256;
    for (std::size_t i = 0; i < total; i += block) {
        const int n = static_cast<int>(std::min<std::size_t>(block, total - i));
        plate.render(in.data() + i, gp.data() + i, n);
        bank.process_add(in.data() + i, mb.data() + i, n);
    }

    double max_abs = 0.0, max_diff = 0.0;
    for (std::size_t i = 0; i < total; ++i) {
        max_abs = std::max(max_abs, static_cast<double>(std::fabs(mb[i])));
        max_diff = std::max(max_diff, static_cast<double>(std::fabs(gp[i] - mb[i])));
    }
    INFO("max|ModalBank|=" << max_abs << " max|diff|=" << max_diff);
    REQUIRE(max_abs > 0.0);
    REQUIRE(max_diff <= 1.0e-3 * max_abs);
}

TEST_CASE("gong instrument plays the struck pitch", "[gong]") {
    const double fs = kFs;
    const int block = 512;

    auto render_note = [&](int note) {
        examples::GongInstrument gong;
        state::StateStore store;
        gong.define_parameters(store);
        gong.set_state_store(&store);
        format::PrepareContext pctx;
        pctx.sample_rate = fs;
        pctx.max_buffer_size = block;
        gong.prepare(pctx);

        const int total = static_cast<int>(3.0 * fs);
        std::vector<float> mono(static_cast<std::size_t>(total), 0.0f);
        audio::Buffer<float> buf(2, static_cast<std::size_t>(block));
        const float* inp[2] = {nullptr, nullptr};
        audio::BufferView<const float> in(inp, 0, static_cast<std::size_t>(block));
        bool sent = false;
        for (int pos = 0; pos < total; pos += block) {
            const int n = std::min(block, total - pos);
            for (int k = 0; k < n; ++k) { buf.channel(0)[k] = 0.0f; buf.channel(1)[k] = 0.0f; }
            float* chans[2] = {buf.channel(0).data(), buf.channel(1).data()};
            audio::BufferView<float> out(chans, 2, static_cast<std::size_t>(n));
            midi::MidiBuffer mi, mo;
            if (!sent) { mi.add(midi::MidiEvent::note_on(0, static_cast<uint8_t>(note), 127)); sent = true; }
            format::ProcessContext ctx;
            ctx.sample_rate = fs;
            ctx.num_samples = n;
            gong.process(out, in, mi, mo, ctx);
            for (int i = 0; i < n; ++i) mono[static_cast<std::size_t>(pos + i)] = buf.channel(0)[i];
        }
        return mono;
    };

    // Root note (MIDI 33 = A1) plays the plate at its authored 55 Hz fundamental;
    // an octave up (MIDI 45) uniformly scales every mode by 2. Measure the plate
    // fundamental in each render (bloom on) and confirm the octave.
    const std::vector<float> a1 = render_note(33);
    const std::vector<float> a2 = render_note(45);

    double peak = 0.0;
    for (float s : a1) { REQUIRE(std::isfinite(s)); peak = std::max(peak, static_cast<double>(std::fabs(s))); }
    INFO("A1 peak = " << peak);
    REQUIRE(peak > 0.01);          // it sounds
    REQUIRE(peak < 2.0);           // and is not wildly hot

    const MeasuredMode f_a1 = measure_mode(a1, kFs, 55.0);
    const MeasuredMode f_a2 = measure_mode(a2, kFs, 110.0);
    INFO("A1 fundamental=" << f_a1.freq_hz << " A2 fundamental=" << f_a2.freq_hz);
    REQUIRE(f_a1.freq_hz == Catch::Approx(55.0).margin(3.0));
    REQUIRE(f_a2.freq_hz == Catch::Approx(110.0).margin(5.0));
    REQUIRE(f_a2.freq_hz / f_a1.freq_hz == Catch::Approx(2.0).margin(0.15));
}

TEST_CASE("gong instrument process is allocation-free after prepare", "[gong]") {
    const double fs = kFs;
    const int block = 512;

    examples::GongInstrument gong;
    state::StateStore store;
    gong.define_parameters(store);
    gong.set_state_store(&store);
    format::PrepareContext pctx;
    pctx.sample_rate = fs;
    pctx.max_buffer_size = block;
    gong.prepare(pctx);

    audio::Buffer<float> buf(2, static_cast<std::size_t>(block));
    float* chans[2] = {buf.channel(0).data(), buf.channel(1).data()};
    const float* inp[2] = {nullptr, nullptr};
    audio::BufferView<const float> in(inp, 0, static_cast<std::size_t>(block));

    midi::MidiBuffer with_note, empty, mo;
    with_note.add(midi::MidiEvent::note_on(0, 40, 110));

    format::ProcessContext ctx;
    ctx.sample_rate = fs;
    ctx.num_samples = block;

    // Read counters inside the probe scope before any Catch2 macro (which itself
    // allocates via stringstream). The first block strikes (retune + arm), the
    // rest ring the bloom -- all must be allocation-free.
    std::size_t allocations = 0, allocated_bytes = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 32; ++b) {
            audio::BufferView<float> out(chans, 2, static_cast<std::size_t>(block));
            midi::MidiBuffer& mi = (b == 0) ? with_note : empty;
            gong.process(out, in, mi, mo, ctx);
        }
        allocations = probe.allocation_count();
        allocated_bytes = probe.allocated_bytes();
    }
    INFO("allocations in process(): " << allocations << " (" << allocated_bytes << " bytes)");
    REQUIRE(allocations == 0);
}

TEST_CASE("gong coupling cost scales in real time on one core",
          "[signal][gong][bench]") {
    const double fs = kFs;
    constexpr int block = 512;
    const std::size_t seconds_samples = 48000;

    std::printf("\ngong coupling cost (single core, %d-sample blocks, %.0f Hz)\n", block, fs);
    std::printf("%8s %8s %12s %14s %14s %10s\n",
                "modes", "triads", "ns/sample", "ns/samp(cp)", "ns/triad-blk", "%core");

    double sink = 0.0;
    for (int M : {64, 128, 256, 512, 1024}) {
        for (int c : {4, 8, 16}) {
            std::vector<signal::ModalMode> modes(static_cast<std::size_t>(M));
            std::mt19937 rng(0x6047acc0u);
            std::uniform_real_distribution<float> fr(40.0f, 11000.0f);
            std::uniform_real_distribution<float> t60(0.3f, 10.0f);
            for (auto& m : modes)
                m = {fr(rng), t60(rng), 1.0f / static_cast<float>(M)};

            GongPlate plate;
            plate.prepare(fs, M, c, block);
            plate.set_modes(modes);

            std::vector<float> in(seconds_samples, 0.0f), out(seconds_samples, 0.0f);
            in[0] = 1.0f;

            auto time_run = [&](double bloom) {
                plate.set_bloom(bloom);
                double best = 1e30;
                for (int rep = 0; rep < 3; ++rep) {
                    plate.reset();
                    std::fill(out.begin(), out.end(), 0.0f);
                    const auto t0 = std::chrono::steady_clock::now();
                    for (std::size_t i = 0; i < seconds_samples; i += block)
                        plate.render(in.data() + i, out.data() + i, block);
                    const auto t1 = std::chrono::steady_clock::now();
                    best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
                    sink += static_cast<double>(out[seconds_samples / 2]);
                }
                return best;
            };

            const double best = time_run(1.0);
            const double best_lin = time_run(0.0);
            const int K = plate.triad_count();
            const int blocks = static_cast<int>(seconds_samples) / GongPlate::kCouplingBlock;
            const double ns = best * 1e9 / seconds_samples;
            const double ns_cp = (best - best_lin) * 1e9 / seconds_samples;
            const double ns_tri = K > 0 ? (best - best_lin) * 1e9 / (static_cast<double>(K) * blocks) : 0.0;
            std::printf("%8d %8d %12.1f %14.2f %14.3f %10.4f\n",
                        M, K, ns, ns_cp, ns_tri, best * 100.0);
        }
    }
    REQUIRE(std::isfinite(sink));

    // The load-bearing invariant that settles the GPU question: the largest
    // configuration here (1024 coupled modes, cap 16) must ring + couple in real
    // time on one core with comfortable margin. The coupling is ~two orders of
    // magnitude below the linear ring, so the whole struck-gong instrument sits
    // well under one core -- nowhere near the 50%-of-a-core GPU-adoption gate.
    {
        std::vector<signal::ModalMode> modes(1024);
        std::mt19937 rng(1u);
        std::uniform_real_distribution<float> fr(40.0f, 11000.0f);
        std::uniform_real_distribution<float> t60(0.3f, 10.0f);
        for (auto& m : modes) m = {fr(rng), t60(rng), 1.0f / 1024.0f};
        GongPlate plate;
        plate.prepare(fs, 1024, 16, block);
        plate.set_modes(modes);
        plate.set_bloom(1.0);
        std::vector<float> in(seconds_samples, 0.0f), out(seconds_samples, 0.0f);
        in[0] = 1.0f;
        double best = 1e30;
        for (int rep = 0; rep < 3; ++rep) {
            plate.reset();
            std::fill(out.begin(), out.end(), 0.0f);
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < seconds_samples; i += block)
                plate.render(in.data() + i, out.data() + i, block);
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
        }
        INFO("1024-mode/cap16 coupled render = " << best * 100.0 << "% of one core");
        REQUIRE(best < 0.5);   // comfortably real time on one core
    }
}
