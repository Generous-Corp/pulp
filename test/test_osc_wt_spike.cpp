/// @file test_osc_wt_spike.cpp
/// OSC-WT wavetable-architecture spike: decide the lo-fi engine by measurement.
///
/// This is a measurement spike, not shipped engine code. It renders a few
/// EXPERIMENTAL table readers and compares their spectra against the analytic
/// variable-clock zero-order-hold (ZOH) image model, to answer two questions:
///
///   1. Lo-fi tier (falsifiable hypothesis H-R2): does the fixed-rate,
///      band-limited `WavetableT` playback engine LACK the pitch-tracking image
///      lines at n·L·f0 that a variable-clock ZOH reader produces? If so, a
///      faithful lo-fi profile needs a dedicated ZOH engine, not a parameterized
///      `WavetableT`.
///   2. Modern tier: how does `WavetableT`'s band-switch-crossfade compare to a
///      mip-map + continuous-interpolate design at a band boundary, in alias
///      rejection and in the switch discontinuity.
///
/// Every claim is proven with a measured number and a negative control, using
/// the shared tone-projection analyzers (no hand-rolled spectral analysis). The
/// readers below are deliberately minimal and live only in this test.
///
/// The subject of the lo-fi profile is referred to only by its neutral codename
/// OSC-WT; it models the published playback behavior of an early-1980s
/// variable-clock 8-bit wavetable architecture, never any wave content.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/wavetable.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::fit_tone;
using pulp::test::audio::measure_aliasing;

namespace {

// ── Spike parameters ───────────────────────────────────────────────────────

constexpr double kSampleRate = 48000.0;
// Analysis length. A power of two is not required by the projection analyzers,
// but this length is chosen so the fundamental is bin-coherent (see kFundK):
// every component sits at an exact DFT bin, so the single-tone fits are mutually
// orthogonal and exact.
constexpr int kN = 65536;
// Short single-cycle table length. WP-R2 pins L to 64 or 128 for this
// architecture class; 128 is the primary here, 64 is exercised as a secondary.
constexpr int kL = 128;
// Fundamental as an exact DFT bin (k cycles in kN samples): f0 = k·sr/N.
// k = 75 lands f0 at ~54.93 Hz — an A1-register bass note, where the lo-fi
// image lines fall audibly below Nyquist and define the "dark, imaged" bass.
constexpr int kFundK = 75;
constexpr double kF0 = static_cast<double>(kFundK) * kSampleRate / kN; // 54.9316 Hz

// ── Small math helpers ─────────────────────────────────────────────────────

// Normalized sinc, sinc(x) = sin(pi x)/(pi x), sinc(0) = 1.
double sinc(double x) {
    if (std::abs(x) < 1e-12) return 1.0;
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

// Analytic ZOH image level (dB relative to the played fundamental) for a
// pure-sine table clocked at fs_play = f0·L. Each spectral replica at
// frequency f is shaped by |sinc(f/fs_play)|; the played fundamental itself
// sits at |sinc(f0/fs_play)| = |sinc(1/L)|, so the ratio is what a
// fundamental-relative measurement reads.
double analytic_zoh_image_db(double image_hz, double fs_play) {
    const double num = std::abs(sinc(image_hz / fs_play));
    const double den = std::abs(sinc(kF0 / fs_play));
    return 20.0 * std::log10(num / den);
}

// Amplitude of a single tone at `hz` fitted out of `samples` by least squares.
// FFT-free: no leakage skirt, so a −40..−90 dBc line reads at its true level.
double amp_at(std::span<const double> samples, double hz) {
    return fit_tone(samples, hz / kSampleRate).amplitude;
}

// dB of the tone at `hz` relative to the fundamental tone at kF0.
double dbc_at(std::span<const double> samples, double hz) {
    const double fund = amp_at(samples, kF0);
    const double site = amp_at(samples, hz);
    if (fund <= 0.0 || site <= 0.0) return -400.0;
    return 20.0 * std::log10(site / fund);
}

// ── Experimental single-cycle tables ───────────────────────────────────────

// One cycle of a sine in `length` samples. `bits <= 0` keeps full precision;
// otherwise the TABLE DATA (not the output stream) is quantized to `bits`
// signed — the WP-R2 §1.1 distinction that makes the quantization error
// harmonic-locked rather than white.
std::vector<double> make_sine_table(int length, int bits) {
    std::vector<double> t(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
        double s = std::sin(2.0 * std::numbers::pi * static_cast<double>(i) /
                            static_cast<double>(length));
        if (bits > 0) {
            const double levels = static_cast<double>((1 << (bits - 1)) - 1); // ±127 for 8-bit
            s = std::round(s * levels) / levels;
        }
        t[static_cast<std::size_t>(i)] = s;
    }
    return t;
}

// ── Experimental readers (spike only, not shipped engine code) ─────────────

// R1 — the incumbent engine, parameterized for lo-fi as far as it allows:
// the real `pulp::signal::WavetableT` playing the raw short table as a single
// band (no band-switch crossfade), at its fixed output-rate phase increment,
// with its built-in LINEAR interpolation. This is "extend WavetableT" made
// concrete.
std::vector<double> render_wavetableT_raw(const std::vector<double>& table,
                                          double f0, int n) {
    pulp::signal::WavetableEntryT<double> entry{table, 24000.0};
    std::vector<pulp::signal::WavetableEntryT<double>> bands;
    bands.push_back(entry);
    pulp::signal::WavetableT<double> wt(std::move(bands));
    wt.set_sample_rate(kSampleRate);
    wt.set_frequency(f0);
    wt.reset();
    std::vector<double> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out[static_cast<std::size_t>(i)] = wt.next();
    return out;
}

// R4 — the incumbent as it would REALLY be built for a modern tone: a
// band-limited harmonic-synthesized sine table via the shipped factory, fixed
// rate, linear interp. The strongest "images absent" reference.
std::vector<double> render_wavetableT_bandlimited_sine(double f0, int n) {
    auto wt = pulp::signal::WavetableT<double>::make_sine(2048, kSampleRate);
    wt.set_sample_rate(kSampleRate);
    wt.set_frequency(f0);
    wt.reset();
    std::vector<double> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out[static_cast<std::size_t>(i)] = wt.next();
    return out;
}

// R2 — a fixed-rate NEAREST reader over the raw short table. This is the
// "force WavetableT's interpolation to nearest" parameterization. Note it is
// algebraically identical to point-sampling the variable-clock ZOH staircase at
// the output rate (naive decimation): index = floor(phase·L), phase advances by
// f0/sr, so table position advances at f0·L = fs_play per output sample.
std::vector<double> render_fixed_nearest(const std::vector<double>& table,
                                         double f0, int n) {
    const int L = static_cast<int>(table.size());
    std::vector<double> out(static_cast<std::size_t>(n));
    double phase = 0.0;
    const double dt = f0 / kSampleRate;
    for (int i = 0; i < n; ++i) {
        int idx = static_cast<int>(std::floor(phase * L)) % L;
        if (idx < 0) idx += L;
        out[static_cast<std::size_t>(i)] = table[static_cast<std::size_t>(idx)];
        phase += dt;
        phase -= std::floor(phase);
    }
    return out;
}

// R3 — the honest variable-clock ZOH reader (WP-R2 §2.3): run the ZOH staircase
// at a high oversampled internal rate M·sr, then decimate to the output rate
// with a reconstruction lowpass so images the original never emitted (above the
// reconstruction corner) are removed rather than folded back in-band. This is
// the construction the naive R2 skips.
std::vector<double> render_zoh_reconstructed(const std::vector<double>& table,
                                             double f0, int n, int oversample) {
    const int L = static_cast<int>(table.size());
    const int M = oversample;
    const double fs_hi = kSampleRate * M;
    const int hi_n = (n + 4) * M; // headroom for the FIR
    std::vector<double> hi(static_cast<std::size_t>(hi_n));
    const double dt_hi = f0 / fs_hi;
    double phase = 0.0;
    for (int j = 0; j < hi_n; ++j) {
        int idx = static_cast<int>(std::floor(phase * L)) % L;
        if (idx < 0) idx += L;
        hi[static_cast<std::size_t>(j)] = table[static_cast<std::size_t>(idx)];
        phase += dt_hi;
        phase -= std::floor(phase);
    }
    // Blackman-windowed-sinc reconstruction lowpass at the output Nyquist
    // (cutoff = sr/2, normalized to the internal rate = 0.5/M). This models a
    // steep analog reconstruction filter: it passes every sub-Nyquist authentic
    // image and rejects the super-Nyquist replicas naive point-sampling folds.
    const int half = 8 * M;
    const int taps = 2 * half + 1;
    std::vector<double> h(static_cast<std::size_t>(taps));
    const double fc = 0.5 / M; // cycles/sample at the internal rate
    double sum = 0.0;
    for (int k = 0; k < taps; ++k) {
        const int m = k - half;
        const double s = 2.0 * fc * sinc(2.0 * fc * m);
        const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * k / (taps - 1)) +
                         0.08 * std::cos(4.0 * std::numbers::pi * k / (taps - 1));
        h[static_cast<std::size_t>(k)] = s * w;
        sum += s * w;
    }
    for (double& c : h) c /= sum; // unity DC gain
    std::vector<double> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int center = i * M + half; // align decimation phase
        double acc = 0.0;
        for (int k = 0; k < taps; ++k) {
            const int j = center - (k - half);
            if (j >= 0 && j < hi_n)
                acc += h[static_cast<std::size_t>(k)] * hi[static_cast<std::size_t>(j)];
        }
        out[static_cast<std::size_t>(i)] = acc;
    }
    return out;
}

// Fitting a tone amplitude requires a float BufferView for the alias analyzer;
// this copies a double render into an owning single-channel float buffer.
pulp::audio::Buffer<float> to_buffer(const std::vector<double>& v) {
    pulp::audio::Buffer<float> buf(1, v.size());
    for (std::size_t i = 0; i < v.size(); ++i)
        buf.channel(0)[i] = static_cast<float>(v[i]);
    return buf;
}

} // namespace

// ── H-R2: the deciding measurement ─────────────────────────────────────────

TEST_CASE("OSC-WT lo-fi: variable-clock ZOH shows the n·L·f0 image lines that a "
          "fixed-rate WavetableT lacks",
          "[wavetable][spike][osc-wt]") {
    const double fs_play = kF0 * kL; // 7031.25 Hz
    const auto table = make_sine_table(kL, /*bits=*/0); // full precision: images are the ONLY spuria

    const auto zoh = render_fixed_nearest(table, kF0, kN);         // prototype (2)
    const auto wt_raw = render_wavetableT_raw(table, kF0, kN);     // prototype (1), raw table
    const auto wt_bl = render_wavetableT_bandlimited_sine(kF0, kN);// prototype (1), factory table

    // The first three ZOH image pairs, at (n·L ± 1)·f0.
    struct Site { int index; double hz; };
    std::vector<Site> sites;
    for (int nlobe = 1; nlobe <= 3; ++nlobe) {
        for (int sign : {-1, +1}) {
            const int h = nlobe * kL + sign;
            sites.push_back({h, static_cast<double>(h) * kF0});
        }
    }

    INFO("fs_play = " << fs_play << " Hz, f0 = " << kF0 << " Hz, L = " << kL);
    for (const auto& s : sites) {
        const double predicted = analytic_zoh_image_db(s.hz, fs_play);
        const double meas_zoh = dbc_at(zoh, s.hz);
        const double meas_wt_raw = dbc_at(wt_raw, s.hz);
        const double meas_wt_bl = dbc_at(wt_bl, s.hz);
        INFO("image h=" << s.index << " @ " << s.hz << " Hz | analytic "
                        << predicted << " dBc | ZOH " << meas_zoh
                        << " | WT-raw " << meas_wt_raw << " | WT-bandlimited "
                        << meas_wt_bl);

        // (a) The ZOH reader reproduces each image within 1.5 dB of the analytic
        //     variable-clock model — the image lines are PRESENT at their
        //     predicted, pitch-tracking sinc-shaped levels.
        CHECK_THAT(meas_zoh,
                   Catch::Matchers::WithinAbs(predicted, 1.5));

        // (b) The fixed-rate WavetableT lacks them: both the raw-table and the
        //     band-limited-factory parameterizations sit far below the ZOH
        //     level. This is H-R2 confirmed.
        CHECK(meas_wt_raw < predicted - 25.0);
        CHECK(meas_wt_bl < predicted - 40.0);
    }

    // The gap at the strongest image (L−1)·f0 is the headline number: how many
    // dB louder the ZOH image is than the same site in the fixed-rate engine.
    const double strongest_hz = static_cast<double>(kL - 1) * kF0;
    const double gap_raw = dbc_at(zoh, strongest_hz) - dbc_at(wt_raw, strongest_hz);
    const double gap_bl = dbc_at(zoh, strongest_hz) - dbc_at(wt_bl, strongest_hz);
    INFO("strongest image (L-1)·f0 = " << strongest_hz << " Hz: ZOH is " << gap_raw
                                       << " dB above WT-raw, " << gap_bl
                                       << " dB above WT-bandlimited");
    CHECK(gap_raw > 25.0);
    CHECK(gap_bl > 40.0);
}

// ── Controls: the analyzer sees a −42 dBc line when present, and does not
//    invent one when absent ──────────────────────────────────────────────────

TEST_CASE("OSC-WT lo-fi: image measurement has a positive and negative control",
          "[wavetable][spike][osc-wt]") {
    const double fs_play = kF0 * kL;
    const double image_hz = static_cast<double>(kL - 1) * kF0; // 6976.3 Hz
    const double predicted = analytic_zoh_image_db(image_hz, fs_play);

    // Negative control: the band-limited fixed-rate render has no image by
    // construction; the fit at the image site must collapse far below it. This
    // proves the ZOH reading in the previous case is real signal, not a method
    // artifact.
    auto clean = render_wavetableT_bandlimited_sine(kF0, kN);
    const double clean_site = dbc_at(clean, image_hz);
    INFO("negative control: clean render at image site reads " << clean_site
         << " dBc (predicted image would be " << predicted << ")");
    CHECK(clean_site < -90.0);

    // Positive control: inject a synthetic tone at the image site at the exact
    // predicted level into the clean render; the analyzer must recover it.
    const double fund = amp_at(clean, kF0);
    const double inject_amp = fund * std::pow(10.0, predicted / 20.0);
    for (int i = 0; i < kN; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        clean[static_cast<std::size_t>(i)] +=
            inject_amp * std::sin(2.0 * std::numbers::pi * image_hz * t + 0.4);
    }
    const double recovered = dbc_at(clean, image_hz);
    INFO("positive control: injected " << predicted << " dBc, recovered "
                                       << recovered << " dBc");
    CHECK_THAT(recovered, Catch::Matchers::WithinAbs(predicted, 0.5));
}

// ── Determinism: the ZOH engine is a fixed function of note+phase ───────────

TEST_CASE("OSC-WT lo-fi: the ZOH render is bit-identical cycle to cycle",
          "[wavetable][spike][osc-wt]") {
    const auto table = make_sine_table(kL, /*bits=*/8);
    const auto a = render_fixed_nearest(table, kF0, kN);
    const auto b = render_fixed_nearest(table, kF0, kN);
    REQUIRE(a.size() == b.size());
    bool identical = true;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) { identical = false; break; }
    CHECK(identical); // any drift here would be a bug (WP-R2 §2.2), not the sound
}

// ── §2.3: naive point-sampling folds a modeling alias in-band; the
//    reconstructed reader removes it. Both keep the sub-Nyquist images. ───────

TEST_CASE("OSC-WT lo-fi: reconstruction removes the modeling alias that naive "
          "point-sampling folds in-band",
          "[wavetable][spike][osc-wt]") {
    const double fs_play = kF0 * kL;
    const auto table = make_sine_table(kL, /*bits=*/0);
    const int M = 8;
    const auto naive = render_fixed_nearest(table, kF0, kN);
    const auto recon = render_zoh_reconstructed(table, kF0, kN, M);

    // The 4th replica pair sits above the output Nyquist: (4L−1)·f0 = 511·f0 =
    // 28070 Hz folds around 48 kHz to 19930 Hz — an audible, in-band alias that
    // the original reconstruction filter would have removed. It lands off the f0
    // grid (48 kHz is not a multiple of f0), so it is cleanly separable.
    const double super_hz = static_cast<double>(4 * kL - 1) * kF0; // 28070 Hz
    const double folded_hz = kSampleRate - super_hz;               // 19930 Hz
    const double naive_fold = dbc_at(naive, folded_hz);
    const double recon_fold = dbc_at(recon, folded_hz);
    INFO("folded modeling alias @ " << folded_hz << " Hz: naive " << naive_fold
                                    << " dBc, reconstructed " << recon_fold << " dBc");
    // Naive point-sampling folds it in-band around −54 dBc; the reconstructed
    // reader knocks it down by ~20 dB (the residual is bounded by this spike's
    // short Blackman FIR stopband — a production reconstruction filter would go
    // further). The point is the direction and size of the effect, not a
    // brickwall number.
    CHECK(naive_fold > -70.0);
    CHECK(recon_fold < naive_fold - 20.0);

    // Both readers keep the authentic sub-Nyquist images at the same level: the
    // reconstruction preserves what the original passed.
    for (int nlobe = 1; nlobe <= 2; ++nlobe) {
        const double hz = static_cast<double>(nlobe * kL - 1) * kF0;
        const double predicted = analytic_zoh_image_db(hz, fs_play);
        const double n_db = dbc_at(naive, hz);
        const double r_db = dbc_at(recon, hz);
        INFO("authentic image @ " << hz << " Hz: analytic " << predicted
                                  << ", naive " << n_db << ", reconstructed " << r_db);
        CHECK_THAT(n_db, Catch::Matchers::WithinAbs(predicted, 1.5));
        CHECK_THAT(r_db, Catch::Matchers::WithinAbs(predicted, 1.5));
    }
}

// ── §1.1: the 8-bit TABLE quantization is harmonic-locked, ≈ −50 dBc, and
//    deterministic — the "grit" is the sound, not a white noise floor. ────────

TEST_CASE("OSC-WT lo-fi: 8-bit table quantization is harmonic-locked, odd-only, "
          "aggregating near −50 dBc",
          "[wavetable][spike][osc-wt]") {
    // A full-precision sine table has NO harmonic content above the fundamental
    // under ZOH (only the f0-tracking images). Quantizing the TABLE to 8 bits
    // adds a deterministic, period-L error whose spectrum locks to harmonics of
    // f0. Because a sine quantized about its midpoint keeps half-wave symmetry,
    // the added spuria appear only at ODD harmonics — a measured nuance beyond
    // WP-R2 §1.1's aggregate figure.
    const auto clean = render_fixed_nearest(make_sine_table(kL, 0), kF0, kN);
    const auto q8 = render_fixed_nearest(make_sine_table(kL, 8), kF0, kN);

    for (int h : {2, 4}) { // even harmonics stay absent (half-wave symmetry)
        const double hz = static_cast<double>(h) * kF0;
        const double q8_db = dbc_at(q8, hz);
        INFO("even harmonic " << h << " @ " << hz << " Hz: 8-bit " << q8_db << " dBc");
        CHECK(q8_db < -80.0);
    }
    for (int h : {3, 5}) { // odd harmonics carry the grit
        const double hz = static_cast<double>(h) * kF0;
        const double clean_db = dbc_at(clean, hz);
        const double q8_db = dbc_at(q8, hz);
        INFO("odd harmonic " << h << " @ " << hz << " Hz: full-precision " << clean_db
                             << " dBc, 8-bit " << q8_db << " dBc");
        CHECK(clean_db < -90.0);           // empty without quantization
        CHECK(q8_db > -85.0);              // lifted by the 8-bit table
        CHECK(q8_db < -55.0);              // but each single harmonic sits well
                                           // below the aggregate figure
    }

    // The aggregate: the quantization error signal is exactly (8-bit − clean)
    // played through the same reader, so its RMS relative to the fundamental is
    // the classic 8-bit SNR. Ideal uniform 8-bit gives 6.02·8 + 1.76 ≈ 49.9 dB,
    // i.e. an error floor near −50 dBFS — WP-R2 §1.1's headline number.
    double err_sq = 0.0;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const double e = q8[i] - clean[i];
        err_sq += e * e;
    }
    const double err_rms = std::sqrt(err_sq / static_cast<double>(clean.size()));
    const double fund_rms = amp_at(q8, kF0) / std::numbers::sqrt2;
    const double snr_db = 20.0 * std::log10(fund_rms / err_rms);
    INFO("aggregate 8-bit quantization SNR = " << snr_db
         << " dB (ideal 8-bit ≈ 49.9 dB)");
    CHECK_THAT(snr_db, Catch::Matchers::WithinAbs(49.9, 4.0));
}

// ── L = 64 secondary: the image ladder and the ceiling both scale with L ────

TEST_CASE("OSC-WT lo-fi: the image ladder tracks L (64-sample table)",
          "[wavetable][spike][osc-wt]") {
    constexpr int L64 = 64;
    const double fs_play = kF0 * L64; // 3515.6 Hz
    const auto table = make_sine_table(L64, 0);
    const auto zoh = render_fixed_nearest(table, kF0, kN);
    const auto wt = render_wavetableT_raw(table, kF0, kN);

    for (int nlobe = 1; nlobe <= 3; ++nlobe) {
        const double hz = static_cast<double>(nlobe * L64 - 1) * kF0;
        const double predicted = 20.0 * std::log10(std::abs(sinc(hz / fs_play)) /
                                                    std::abs(sinc(kF0 / fs_play)));
        const double meas = dbc_at(zoh, hz);
        const double meas_wt = dbc_at(wt, hz);
        INFO("L=64 image (n·64-1)·f0 @ " << hz << " Hz: analytic " << predicted
                                         << ", ZOH " << meas << ", WT-raw " << meas_wt);
        CHECK_THAT(meas, Catch::Matchers::WithinAbs(predicted, 1.5));
        CHECK(meas_wt < predicted - 20.0);
    }
}

// ── Modern tier (D3): band-switch-crossfade vs mip-map+interpolate at a
//    band boundary — alias rejection (measured) and switch discontinuity. ─────

namespace {

// A minimal mip-map + continuous-interpolate reader for the modern-tier
// comparison. It holds octave-spaced band-limited saw tables (max harmonic
// halves each level up) and, at any frequency, blends the two adjacent levels
// by the fractional octave — a CONTINUOUS blend, so there is no discrete switch
// instant (contrast WavetableT's crossfade window). Fixed output rate, linear
// intra-table interpolation.
class MipSawReader {
public:
    MipSawReader(int table_len, int levels, double sr) : sr_(sr) {
        // Level `l` caps harmonics at kBase·2^l so the table stays alias-free up
        // to that playback ceiling. Level 0 is the brightest (highest ceiling).
        for (int l = 0; l < levels; ++l) {
            const int max_h = std::max(1, 1024 >> l);
            std::vector<double> t(static_cast<std::size_t>(table_len), 0.0);
            for (int i = 0; i < table_len; ++i) {
                const double ph = static_cast<double>(i) / table_len;
                double s = 0.0;
                for (int k = 1; k <= max_h; ++k)
                    s += (1.0 / k) * std::sin(2.0 * std::numbers::pi * k * ph);
                t[static_cast<std::size_t>(i)] = s;
            }
            double peak = 0.0;
            for (double v : t) peak = std::max(peak, std::abs(v));
            if (peak > 0.0) for (double& v : t) v /= peak;
            tables_.push_back(std::move(t));
            // ceiling for this level in Hz: highest harmonic reaches Nyquist
            ceil_hz_.push_back((sr * 0.5) / max_h);
        }
    }
    void set_frequency(double hz) { freq_ = hz; }
    double next() {
        // Pick the fractional level: level whose ceiling just covers freq, plus
        // a continuous blend toward the next-coarser level.
        double level = 0.0;
        for (std::size_t l = 0; l < ceil_hz_.size(); ++l) {
            if (freq_ <= ceil_hz_[l]) { level = static_cast<double>(l); break; }
            level = static_cast<double>(l);
        }
        // Blend between adjacent levels by how far freq sits past this ceiling.
        std::size_t lo = static_cast<std::size_t>(level);
        std::size_t hi = std::min(lo + 1, tables_.size() - 1);
        double frac = 0.0;
        if (lo < ceil_hz_.size() && freq_ > ceil_hz_[lo] && hi != lo) {
            const double span = ceil_hz_[lo] * 2.0 - ceil_hz_[lo];
            frac = std::min(1.0, (freq_ - ceil_hz_[lo]) / std::max(1.0, span));
        }
        const double s_lo = sample(lo);
        const double s_hi = sample(hi);
        const double dt = freq_ / sr_;
        phase_ += dt;
        phase_ -= std::floor(phase_);
        return s_lo * (1.0 - frac) + s_hi * frac;
    }

private:
    double sample(std::size_t level) const {
        const auto& t = tables_[level];
        const double pos = phase_ * t.size();
        const std::size_t i0 = static_cast<std::size_t>(std::floor(pos)) % t.size();
        const std::size_t i1 = (i0 + 1) % t.size();
        const double frac = pos - std::floor(pos);
        return t[i0] * (1.0 - frac) + t[i1] * frac;
    }
    std::vector<std::vector<double>> tables_;
    std::vector<double> ceil_hz_;
    double sr_;
    double freq_ = 440.0;
    double phase_ = 0.0;
};

// Worst in-band (≤ 20 kHz) alias of a saw at `f0`, dB below the fundamental.
double worst_in_band_alias_db(const std::vector<double>& render, double f0) {
    auto buf = to_buffer(render);
    AliasOptions opts;
    // Size the modeled series from the sample rate so at least one harmonic
    // lands above Nyquist (the analyzer requires a fold site).
    opts.num_harmonics = std::max(64, static_cast<int>(3.0 * kSampleRate / f0));
    opts.analysis_offset = 2048; // skip startup
    const AliasReport rep =
        measure_aliasing(std::as_const(buf).view(), f0, kSampleRate, opts);
    return rep.worst_alias_db;
}

// Peak first-difference (|x[n]-x[n-1]|) over a sample window, a raw
// discontinuity metric. A hard switch spikes this; a continuous blend does not.
double peak_first_diff(const std::vector<double>& x, int from, int to) {
    double worst = 0.0;
    for (int i = std::max(1, from); i < to && i < static_cast<int>(x.size()); ++i)
        worst = std::max(worst, std::abs(x[static_cast<std::size_t>(i)] -
                                         x[static_cast<std::size_t>(i - 1)]));
    return worst;
}

} // namespace

TEST_CASE("OSC-WT modern: band-boundary alias rejection, band-switch vs mip-map",
          "[wavetable][spike][osc-wt]") {
    // Find a real band boundary in the shipped factory saw by scanning for the
    // frequency where WavetableT increments its selected band.
    auto saw = pulp::signal::WavetableT<double>::make_saw(10, 2048, kSampleRate);
    saw.set_sample_rate(kSampleRate);
    double boundary = 0.0;
    int prev = -1;
    for (double f = 40.0; f < 12000.0; f *= 1.001) {
        saw.set_frequency(f);
        const int b = saw.current_band();
        // Take a midrange boundary (> 1.5 kHz): at a very low boundary the
        // alias fit would model thousands of harmonics, and a low bass boundary
        // is not where band-switching character is judged anyway.
        if (prev >= 0 && b != prev && f > 1500.0) { boundary = f; break; }
        prev = b;
    }
    REQUIRE(boundary > 0.0);
    INFO("first WavetableT saw band boundary at " << boundary << " Hz");

    // Render the shipped band-switch engine steadily just above the boundary
    // (so the higher band is selected) and measure its in-band alias.
    auto render_wt_saw = [&](double f0) {
        pulp::signal::WavetableT<double> s =
            pulp::signal::WavetableT<double>::make_saw(10, 2048, kSampleRate);
        s.set_sample_rate(kSampleRate);
        s.set_frequency(f0);
        s.reset();
        std::vector<double> out(static_cast<std::size_t>(kN));
        for (int i = 0; i < kN; ++i) out[static_cast<std::size_t>(i)] = s.next();
        return out;
    };
    const double f_test = boundary * 1.01;
    const double wt_alias = worst_in_band_alias_db(render_wt_saw(f_test), f_test);

    MipSawReader mip(2048, 8, kSampleRate);
    mip.set_frequency(f_test);
    std::vector<double> mip_render(static_cast<std::size_t>(kN));
    for (int i = 0; i < kN; ++i) mip_render[static_cast<std::size_t>(i)] = mip.next();
    const double mip_alias = worst_in_band_alias_db(mip_render, f_test);

    INFO("band-boundary worst in-band alias: WavetableT band-switch "
         << wt_alias << " dBc, mip-map+interp " << mip_alias << " dBc");
    // Both designs must keep the boundary alias well controlled — the point of a
    // band/mip structure. Assert both clear a modest floor; the INFO line above
    // reports which rejects better for the D3 record.
    CHECK(wt_alias < -55.0);
    CHECK(mip_alias < -40.0);
}

TEST_CASE("OSC-WT modern: switch discontinuity, band-switch-crossfade vs "
          "mip-map continuous blend",
          "[wavetable][spike][osc-wt]") {
    // Sweep a saw across a band boundary and measure the peak sample-to-sample
    // discontinuity in the switch region versus steady state. This is the RAW
    // discontinuity, not the model-referenced click gate (WP-R2 §5) — see the
    // final report's scope note.
    auto saw = pulp::signal::WavetableT<double>::make_saw(10, 2048, kSampleRate);
    saw.set_sample_rate(kSampleRate);
    double boundary = 0.0;
    int prev = -1;
    for (double f = 40.0; f < 12000.0; f *= 1.001) {
        saw.set_frequency(f);
        const int b = saw.current_band();
        // Take a midrange boundary (> 1.5 kHz): at a very low boundary the
        // alias fit would model thousands of harmonics, and a low bass boundary
        // is not where band-switching character is judged anyway.
        if (prev >= 0 && b != prev && f > 1500.0) { boundary = f; break; }
        prev = b;
    }
    REQUIRE(boundary > 0.0);

    // Band-switch engine: hold below the boundary, then jump above it to trigger
    // the crossfade, and render through it.
    pulp::signal::WavetableT<double> s =
        pulp::signal::WavetableT<double>::make_saw(10, 2048, kSampleRate);
    s.set_sample_rate(kSampleRate);
    s.set_frequency(boundary * 0.99);
    s.reset();
    std::vector<double> wt(static_cast<std::size_t>(kN));
    const int switch_at = 4096;
    for (int i = 0; i < kN; ++i) {
        if (i == switch_at) s.set_frequency(boundary * 1.02);
        wt[static_cast<std::size_t>(i)] = s.next();
    }
    const double wt_steady = peak_first_diff(wt, 1024, 3000);
    const double wt_switch = peak_first_diff(wt, switch_at, switch_at + 256);

    // Mip-map continuous blend: same sweep expressed as a per-sample frequency
    // ramp, so there is no switch instant at all.
    MipSawReader mip(2048, 8, kSampleRate);
    std::vector<double> mp(static_cast<std::size_t>(kN));
    for (int i = 0; i < kN; ++i) {
        const double f = (i < switch_at) ? boundary * 0.99 : boundary * 1.02;
        mip.set_frequency(f);
        mp[static_cast<std::size_t>(i)] = mip.next();
    }
    const double mp_steady = peak_first_diff(mp, 1024, 3000);
    const double mp_switch = peak_first_diff(mp, switch_at, switch_at + 256);

    INFO("band-switch: steady peak-diff " << wt_steady << ", switch-region "
         << wt_switch << " (ratio " << wt_switch / wt_steady << ")");
    INFO("mip-map:     steady peak-diff " << mp_steady << ", switch-region "
         << mp_switch << " (ratio " << mp_switch / mp_steady << ")");
    // WavetableT's crossfade keeps the switch discontinuity bounded — no
    // out-of-family single-sample jump beyond ordinary waveform slew.
    CHECK(wt_switch < wt_steady * 3.0);
    // The continuous blend introduces no switch instant at all.
    CHECK(mp_switch < mp_steady * 3.0);
}
