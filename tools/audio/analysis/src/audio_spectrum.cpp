// audio_spectrum.cpp — buffer-level offline spectrum analyzers (magnitude
// response + THD/THD+N). The per-analyzer determinism contracts live in
// audio_spectrum.hpp; keep the window/leakage/coherence behavior here in
// lock-step with them.

#include <pulp/audio/analysis/audio_spectrum.hpp>

#include <pulp/signal/fft.hpp>
#include <pulp/signal/windowing.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace pulp::test::audio {

namespace {

void require(bool ok, const char* what) {
    if (!ok)
        throw std::invalid_argument(std::string("AudioSpectrum: ") + what);
}

bool is_power_of_two(int n) { return n > 0 && (n & (n - 1)) == 0; }

pulp::signal::WindowFunction::Type to_signal_type(Window w) {
    using Type = pulp::signal::WindowFunction::Type;
    switch (w) {
        case Window::rectangular: return Type::rectangular;
        case Window::hann:        return Type::hann;
        case Window::hamming:     return Type::hamming;
        case Window::blackman:    return Type::blackman;
        case Window::flat_top:    return Type::flat_top;
        case Window::kaiser:      return Type::kaiser;
    }
    return Type::rectangular;
}

// Analytic coherent gain, for the two windows whose divisor must be EXACT.
//
// A periodic cosine-sum window's mean is its `a0` term (every cosine term sums
// to zero over a full period), so `a0` is the analytic coherent gain. But only
// rectangular (1.0) and hann (0.5) get it from this table, and the reason is
// bit-exactness, not elegance: those two are the windows that predate the enum
// widening, and returning the exact power-of-two divisor is what keeps their
// results bit-for-bit identical to the un-normalized ones. A mean summed from
// the coefficients would land an ulp away and silently perturb them.
//
// Every other window returns 0.0, meaning "measure the true mean instead". That
// is deliberate: core/signal/windowing.hpp writes its coefficients as FLOAT
// literals (`SampleType{0.54f}`), so instantiated at double the a0 actually used
// is `double(0.54f)` = 0.54000002…, not 0.54 — the textbook constant is ~2e-8
// away from the coefficients it would be normalizing, which is enough to leave
// the DC gain visibly off 1. The measured mean is exact by construction and
// immune to how upstream spells its constants. These windows are new, so they
// carry no bit-exactness obligation and can take the robust path.
double analytic_coherent_gain(Window w) {
    switch (w) {
        case Window::rectangular: return 1.0;
        case Window::hann:        return 0.5;
        case Window::hamming:
        case Window::blackman:
        case Window::flat_top:
        case Window::kaiser:      return 0.0;
    }
    return 1.0;
}

// Un-normalized periodic (DFT-even) coefficients of length n.
//
// core/signal owns the coefficient math (windowing.hpp) — none is duplicated
// here. Its generate() is SYMMETRIC (denominator size-1), while spectral
// analysis of an FFT segment needs the PERIODIC form with denominator n.
// Generating n+1 points and dropping the last gives exactly that: the
// symmetric window of length n+1 has denominator (n+1)-1 = n, so its first n
// coefficients ARE the periodic window of length n.
std::vector<double> raw_periodic_window(Window w, int n, double kaiser_beta) {
    auto coeffs = pulp::signal::WindowFunction::generate<double>(
        n + 1, to_signal_type(w), kaiser_beta);
    coeffs.resize(static_cast<std::size_t>(n));
    return coeffs;
}

// Extract one channel segment [offset, offset+N) into a windowed DOUBLE buffer
// ready for forward_real(). `win` holds the N precomputed, coherent-gain-
// normalized coefficients (see window_coefficients). DC is removed (segment
// mean subtracted) when `remove_dc` is true — true for steady tones, but false
// for an impulse-response segment, where subtracting the mean would add a
// constant pedestal across the whole window and bias the low bins.
//
// The segment must lie entirely within the channel. Zero-padding a short
// buffer would window a TRUNCATED signal, and the truncation edge leaks like
// any other discontinuity: a clean coherent tone over 4096 samples analyzed
// at fft_length 16384 reads THD −48.7 dB (true value −176.7) and a kaiser
// spectrum's floor collapses from −135 dB to −14 dB — confidently wrong
// numbers, while the result still claims coherence. The phase / group-delay
// path is no different: reading past the capture would differentiate the
// truncation edge, not the signal. Refusing is the only honest answer; callers
// with a short capture must shrink fft_length instead (the doctor CLI already
// does).
std::vector<double> windowed_segment_f64(std::span<const float> channel,
                                         int offset, int N,
                                         std::span<const double> win,
                                         bool remove_dc) {
    require(win.size() == static_cast<std::size_t>(N),
            "window coefficient count must match the segment length");
    require(offset >= 0 &&
                static_cast<std::int64_t>(offset) + N <=
                    static_cast<std::int64_t>(channel.size()),
            "analysis segment [analysis_offset, analysis_offset + fft_length) "
            "must lie within the signal — zero-padding a short buffer would "
            "measure the truncation edge, not the signal; use a shorter "
            "fft_length");
    double mean = 0.0;
    if (remove_dc) {
        for (int i = 0; i < N; ++i)
            mean += channel[static_cast<std::size_t>(offset + i)];
        mean /= N;
    }

    std::vector<double> out(static_cast<std::size_t>(N), 0.0);
    for (int i = 0; i < N; ++i)
        out[static_cast<std::size_t>(i)] =
            (channel[static_cast<std::size_t>(offset + i)] - mean) *
            win[static_cast<std::size_t>(i)];
    return out;
}

// Float view of the same segment, for the float-FFT analyzers. The math is
// identical — the double core already computed it and this only narrows the
// result, exactly as the single-precision path always did (that path also
// formed `(sample - mean) * win` in double before casting to float).
std::vector<float> windowed_segment(std::span<const float> channel, int offset,
                                    int N, std::span<const double> win,
                                    bool remove_dc) {
    const auto wide = windowed_segment_f64(channel, offset, N, win, remove_dc);
    return {wide.begin(), wide.end()};
}

// Forward real FFT of a windowed segment → linear magnitudes for bins
// [0, N/2]. The absolute scale is whatever the active FFT backend produces;
// callers must only use ratios of these magnitudes (backend-stable).
std::vector<double> spectrum_magnitude(const std::vector<float>& segment) {
    const int N = static_cast<int>(segment.size());
    pulp::signal::Fft fft(N);
    std::vector<std::complex<float>> freq(static_cast<std::size_t>(N));
    fft.forward_real(segment.data(), freq.data());

    const int bins = N / 2 + 1;
    std::vector<double> mag(static_cast<std::size_t>(bins));
    for (int i = 0; i < bins; ++i)
        mag[static_cast<std::size_t>(i)] =
            std::abs(freq[static_cast<std::size_t>(i)]);
    return mag;
}

int nearest_bin(double hz, double bin_hz, int num_bins) {
    if (bin_hz <= 0.0)
        return 0;
    const int bin = static_cast<int>(std::llround(hz / bin_hz));
    return std::clamp(bin, 0, num_bins - 1);
}

constexpr double kLinearFloor = 1e-12;

} // namespace

std::string window_name(Window w) {
    switch (w) {
        case Window::rectangular: return "rectangular";
        case Window::hann:        return "hann";
        case Window::hamming:     return "hamming";
        case Window::blackman:    return "blackman";
        case Window::flat_top:    return "flat_top";
        case Window::kaiser:      return "kaiser";
    }
    return "rectangular";
}

double window_coherent_gain(Window w, int n, double kaiser_beta) {
    require(n > 0, "window length must be > 0");
    require(w != Window::kaiser || kaiser_beta > 0.0,
            "kaiser_beta must be > 0 for Window::kaiser");

    // rectangular / hann: the exact analytic constant (see the table's comment
    // — this is the bit-exactness contract, not a shortcut).
    const double analytic = analytic_coherent_gain(w);
    if (analytic > 0.0)
        return analytic;

    // Everything else: measure the mean of the actual length-n coefficients, so
    // the DC gain is exactly 1 by construction whatever core/signal's
    // coefficients round to. Length-dependent, which is why this takes `n` —
    // normalizing length-n coefficients by another length's mean would leave
    // the gain slightly off.
    const auto raw = raw_periodic_window(w, n, kaiser_beta);
    return std::accumulate(raw.begin(), raw.end(), 0.0) / n;
}

std::vector<double> window_coefficients(Window w, int n, double kaiser_beta) {
    require(n > 0, "window length must be > 0");
    require(w != Window::kaiser || kaiser_beta > 0.0,
            "kaiser_beta must be > 0 for Window::kaiser");

    auto coeffs = raw_periodic_window(w, n, kaiser_beta);

    // Coherent-gain normalization: divide by the window's DC gain so every
    // window measures a tone at the same level (see the header contract). For
    // rectangular (1.0) and hann (0.5) the divisor is an exact power of two,
    // so those paths stay bit-for-bit identical to the pre-normalization
    // coefficients and every dB/ratio the analyzers report is unchanged.
    const double gain = window_coherent_gain(w, n, kaiser_beta);
    require(gain > 0.0, "window has non-positive coherent gain");
    for (double& c : coeffs)
        c /= gain;
    return coeffs;
}

// ── Magnitude / frequency response ────────────────────────────────────────

double ResponseCurve::magnitude_db_at(double hz) const {
    if (full.empty())
        return kSilenceFloorDb;
    const int bin = nearest_bin(hz, bin_hz, static_cast<int>(full.size()));
    return full[static_cast<std::size_t>(bin)].magnitude_db;
}

ResponseCurve response_relative_to_input(
    const pulp::audio::BufferView<const float>& input,
    const pulp::audio::BufferView<const float>& output, double sample_rate,
    std::span<const double> checkpoints_hz, const ResponseOptions& options) {
    require(is_power_of_two(options.fft_length),
            "response fft_length must be a power of two");
    require(options.analysis_offset >= 0, "analysis_offset must be >= 0");
    require(input.num_channels() > 0 && output.num_channels() > 0,
            "response needs at least one input and output channel");
    const int N = options.fft_length;

    const int out_ch =
        std::clamp(options.channel, 0,
                   static_cast<int>(output.num_channels()) - 1);
    const int in_ch =
        std::clamp(options.channel, 0,
                   static_cast<int>(input.num_channels()) - 1);
    const auto out_channel = output.channel(static_cast<std::size_t>(out_ch));
    const auto in_channel = input.channel(static_cast<std::size_t>(in_ch));

    // Impulse response: no DC removal (the impulse's flat spectrum IS the
    // reference; subtracting the mean would tilt the low bins). The same
    // window is applied to input and output so its shaping cancels in the
    // output/input ratio.
    const auto win =
        window_coefficients(options.window, N, options.kaiser_beta);
    const auto in_seg = windowed_segment(in_channel, options.analysis_offset, N,
                                         win, /*remove_dc=*/false);
    const auto out_seg = windowed_segment(out_channel, options.analysis_offset,
                                          N, win, /*remove_dc=*/false);
    const auto in_mag = spectrum_magnitude(in_seg);
    const auto out_mag = spectrum_magnitude(out_seg);

    // Guard a near-silent reference window. The default stimulus is an impulse
    // at frame 0, so with analysis_offset > 0 the input window [offset, offset+N)
    // no longer contains the impulse → in_mag ≈ 0, and the bin-by-bin division
    // would yield garbage (huge +dB). Require the reference window to carry real
    // energy so a caller can't silently get a meaningless transfer curve. A
    // non-impulse reference (e.g. a decoded WAV) with offset > 0 is fine as long
    // as its window is not empty.
    double in_energy = 0.0;
    for (double m : in_mag)
        in_energy += m * m;
    require(in_energy > kLinearFloor,
            "reference (input) analysis window is effectively silent — an "
            "impulse reference requires analysis_offset == 0; use "
            "magnitude_spectrum_curve for an offset self-spectrum");

    ResponseCurve curve;
    curve.stimulus = "impulse";
    curve.window = options.window;
    curve.fft_length = N;
    curve.analysis_offset = options.analysis_offset;
    curve.sample_rate = sample_rate;
    curve.bin_hz = sample_rate / N;

    const int bins = static_cast<int>(out_mag.size());
    curve.full.reserve(static_cast<std::size_t>(bins));
    for (int i = 0; i < bins; ++i) {
        const double ratio = out_mag[static_cast<std::size_t>(i)] /
                             std::max(in_mag[static_cast<std::size_t>(i)],
                                      kLinearFloor);
        curve.full.push_back({i * curve.bin_hz,
                              20.0 * std::log10(std::max(ratio, kLinearFloor))});
    }

    curve.checkpoints.reserve(checkpoints_hz.size());
    for (double hz : checkpoints_hz)
        curve.checkpoints.push_back({hz, curve.magnitude_db_at(hz)});
    return curve;
}

ResponseCurve magnitude_spectrum_curve(
    const pulp::audio::BufferView<const float>& signal, double sample_rate,
    std::span<const double> checkpoints_hz, const ResponseOptions& options) {
    require(is_power_of_two(options.fft_length),
            "spectrum fft_length must be a power of two");
    require(options.analysis_offset >= 0, "analysis_offset must be >= 0");
    require(signal.num_channels() > 0, "spectrum needs at least one channel");
    const int N = options.fft_length;
    const int ch =
        std::clamp(options.channel, 0,
                   static_cast<int>(signal.num_channels()) - 1);
    const auto channel = signal.channel(static_cast<std::size_t>(ch));
    // DC removed so a constant offset can't dominate; the signal's OWN spectrum
    // (no reference) normalized to its peak bin.
    const auto win =
        window_coefficients(options.window, N, options.kaiser_beta);
    const auto seg = windowed_segment(channel, options.analysis_offset, N, win,
                                      /*remove_dc=*/true);
    const auto mag = spectrum_magnitude(seg);

    ResponseCurve curve;
    curve.analyzer = "magnitude_spectrum";
    curve.stimulus = "self (peak-normalized)";
    curve.window = options.window;
    curve.fft_length = N;
    curve.analysis_offset = options.analysis_offset;
    curve.sample_rate = sample_rate;
    curve.bin_hz = sample_rate / N;

    const int bins = static_cast<int>(mag.size());
    // Peak bin magnitude (exclude DC bin 0, already removed) → the 0 dB anchor.
    double peak = kLinearFloor;
    for (int i = 1; i < bins; ++i)
        peak = std::max(peak, mag[static_cast<std::size_t>(i)]);

    curve.full.reserve(static_cast<std::size_t>(bins));
    for (int i = 0; i < bins; ++i)
        curve.full.push_back(
            {i * curve.bin_hz,
             20.0 * std::log10(std::max(mag[static_cast<std::size_t>(i)],
                                        kLinearFloor) / peak)});

    curve.checkpoints.reserve(checkpoints_hz.size());
    for (double hz : checkpoints_hz)
        curve.checkpoints.push_back({hz, curve.magnitude_db_at(hz)});
    return curve;
}

// ── Phase / group delay ───────────────────────────────────────────────────

namespace {

// Complex spectrum, bins [0, N/2], in double precision. Fft64 also pins the
// portable radix-2 path (the vDSP specialization is float-only), so the phase
// analyzer's numbers do not move with the FFT backend.
std::vector<std::complex<double>> spectrum_complex(
    const std::vector<double>& segment) {
    const int N = static_cast<int>(segment.size());
    pulp::signal::Fft64 fft(N);
    std::vector<std::complex<double>> freq(static_cast<std::size_t>(N));
    fft.forward_real(segment.data(), freq.data());
    freq.resize(static_cast<std::size_t>(N / 2 + 1));
    return freq;
}

// Per-bin group delay of one segment via the ramped-signal identity (see
// measure_group_delay): tau(w) = Re{ X_r * conj(X) } / |X|^2, X_r = FFT(n*x[n]).
// `spec` must be the spectrum of `segment`. Delay is in samples relative to the
// segment's own n = 0; a common offset cancels when two of these are
// subtracted, which is how the transfer group delay is formed.
std::vector<double> segment_group_delay(
    const std::vector<double>& segment,
    const std::vector<std::complex<double>>& spec) {
    std::vector<double> ramped(segment.size());
    for (std::size_t n = 0; n < segment.size(); ++n)
        ramped[n] = segment[n] * static_cast<double>(n);
    const auto ramp_spec = spectrum_complex(ramped);

    std::vector<double> tau(spec.size(), 0.0);
    for (std::size_t k = 0; k < spec.size(); ++k) {
        const double denom = std::norm(spec[k]);
        // A bin with no energy has no phase to differentiate. Zero here is a
        // placeholder only: such bins are gated to `defined = false` by the
        // caller and never surface as a measurement.
        tau[k] = denom > kLinearFloor
                     ? (ramp_spec[k] * std::conj(spec[k])).real() / denom
                     : 0.0;
    }
    return tau;
}

// Standard cumulative-offset unwrap: keep each successive raw-phase difference
// inside (-pi, pi] by accumulating multiples of 2*pi. Exact only while the true
// phase advances less than pi per bin; the aliasing limit that implies is
// documented on measure_group_delay.
std::vector<double> unwrap_phase(const std::vector<std::complex<double>>& h) {
    std::vector<double> phase(h.size(), 0.0);
    constexpr double kPi = std::numbers::pi;
    double offset = 0.0;
    double prev_raw = 0.0;
    for (std::size_t k = 0; k < h.size(); ++k) {
        const double raw = std::arg(h[k]);
        if (k > 0) {
            double delta = raw - prev_raw;
            while (delta > kPi) {
                offset -= 2.0 * kPi;
                delta -= 2.0 * kPi;
            }
            while (delta < -kPi) {
                offset += 2.0 * kPi;
                delta += 2.0 * kPi;
            }
        }
        phase[k] = raw + offset;
        prev_raw = raw;
    }
    return phase;
}

const PhasePoint* nearest_phase_point(const std::vector<PhasePoint>& full,
                                      double hz, double bin_hz) {
    if (full.empty())
        return nullptr;
    return &full[static_cast<std::size_t>(
        nearest_bin(hz, bin_hz, static_cast<int>(full.size())))];
}

} // namespace

bool PhaseCurve::defined_at(double hz) const {
    const auto* p = nearest_phase_point(full, hz, bin_hz);
    return p != nullptr && p->defined;
}

double PhaseCurve::group_delay_samples_at(double hz) const {
    const auto* p = nearest_phase_point(full, hz, bin_hz);
    return p != nullptr ? p->group_delay_samples
                        : std::numeric_limits<double>::quiet_NaN();
}

double PhaseCurve::group_delay_seconds_at(double hz) const {
    if (sample_rate <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return group_delay_samples_at(hz) / sample_rate;
}

double PhaseCurve::phase_radians_at(double hz) const {
    const auto* p = nearest_phase_point(full, hz, bin_hz);
    return p != nullptr ? p->phase_rad
                        : std::numeric_limits<double>::quiet_NaN();
}

double PhaseCurve::magnitude_db_at(double hz) const {
    const auto* p = nearest_phase_point(full, hz, bin_hz);
    return p != nullptr ? p->magnitude_db : kSilenceFloorDb;
}

PhaseCurve measure_group_delay(
    const pulp::audio::BufferView<const float>& input,
    const pulp::audio::BufferView<const float>& output, double sample_rate,
    std::span<const double> checkpoints_hz, const GroupDelayOptions& options) {
    require(is_power_of_two(options.fft_length),
            "group delay fft_length must be a power of two");
    require(options.analysis_offset >= 0, "analysis_offset must be >= 0");
    require(sample_rate > 0.0, "sample_rate must be > 0");
    require(input.num_channels() > 0 && output.num_channels() > 0,
            "group delay needs at least one input and output channel");
    const int N = options.fft_length;

    const int out_ch = std::clamp(options.channel, 0,
                                  static_cast<int>(output.num_channels()) - 1);
    const int in_ch = std::clamp(options.channel, 0,
                                 static_cast<int>(input.num_channels()) - 1);

    // No DC removal, for the same reason as the response analyzer: the impulse
    // reference's flat spectrum IS the reference, and subtracting the segment
    // mean would tilt the low bins. The same window is applied to both sides,
    // so with the default rectangular window the transfer phase is exact.
    // GroupDelayOptions carries no kaiser_beta (it is rectangular-only-exact);
    // the default only matters if a caller deliberately picks Window::kaiser.
    const auto win =
        window_coefficients(options.window, N, kDefaultKaiserBeta);
    const auto in_seg =
        windowed_segment_f64(input.channel(static_cast<std::size_t>(in_ch)),
                             options.analysis_offset, N, win,
                             /*remove_dc=*/false);
    const auto out_seg =
        windowed_segment_f64(output.channel(static_cast<std::size_t>(out_ch)),
                             options.analysis_offset, N, win,
                             /*remove_dc=*/false);
    const auto in_spec = spectrum_complex(in_seg);
    const auto out_spec = spectrum_complex(out_seg);

    // Same guard as the response analyzer: an impulse reference that has fallen
    // outside the analysis window leaves nothing to divide by, and every bin
    // would be noise-over-noise.
    double in_energy = 0.0;
    for (const auto& c : in_spec)
        in_energy += std::norm(c);
    require(in_energy > kLinearFloor,
            "reference (input) analysis window is effectively silent — an "
            "impulse reference requires analysis_offset == 0");

    const auto in_tau = segment_group_delay(in_seg, in_spec);
    const auto out_tau = segment_group_delay(out_seg, out_spec);

    PhaseCurve curve;
    curve.stimulus = "impulse";
    curve.window = options.window;
    curve.fft_length = N;
    curve.analysis_offset = options.analysis_offset;
    curve.sample_rate = sample_rate;
    curve.bin_hz = sample_rate / N;
    curve.magnitude_floor_db = options.magnitude_floor_db;

    const int bins = static_cast<int>(out_spec.size());

    // Transfer function H = output / input, bin by bin. A reference bin without
    // energy makes H undefined there no matter how loud the output is.
    std::vector<std::complex<double>> h(static_cast<std::size_t>(bins));
    std::vector<bool> reference_present(static_cast<std::size_t>(bins), false);
    for (int i = 0; i < bins; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        const bool present = std::norm(in_spec[idx]) > kLinearFloor;
        reference_present[idx] = present;
        h[idx] = present ? out_spec[idx] / in_spec[idx]
                         : std::complex<double>{0.0, 0.0};
    }

    const auto phase = unwrap_phase(h);

    // The gate is relative to the curve's own peak transfer magnitude, so it
    // tracks the passband whatever the processor's overall gain is.
    double peak = kLinearFloor;
    for (int i = 0; i < bins; ++i)
        peak = std::max(peak, std::abs(h[static_cast<std::size_t>(i)]));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    curve.full.reserve(static_cast<std::size_t>(bins));
    for (int i = 0; i < bins; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        const double mag_db =
            20.0 * std::log10(std::max(std::abs(h[idx]), kLinearFloor) / peak);
        const bool defined =
            reference_present[idx] && mag_db >= options.magnitude_floor_db;
        curve.full.push_back(
            {i * curve.bin_hz, mag_db, defined ? phase[idx] : nan,
             defined ? out_tau[idx] - in_tau[idx] : nan, defined});
    }

    curve.checkpoints.reserve(checkpoints_hz.size());
    for (double hz : checkpoints_hz) {
        const auto* p = nearest_phase_point(curve.full, hz, curve.bin_hz);
        PhasePoint cp = p != nullptr ? *p : PhasePoint{};
        cp.hz = hz; // report the requested frequency, not the bin center
        curve.checkpoints.push_back(cp);
    }
    return curve;
}

// ── THD / THD+N ───────────────────────────────────────────────────────────

double ThdResult::thd_db() const { return 20.0 * std::log10(std::max(thd, 1e-10)); }

double ThdResult::thd_plus_n_db() const {
    return 20.0 * std::log10(std::max(thd_plus_n, 1e-10));
}

ThdResult measure_thd(const pulp::audio::BufferView<const float>& signal,
                      double fundamental_hz, double sample_rate,
                      const ThdOptions& options) {
    require(is_power_of_two(options.fft_length),
            "thd fft_length must be a power of two");
    require(options.analysis_offset >= 0, "analysis_offset must be >= 0");
    require(fundamental_hz > 0.0, "fundamental_hz must be > 0");
    require(options.num_harmonics >= 1, "num_harmonics must be >= 1");
    require(signal.num_channels() > 0, "thd needs at least one channel");
    require(sample_rate > 0.0, "sample_rate must be > 0");
    // A "fundamental" at or above Nyquist would clamp to the Nyquist bin and
    // measure nothing at all — thd = 0, i.e. a gate pass fabricated from a
    // caller error. The alias analyzer rejects the same input.
    require(fundamental_hz < sample_rate * 0.5,
            "fundamental_hz must be below Nyquist");
    const int N = options.fft_length;
    const double bin_hz = sample_rate / N;

    // Coherent iff the fundamental lands on an exact bin (within half a bin
    // tolerance is NOT enough — require near-integer cycle count).
    const double cycles = fundamental_hz * N / sample_rate;
    const bool coherent = std::abs(cycles - std::llround(cycles)) < 1e-6;
    // A coherent tone leaks nothing through a rectangular window, so no taper
    // can improve on it. Only the non-coherent path has a leakage floor to
    // choose, and the caller owns that choice.
    const Window window =
        coherent ? Window::rectangular : options.non_coherent_window;

    const int channel =
        std::clamp(options.channel, 0,
                   static_cast<int>(signal.num_channels()) - 1);
    const auto out_channel = signal.channel(static_cast<std::size_t>(channel));
    const auto win = window_coefficients(window, N, options.kaiser_beta);
    const auto seg = windowed_segment(out_channel, options.analysis_offset, N,
                                      win, /*remove_dc=*/true);
    const auto mag = spectrum_magnitude(seg);
    const int bins = static_cast<int>(mag.size());

    // Bin energy of the whole spectrum (for THD+N denominator: everything
    // except the fundamental bin). Bin 0 (DC) is excluded — we removed DC.
    double total_energy = 0.0;
    for (int i = 1; i < bins; ++i)
        total_energy += mag[static_cast<std::size_t>(i)] *
                        mag[static_cast<std::size_t>(i)];

    ThdResult thd;
    thd.fundamental_hz = fundamental_hz;
    thd.window = window;
    thd.fft_length = N;
    thd.analysis_offset = options.analysis_offset;
    thd.sample_rate = sample_rate;
    thd.bin_hz = bin_hz;
    thd.coherent = coherent;
    thd.num_harmonics = options.num_harmonics;

    const int fund_bin = nearest_bin(fundamental_hz, bin_hz, bins);
    // DC was removed, so a fundamental that resolves to bin 0 has no signal to
    // measure against and would make thd/thd+n divide by a near-zero floor.
    require(fund_bin >= 1, "fundamental_hz must resolve to a non-DC bin");
    const double fund_mag = mag[static_cast<std::size_t>(fund_bin)];
    // Every ratio below is relative to the fundamental. With nothing there —
    // a silent buffer, or a tone that is not where the caller said — thd
    // would come back 0 and a "thd below X" gate would PASS on a dead
    // processor. Refuse instead, as the alias analyzer does.
    require(fund_mag > kLinearFloor,
            "no energy at the fundamental — thd and thd+n are relative to it");
    thd.harmonics.push_back({1, fundamental_hz, fund_mag, 0.0});

    double harmonic_sq = 0.0;
    for (int h = 2; h <= options.num_harmonics + 1; ++h) {
        const double harm_hz = fundamental_hz * h;
        if (harm_hz >= sample_rate / 2.0)
            break; // above Nyquist: would alias, not a real harmonic bin
        const int bin = nearest_bin(harm_hz, bin_hz, bins);
        const double hmag = mag[static_cast<std::size_t>(bin)];
        harmonic_sq += hmag * hmag;
        const double db = 20.0 * std::log10(std::max(hmag, kLinearFloor) /
                                            std::max(fund_mag, kLinearFloor));
        thd.harmonics.push_back({h, harm_hz, hmag, db});
    }

    const double fund_safe = std::max(fund_mag, kLinearFloor);
    thd.thd = std::sqrt(harmonic_sq) / fund_safe;
    const double non_fundamental =
        std::max(total_energy - fund_mag * fund_mag, 0.0);
    thd.thd_plus_n = std::sqrt(non_fundamental) / fund_safe;
    return thd;
}

// ── Tone projection ───────────────────────────────────────────────────────
//
// FFT-free from here down. See the header for why: a fit has no leakage skirt,
// so it measures what a window can only bury.

namespace {

// A tone's two basis vectors over an M-sample segment, sin first then cos.
// Phase runs from n = 0 at the segment start; since both quadratures are
// fitted, the 2-D subspace they span is identical for any start offset, so the
// amplitude and the residual do not depend on where the caller's segment began
// (only the sin/cos split rotates).
void tone_basis(std::vector<double>& sine, std::vector<double>& cosine,
                double cycles_per_sample, int M) {
    sine.resize(static_cast<std::size_t>(M));
    cosine.resize(static_cast<std::size_t>(M));
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    for (int n = 0; n < M; ++n) {
        const double phase = kTwoPi * cycles_per_sample * n;
        sine[static_cast<std::size_t>(n)] = std::sin(phase);
        cosine[static_cast<std::size_t>(n)] = std::cos(phase);
    }
}

// Solve G·a = b for a symmetric positive-definite G (row-major, n×n) by
// Gaussian elimination with partial pivoting, in place. Returns false when a
// pivot collapses, i.e. the basis was not linearly independent after all — the
// caller must treat that as "cannot measure", never as a zero.
//
// G is formed from UNIT-NORM columns, so its diagonal starts at exactly 1 and
// `pivot_eps` is a scale-free collinearity threshold rather than a magic
// number tied to the signal's level.
bool solve_normal_equations(std::vector<double>& G, std::vector<double>& b,
                            int n, double pivot_eps) {
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
            if (std::abs(G[static_cast<std::size_t>(r * n + col)]) >
                std::abs(G[static_cast<std::size_t>(pivot * n + col)]))
                pivot = r;
        if (std::abs(G[static_cast<std::size_t>(pivot * n + col)]) < pivot_eps)
            return false;
        if (pivot != col) {
            for (int c = 0; c < n; ++c)
                std::swap(G[static_cast<std::size_t>(col * n + c)],
                          G[static_cast<std::size_t>(pivot * n + c)]);
            std::swap(b[static_cast<std::size_t>(col)],
                      b[static_cast<std::size_t>(pivot)]);
        }
        const double diag = G[static_cast<std::size_t>(col * n + col)];
        for (int r = col + 1; r < n; ++r) {
            const double factor =
                G[static_cast<std::size_t>(r * n + col)] / diag;
            if (factor == 0.0)
                continue;
            for (int c = col; c < n; ++c)
                G[static_cast<std::size_t>(r * n + c)] -=
                    factor * G[static_cast<std::size_t>(col * n + c)];
            b[static_cast<std::size_t>(r)] -=
                factor * b[static_cast<std::size_t>(col)];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double sum = b[static_cast<std::size_t>(r)];
        for (int c = r + 1; c < n; ++c)
            sum -= G[static_cast<std::size_t>(r * n + c)] *
                   b[static_cast<std::size_t>(c)];
        b[static_cast<std::size_t>(r)] =
            sum / G[static_cast<std::size_t>(r * n + r)];
    }
    return true;
}

// Joint least-squares fit of K tones at `cycles` to `x`, plus a constant.
//
// Jointly, not one site at a time. The usual justification — "otherwise the
// loud fundamental contaminates the quiet alias" — is NOT the reason, and is
// in fact false: for two tones with basis correlation p, fitting the loud one
// first and the quiet one on the residual gives a2*(1 - p^2), in which the
// loud amplitude has cancelled exactly. Sequential fitting is well behaved.
//
// The real reasons are narrower and both matter here:
//   * It is exact rather than approximate. Sequential fitting is exact only for
//     an orthogonal basis; its p^2 error grows as sites approach the resolution
//     limit, and this analyzer deliberately admits sites only 2 bins apart.
//   * It FAILS LOUDLY. A joint solve's pivot collapses when the segment genuinely
//     cannot separate two sites, which is what lets the caller be told "cannot
//     measure" instead of being handed a number. A sequential pass has no such
//     signal — it always returns something.
//
// The constant column is load-bearing and NOT a substitute for subtracting the
// mean up front. Subtracting the mean is the least-squares fit of a constant
// ALONE, which ignores that the tones are also non-orthogonal to a constant
// over a finite segment; it therefore removes the wrong constant and leaves a
// DC pedestal in the residual that no tone column can absorb. Measured, that
// pedestal put the residual at -69 dBc on a fixture whose true residual was
// float quantization ~80 dB lower, and it wrecked the detection floor with it.
// Fitting the constant jointly removes exactly the right one.
struct MultiToneFit {
    std::vector<double> amplitude; ///< Per tone, input units.
    std::vector<double> residual;  ///< x minus every fitted tone and the constant.
    bool conditioned = false;
};

MultiToneFit fit_tone_set(std::span<const double> x,
                          std::span<const double> cycles, double pivot_eps) {
    const int M = static_cast<int>(x.size());
    const int K = static_cast<int>(cycles.size());
    // Two columns per tone (sin, cos) plus one constant column, which lives
    // last so the per-tone indexing stays 2k / 2k+1.
    const int n = 2 * K + 1;

    MultiToneFit fit;
    fit.amplitude.assign(static_cast<std::size_t>(K), 0.0);
    fit.residual.assign(x.begin(), x.end());
    if (K == 0) {
        fit.conditioned = true;
        return fit;
    }

    // Materialize the design matrix column-by-column and normalize each column
    // to unit norm. Normalizing is not cosmetic: the normal equations square
    // the condition number, and unit columns are what keep that squaring
    // harmless for the well-separated sites this analyzer fits.
    std::vector<double> columns(static_cast<std::size_t>(n) *
                                static_cast<std::size_t>(M));
    std::vector<double> scale(static_cast<std::size_t>(n), 0.0);
    std::vector<double> sine, cosine;
    for (int k = 0; k < K; ++k) {
        tone_basis(sine, cosine, cycles[static_cast<std::size_t>(k)], M);
        for (int q = 0; q < 2; ++q) {
            const auto& src = (q == 0) ? sine : cosine;
            const int col = 2 * k + q;
            double norm_sq = 0.0;
            for (int i = 0; i < M; ++i)
                norm_sq += src[static_cast<std::size_t>(i)] *
                           src[static_cast<std::size_t>(i)];
            const double norm = std::sqrt(norm_sq);
            // A zero-norm column means a degenerate basis (sin at DC or at
            // exactly Nyquist). The caller screens those out before we get
            // here; bail rather than divide by zero if one slips through.
            if (!(norm > 0.0))
                return fit;
            scale[static_cast<std::size_t>(col)] = norm;
            for (int i = 0; i < M; ++i)
                columns[static_cast<std::size_t>(col) *
                            static_cast<std::size_t>(M) +
                        static_cast<std::size_t>(i)] =
                    src[static_cast<std::size_t>(i)] / norm;
        }
    }
    const int dc_col = 2 * K;
    scale[static_cast<std::size_t>(dc_col)] = std::sqrt(static_cast<double>(M));
    for (int i = 0; i < M; ++i)
        columns[static_cast<std::size_t>(dc_col) * static_cast<std::size_t>(M) +
                static_cast<std::size_t>(i)] =
            1.0 / std::sqrt(static_cast<double>(M));

    std::vector<double> G(static_cast<std::size_t>(n) *
                          static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        for (int j = i; j < n; ++j) {
            double sum = 0.0;
            const double* ci =
                &columns[static_cast<std::size_t>(i) *
                         static_cast<std::size_t>(M)];
            const double* cj =
                &columns[static_cast<std::size_t>(j) *
                         static_cast<std::size_t>(M)];
            for (int t = 0; t < M; ++t)
                sum += ci[t] * cj[t];
            G[static_cast<std::size_t>(i * n + j)] = sum;
            G[static_cast<std::size_t>(j * n + i)] = sum;
        }
    }

    std::vector<double> rhs(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        double sum = 0.0;
        const double* ci =
            &columns[static_cast<std::size_t>(i) * static_cast<std::size_t>(M)];
        for (int t = 0; t < M; ++t)
            sum += ci[t] * x[static_cast<std::size_t>(t)];
        rhs[static_cast<std::size_t>(i)] = sum;
    }

    if (!solve_normal_equations(G, rhs, n, pivot_eps))
        return fit;

    // rhs now holds the unit-column coefficients; subtract each fitted tone
    // from the residual and un-scale back to input units for the amplitudes.
    for (int i = 0; i < n; ++i) {
        const double coeff = rhs[static_cast<std::size_t>(i)];
        const double* ci =
            &columns[static_cast<std::size_t>(i) * static_cast<std::size_t>(M)];
        for (int t = 0; t < M; ++t)
            fit.residual[static_cast<std::size_t>(t)] -= coeff * ci[t];
    }
    for (int k = 0; k < K; ++k) {
        const double s = rhs[static_cast<std::size_t>(2 * k)] /
                         scale[static_cast<std::size_t>(2 * k)];
        const double c = rhs[static_cast<std::size_t>(2 * k + 1)] /
                         scale[static_cast<std::size_t>(2 * k + 1)];
        fit.amplitude[static_cast<std::size_t>(k)] = std::hypot(s, c);
    }
    fit.conditioned = true;
    return fit;
}

double rms_of(std::span<const double> v) {
    if (v.empty())
        return 0.0;
    double sum = 0.0;
    for (double s : v)
        sum += s * s;
    return std::sqrt(sum / static_cast<double>(v.size()));
}

double ratio_to_db(double numerator, double denominator) {
    if (!(denominator > 0.0))
        return kSilenceFloorDb;
    const double ratio = numerator / denominator;
    if (!(ratio > 0.0))
        return kSilenceFloorDb;
    return std::max(20.0 * std::log10(ratio), kSilenceFloorDb);
}

} // namespace

ToneFit fit_tone(std::span<const double> samples, double cycles_per_sample) {
    require(samples.size() >= 4, "tone fit needs at least 4 samples");
    // Fold the requested frequency into [0, 0.5] first: sampling has already
    // done exactly that to the signal, so fitting 0.6 cycles/sample means
    // fitting the 0.4 that is actually there. Doing it here rather than
    // rejecting it keeps the alias analyzer's fold sites and this entry point
    // speaking the same language.
    const double f = fold_frequency(cycles_per_sample, 1.0);
    require(f > 0.0 && f < 0.5,
            "cycles_per_sample must fold strictly between DC and Nyquist — a "
            "tone at exactly 0 or 0.5 has no sin quadrature, so its amplitude "
            "and phase cannot be separated");

    const int M = static_cast<int>(samples.size());
    std::vector<double> sine, cosine;
    tone_basis(sine, cosine, f, M);

    double sin_projection = 0.0, cos_projection = 0.0;
    double sin_energy = 0.0, cos_energy = 0.0, cross_energy = 0.0;
    for (int i = 0; i < M; ++i) {
        const auto u = static_cast<std::size_t>(i);
        sin_projection += samples[u] * sine[u];
        cos_projection += samples[u] * cosine[u];
        sin_energy += sine[u] * sine[u];
        cos_energy += cosine[u] * cosine[u];
        cross_energy += sine[u] * cosine[u];
    }
    const double determinant =
        sin_energy * cos_energy - cross_energy * cross_energy;
    require(std::abs(determinant) > 0.0,
            "tone fit basis is degenerate over this segment");

    ToneFit fit;
    fit.cycles_per_sample = f;
    fit.sin_gain =
        (sin_projection * cos_energy - cos_projection * cross_energy) /
        determinant;
    fit.cos_gain =
        (cos_projection * sin_energy - sin_projection * cross_energy) /
        determinant;
    fit.amplitude = std::hypot(fit.sin_gain, fit.cos_gain);

    for (int i = 0; i < M; ++i) {
        const auto u = static_cast<std::size_t>(i);
        const double fitted = fit.sin_gain * sine[u] + fit.cos_gain * cosine[u];
        fit.fitted_energy += fitted * fitted;
        const double residual = samples[u] - fitted;
        fit.residual_energy += residual * residual;
    }
    return fit;
}

double tone_residual_db(std::span<const double> samples,
                        double cycles_per_sample) {
    const auto fit = fit_tone(samples, cycles_per_sample);
    if (!(fit.fitted_energy > 0.0))
        return kSilenceFloorDb;
    return std::max(10.0 * std::log10(std::max(fit.residual_energy, 0.0) /
                                      fit.fitted_energy),
                    kSilenceFloorDb);
}

double tone_gain_db(std::span<const double> samples, double cycles_per_sample,
                    double input_amplitude) {
    require(input_amplitude > 0.0, "input_amplitude must be > 0");
    const auto fit = fit_tone(samples, cycles_per_sample);
    return ratio_to_db(fit.amplitude, input_amplitude);
}

// ── Alias / image analysis ────────────────────────────────────────────────

double fold_frequency(double frequency_hz, double sample_rate) {
    require(sample_rate > 0.0, "sample_rate must be > 0");
    // |f| mod fs lands in [0, fs); everything above Nyquist mirrors down. One
    // mod plus one mirror is the closed form of "reflect repeatedly": the map
    // is periodic in fs and even about both 0 and fs/2, so a component 1.67·fs
    // up wraps to 0.67·fs and mirrors to 0.33·fs in a single step.
    double f = std::fmod(std::abs(frequency_hz), sample_rate);
    if (f > sample_rate * 0.5)
        f = sample_rate - f;
    return f;
}

AliasReport measure_aliasing(const pulp::audio::BufferView<const float>& signal,
                             double fundamental_hz, double sample_rate,
                             const AliasOptions& options) {
    require(sample_rate > 0.0, "sample_rate must be > 0");
    require(fundamental_hz > 0.0, "fundamental_hz must be > 0");
    require(fundamental_hz < sample_rate * 0.5,
            "fundamental_hz must be below Nyquist");
    require(options.num_harmonics >= 1, "num_harmonics must be >= 1");
    require(options.analysis_offset >= 0, "analysis_offset must be >= 0");
    require(options.analysis_length >= 0, "analysis_length must be >= 0");
    require(signal.num_channels() > 0, "alias analysis needs at least one channel");
    require(options.min_separation_bins > 0.0, "min_separation_bins must be > 0");
    const double nyquist = sample_rate * 0.5;
    require(options.max_alias_frequency_hz > 0.0 &&
                options.max_alias_frequency_hz <= nyquist,
            "max_alias_frequency_hz must be in (0, Nyquist] — a band qualifier "
            "at or above Nyquist is a full-band gate, which no finite "
            "anti-aliasing method can pass");
    // When every modeled harmonic sits below Nyquist the series contains no
    // fold site at all: whatever aliasing the signal actually has lands in
    // noise_db, worst_alias_db stays at the silence floor, and a gate reads
    // "clean" from a measurement that modeled nowhere to look — on a naive
    // 300 Hz saw under default options the report came back spotless. Refuse
    // outright rather than hand that report to a caller.
    require(fundamental_hz * options.num_harmonics >= nyquist,
            "no harmonic of the requested series reaches Nyquist, so there is "
            "no alias site to measure and any report would read as clean on "
            "any signal — raise num_harmonics (num_harmonics * fundamental_hz "
            "should be several times the sample rate for a saw or pulse)");

    const int channel =
        std::clamp(options.channel, 0,
                   static_cast<int>(signal.num_channels()) - 1);
    const auto source = signal.channel(static_cast<std::size_t>(channel));
    const int available =
        static_cast<int>(source.size()) - options.analysis_offset;
    require(available > 0, "analysis_offset is past the end of the signal");
    const int M = options.analysis_length > 0
                      ? std::min(options.analysis_length, available)
                      : available;

    AliasReport report;
    report.fundamental_hz = fundamental_hz;
    report.sample_rate = sample_rate;
    report.nyquist_hz = nyquist;
    report.max_alias_frequency_hz = options.max_alias_frequency_hz;
    report.num_harmonics = options.num_harmonics;
    report.analysis_offset = options.analysis_offset;
    report.analysis_length = M;
    report.bin_hz = sample_rate / M;

    // Resolution of a fit this long. Two sites closer than this are not
    // linearly independent over the segment, so they get marked unresolved
    // rather than fitted to a number the data cannot support.
    const double min_separation_hz = options.min_separation_bins * report.bin_hz;
    require(nyquist > 2.0 * min_separation_hz,
            "analysis segment is too short to resolve anything between DC and "
            "Nyquist — supply more samples");

    // Copy the segment out as double. The segment mean is subtracted only to
    // keep the values centered for the fit; the constant that actually matters
    // is solved for jointly with the tones (see fit_tone_set), because the mean
    // alone is the wrong constant and leaves a DC pedestal in the residual. A
    // component folding to exactly 0 Hz is indistinguishable from a DC offset
    // either way, and is screened out as unresolved below.
    std::vector<double> x(static_cast<std::size_t>(M));
    double mean = 0.0;
    for (int i = 0; i < M; ++i)
        mean += source[static_cast<std::size_t>(options.analysis_offset + i)];
    mean /= M;
    for (int i = 0; i < M; ++i)
        x[static_cast<std::size_t>(i)] =
            source[static_cast<std::size_t>(options.analysis_offset + i)] - mean;

    // Build the landing sites: every h up to num_harmonics, labeled by where
    // h·f0 falls relative to Nyquist and placed at its fold site. A site is
    // fitted only if it is separable from DC, from Nyquist, and from every site
    // already accepted — the fundamental is enumerated first, so on a collision
    // the LOWER harmonic keeps the site and the higher one is marked unresolved
    // (attributing shared energy to the lower harmonic is the conservative
    // reading: it under-reports aliasing rather than inventing it).
    report.components.reserve(static_cast<std::size_t>(options.num_harmonics));
    std::vector<double> cycles;   // Fitted sites, in cycles/sample.
    std::vector<int> fit_of;      // cycles index -> components index.
    for (int h = 1; h <= options.num_harmonics; ++h) {
        AliasComponent c;
        c.index = h;
        c.source_hz = fundamental_hz * h;
        c.component_class = c.source_hz < nyquist ? ComponentClass::harmonic
                                                  : ComponentClass::alias;
        c.hz = c.component_class == ComponentClass::harmonic
                   ? c.source_hz
                   : fold_frequency(c.source_hz, sample_rate);
        c.in_band = c.component_class == ComponentClass::harmonic ||
                    c.hz <= options.max_alias_frequency_hz;

        bool separable = c.hz > min_separation_hz &&
                         c.hz < nyquist - min_separation_hz;
        if (separable) {
            for (double already : cycles) {
                if (std::abs(already * sample_rate - c.hz) < min_separation_hz) {
                    separable = false;
                    break;
                }
            }
        }
        c.resolved = separable;
        if (separable) {
            cycles.push_back(c.hz / sample_rate);
            fit_of.push_back(static_cast<int>(report.components.size()));
        }
        report.components.push_back(c);
    }
    require(!cycles.empty(),
            "no component of this series is separable over the supplied "
            "segment");
    // The whole report is expressed relative to the fundamental, so a series
    // whose fundamental itself could not be fitted has no reference at all.
    require(report.components.front().resolved,
            "the fundamental's own site is not separable — every dB here is "
            "relative to it");
    require(static_cast<int>(cycles.size()) * 2 < M,
            "analysis segment is too short for the requested harmonic count "
            "(the fit needs more samples than unknowns)");

    // Conditioning threshold on unit-norm columns. Sites separated by >= 2/T
    // are near-orthogonal, so a healthy pivot is O(1); 1e-9 flags a basis that
    // collapsed despite the separation screen rather than trimming a good fit.
    constexpr double kPivotEps = 1e-9;
    const auto fit = fit_tone_set(x, cycles, kPivotEps);
    require(fit.conditioned,
            "the harmonic/fold basis collapsed over this segment — the fit "
            "cannot be trusted, so no alias figure is reported");

    for (std::size_t k = 0; k < fit_of.size(); ++k) {
        auto& c = report.components[static_cast<std::size_t>(fit_of[k])];
        c.amplitude = fit.amplitude[k];
    }
    report.fundamental_amplitude = report.components.front().amplitude;
    require(report.fundamental_amplitude > 0.0,
            "no energy at the fundamental — every dB in this report is "
            "relative to it");

    for (auto& c : report.components) {
        if (!c.resolved)
            continue;
        c.db_below_fundamental =
            ratio_to_db(c.amplitude, report.fundamental_amplitude);
    }

    for (const auto& c : report.components) {
        if (c.component_class != ComponentClass::alias)
            continue;
        if (!c.resolved) {
            if (c.in_band)
                report.has_unresolved_in_band_alias = true;
            continue;
        }
        if (c.db_below_fundamental > report.full_band_worst_alias_db) {
            report.full_band_worst_alias_db = c.db_below_fundamental;
            report.full_band_worst_alias_hz = c.hz;
        }
        if (c.in_band && c.db_below_fundamental > report.worst_alias_db) {
            report.worst_alias_db = c.db_below_fundamental;
            report.worst_alias_hz = c.hz;
            report.worst_alias_index = c.index;
        }
    }

    // Noise: whatever the expected series could not account for.
    const double residual_rms = rms_of(fit.residual);
    const double fundamental_rms =
        report.fundamental_amplitude / std::numbers::sqrt2_v<double>;
    report.noise_db = ratio_to_db(residual_rms, fundamental_rms);

    // Detection floor. Fitting a tone to a residual of RMS σ over M samples
    // yields a spurious amplitude whose per-quadrature standard deviation is
    // σ·√(2/M); the amplitude is the quadrature magnitude, so ≈2σ·√(2/M) is a
    // ~2-sigma bound on what this fit could mistake for a component — under a
    // WHITE-residual assumption. See the header for why that makes this
    // number an inconclusiveness check, never a gate assertion.
    const double floor_amplitude =
        2.0 * residual_rms * std::sqrt(2.0 / static_cast<double>(M));
    report.detection_floor_db =
        ratio_to_db(floor_amplitude, report.fundamental_amplitude);
    return report;
}

} // namespace pulp::test::audio
