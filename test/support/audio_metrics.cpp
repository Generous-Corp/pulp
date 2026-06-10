// audio_metrics.cpp — deterministic offline signal metrics (harness PR 1A).
// See audio_metrics.hpp for the analyzer determinism contract.

#include "audio_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace pulp::test::audio {

double to_dbfs(double linear) {
    if (!(linear > 0.0))
        return kSilenceFloorDb;
    return std::max(kSilenceFloorDb, 20.0 * std::log10(linear));
}

double from_dbfs(double dbfs) {
    return std::pow(10.0, dbfs / 20.0);
}

bool BufferMetrics::has_nan_or_inf() const {
    for (const auto& ch : channels)
        if (ch.nan_samples > 0 || ch.inf_samples > 0)
            return true;
    return false;
}

double BufferMetrics::max_peak() const {
    double peak = 0.0;
    for (const auto& ch : channels)
        peak = std::max(peak, ch.peak);
    return peak;
}

double BufferMetrics::max_rms() const {
    double rms = 0.0;
    for (const auto& ch : channels)
        rms = std::max(rms, ch.rms);
    return rms;
}

std::uint64_t BufferMetrics::total_clipped_samples() const {
    std::uint64_t total = 0;
    for (const auto& ch : channels)
        total += ch.clipped_samples;
    return total;
}

BufferMetrics analyze(const pulp::audio::BufferView<const float>& buffer,
                      double sample_rate,
                      const AnalyzeOptions& options) {
    BufferMetrics metrics;
    metrics.num_channels = static_cast<int>(buffer.num_channels());
    metrics.num_frames = static_cast<int>(buffer.num_samples());
    metrics.sample_rate = sample_rate;
    metrics.channels.resize(buffer.num_channels());

    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        auto span = buffer.channel(ch);
        auto& out = metrics.channels[ch];

        // Double accumulators keep RMS/DC stable across long buffers.
        double sum_squares = 0.0;
        double sum = 0.0;
        std::uint64_t current_silence_run = 0;

        for (float sample : span) {
            if (std::isnan(sample)) {
                ++out.nan_samples;
                continue; // NaN poisons accumulators; count it, skip it.
            }
            if (std::isinf(sample)) {
                ++out.inf_samples;
                continue;
            }
            const double s = sample;
            const double mag = std::abs(s);
            out.peak = std::max(out.peak, mag);
            sum_squares += s * s;
            sum += s;
            if (mag >= options.clip_threshold)
                ++out.clipped_samples;
            if (mag < options.silence_threshold) {
                ++current_silence_run;
                out.longest_silence_run =
                    std::max(out.longest_silence_run, current_silence_run);
            } else {
                current_silence_run = 0;
            }
        }

        const auto finite =
            span.size() - out.nan_samples - out.inf_samples;
        if (finite > 0) {
            out.rms = std::sqrt(sum_squares / static_cast<double>(finite));
            out.dc_offset = sum / static_cast<double>(finite);
        }
    }
    return metrics;
}

BufferMetrics analyze(const pulp::audio::Buffer<float>& buffer,
                      double sample_rate,
                      const AnalyzeOptions& options) {
    // Buffer has no const view(); build one. Offline-only code, so the
    // temporary pointer vector is acceptable.
    std::vector<const float*> ptrs(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch)
        ptrs[ch] = buffer.channel(ch).data();
    pulp::audio::BufferView<const float> view(
        ptrs.data(), buffer.num_channels(), buffer.num_samples());
    return analyze(view, sample_rate, options);
}

FrequencyEstimate estimate_frequency(std::span<const float> samples,
                                     double sample_rate) {
    // Positive-going zero crossings with linear interpolation; see header.
    std::vector<double> crossings;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const float prev = samples[i - 1];
        const float curr = samples[i];
        if (std::isnan(prev) || std::isnan(curr))
            continue;
        if (prev < 0.0f && curr >= 0.0f) {
            const double denom = static_cast<double>(curr) - prev;
            const double frac = denom != 0.0 ? -prev / denom : 0.0;
            crossings.push_back(static_cast<double>(i - 1) + frac);
        }
    }
    if (crossings.size() < 3)
        return {};

    std::vector<double> periods(crossings.size() - 1);
    for (std::size_t i = 1; i < crossings.size(); ++i)
        periods[i - 1] = crossings[i] - crossings[i - 1];

    const double mean =
        std::accumulate(periods.begin(), periods.end(), 0.0) /
        static_cast<double>(periods.size());
    if (mean <= 0.0)
        return {};

    double variance = 0.0;
    for (double p : periods)
        variance += (p - mean) * (p - mean);
    variance /= static_cast<double>(periods.size());
    const double jitter = std::sqrt(variance) / mean;

    FrequencyEstimate estimate;
    estimate.hz = sample_rate / mean;
    estimate.confidence = std::clamp(1.0 - jitter, 0.0, 1.0);
    return estimate;
}

std::string summarize(const BufferMetrics& metrics,
                      const FrequencyEstimate& frequency) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);

    const double seconds = metrics.sample_rate > 0.0
        ? metrics.num_frames / metrics.sample_rate : 0.0;
    out << "Signal summary:\n";
    out << "  duration: " << std::setprecision(3) << seconds << " s ("
        << metrics.num_frames << " frames @ "
        << std::setprecision(0) << metrics.sample_rate << " Hz)\n";
    out << "  channels: " << metrics.num_channels << "\n";
    out << std::setprecision(1);
    out << "  level: peak " << to_dbfs(metrics.max_peak()) << " dBFS, RMS "
        << to_dbfs(metrics.max_rms()) << " dBFS\n";
    if (frequency.hz > 0.0) {
        out << "  dominant pitch: " << frequency.hz << " Hz (confidence "
            << std::setprecision(2) << frequency.confidence << ")\n"
            << std::setprecision(1);
    }
    for (std::size_t ch = 0; ch < metrics.channels.size(); ++ch) {
        const auto& c = metrics.channels[ch];
        out << "  ch" << ch << ": peak " << c.peak_dbfs() << " dBFS, RMS "
            << c.rms_dbfs() << " dBFS, DC " << std::setprecision(4)
            << c.dc_offset << std::setprecision(1);
        if (c.clipped_samples > 0)
            out << ", clipped " << c.clipped_samples;
        if (c.nan_samples > 0)
            out << ", NaN " << c.nan_samples;
        if (c.inf_samples > 0)
            out << ", Inf " << c.inf_samples;
        if (c.longest_silence_run > 0)
            out << ", longest silence run " << c.longest_silence_run;
        out << "\n";
    }
    return out.str();
}

} // namespace pulp::test::audio
