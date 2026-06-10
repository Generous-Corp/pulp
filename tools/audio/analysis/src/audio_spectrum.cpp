// audio_spectrum.cpp — buffer-level offline spectrum analyzers (magnitude
// response + THD/THD+N). The per-analyzer determinism contracts live in
// audio_spectrum.hpp; keep the window/leakage/coherence behavior here in
// lock-step with them.

#include <pulp/audio/analysis/audio_spectrum.hpp>

#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

namespace pulp::test::audio {

namespace {

void require(bool ok, const char* what) {
    if (!ok)
        throw std::invalid_argument(std::string("AudioSpectrum: ") + what);
}

bool is_power_of_two(int n) { return n > 0 && (n & (n - 1)) == 0; }

// Window weight at sample n of N (periodic definition, matching the FFT's
// expectation that the segment wraps). Documented in audio_spectrum.hpp.
double window_weight(Window w, int n, int N) {
    switch (w) {
        case Window::rectangular:
            return 1.0;
        case Window::hann:
            return 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / N);
    }
    return 1.0;
}

// Extract one channel segment [offset, offset+N) into a windowed float buffer
// ready for forward_real(). DC is removed (segment mean subtracted) when
// `remove_dc` is true — true for steady tones, but false for an impulse-
// response segment, where subtracting the mean would add a constant pedestal
// across the whole window and bias the low bins. Out-of-range frames read as
// zero.
std::vector<float> windowed_segment(std::span<const float> channel, int offset,
                                    int N, Window w, bool remove_dc) {
    double mean = 0.0;
    if (remove_dc) {
        int counted = 0;
        for (int i = 0; i < N; ++i) {
            const int idx = offset + i;
            if (idx >= 0 && idx < static_cast<int>(channel.size())) {
                mean += channel[static_cast<std::size_t>(idx)];
                ++counted;
            }
        }
        if (counted > 0)
            mean /= counted;
    }

    std::vector<float> out(static_cast<std::size_t>(N), 0.0f);
    for (int i = 0; i < N; ++i) {
        const int idx = offset + i;
        const double sample =
            (idx >= 0 && idx < static_cast<int>(channel.size()))
                ? channel[static_cast<std::size_t>(idx)] - mean
                : 0.0;
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(sample * window_weight(w, i, N));
    }
    return out;
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
    return w == Window::rectangular ? "rectangular" : "hann";
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
    const auto in_seg = windowed_segment(in_channel, options.analysis_offset, N,
                                         options.window, /*remove_dc=*/false);
    const auto out_seg = windowed_segment(out_channel, options.analysis_offset,
                                          N, options.window, /*remove_dc=*/false);
    const auto in_mag = spectrum_magnitude(in_seg);
    const auto out_mag = spectrum_magnitude(out_seg);

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
    const auto seg = windowed_segment(channel, options.analysis_offset, N,
                                      options.window, /*remove_dc=*/true);
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
    const int N = options.fft_length;
    const double bin_hz = sample_rate / N;

    // Coherent iff the fundamental lands on an exact bin (within half a bin
    // tolerance is NOT enough — require near-integer cycle count).
    const double cycles = fundamental_hz * N / sample_rate;
    const bool coherent = std::abs(cycles - std::llround(cycles)) < 1e-6;
    const Window window = coherent ? Window::rectangular : Window::hann;

    const int channel =
        std::clamp(options.channel, 0,
                   static_cast<int>(signal.num_channels()) - 1);
    const auto out_channel = signal.channel(static_cast<std::size_t>(channel));
    const auto seg = windowed_segment(out_channel, options.analysis_offset, N,
                                      window, /*remove_dc=*/true);
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
    const double fund_mag = mag[static_cast<std::size_t>(fund_bin)];
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

} // namespace pulp::test::audio
