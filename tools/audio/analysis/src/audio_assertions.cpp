// audio_assertions.cpp — reusable signal assertions (harness PR 1A).
// See audio_assertions.hpp for the message and tolerance policy.

#include <pulp/audio/analysis/audio_assertions.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace pulp::test::audio {
namespace {

std::string format_db(double dbfs) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    if (dbfs <= kSilenceFloorDb) {
        out << "-inf dBFS";
    } else {
        out << dbfs << " dBFS";
    }
    return out.str();
}

} // namespace

CheckResult assert_no_nan_inf(const BufferMetrics& metrics) {
    CheckResult result;
    std::ostringstream msg;
    result.passed = true;
    for (std::size_t ch = 0; ch < metrics.channels.size(); ++ch) {
        const auto& c = metrics.channels[ch];
        if (c.nan_samples > 0 || c.inf_samples > 0) {
            result.passed = false;
            msg << "ch" << ch << " has " << c.nan_samples << " NaN and "
                << c.inf_samples << " Inf samples of " << metrics.num_frames
                << ". ";
        }
    }
    result.message = result.passed ? "no NaN/Inf samples" : msg.str();
    return result;
}

CheckResult assert_not_clipped(const BufferMetrics& metrics,
                               double ceiling_dbfs) {
    CheckResult result;
    result.passed = true;
    std::ostringstream msg;
    for (std::size_t ch = 0; ch < metrics.channels.size(); ++ch) {
        const auto& c = metrics.channels[ch];
        if (c.peak_dbfs() >= ceiling_dbfs || c.clipped_samples > 0) {
            result.passed = false;
            msg << "ch" << ch << " peak " << format_db(c.peak_dbfs())
                << " reached ceiling " << format_db(ceiling_dbfs);
            if (c.clipped_samples > 0)
                msg << " (" << c.clipped_samples << " samples at/above the"
                    << " analysis clip threshold)";
            msg << ". ";
        }
    }
    if (result.passed) {
        result.message = "no clipping: max peak " +
            format_db(to_dbfs(metrics.max_peak())) + " below ceiling " +
            format_db(ceiling_dbfs);
    } else {
        result.message = msg.str();
    }
    return result;
}

CheckResult assert_silent(const BufferMetrics& metrics,
                          double threshold_dbfs) {
    CheckResult result;
    result.passed = true;
    std::ostringstream msg;
    for (std::size_t ch = 0; ch < metrics.channels.size(); ++ch) {
        const auto& c = metrics.channels[ch];
        if (c.rms_dbfs() >= threshold_dbfs) {
            result.passed = false;
            msg << "expected silence below " << format_db(threshold_dbfs)
                << " but ch" << ch << " RMS is " << format_db(c.rms_dbfs())
                << " (peak " << format_db(c.peak_dbfs()) << "). ";
        }
    }
    if (result.passed)
        result.message = "silent: max RMS " +
            format_db(to_dbfs(metrics.max_rms())) + " below threshold " +
            format_db(threshold_dbfs);
    else
        result.message = msg.str();
    return result;
}

CheckResult assert_not_silent(const BufferMetrics& metrics,
                              double min_rms_dbfs) {
    CheckResult result;
    result.passed = to_dbfs(metrics.max_rms()) >= min_rms_dbfs;
    std::ostringstream msg;
    if (result.passed) {
        msg << "signal present: max RMS "
            << format_db(to_dbfs(metrics.max_rms())) << " above floor "
            << format_db(min_rms_dbfs);
    } else {
        msg << "expected signal above " << format_db(min_rms_dbfs)
            << " on at least one channel, but ";
        for (std::size_t ch = 0; ch < metrics.channels.size(); ++ch) {
            const auto& c = metrics.channels[ch];
            msg << "ch" << ch << " RMS is " << format_db(c.rms_dbfs());
            if (c.longest_silence_run > 0)
                msg << " (longest silence run " << c.longest_silence_run
                    << " samples)";
            msg << (ch + 1 < metrics.channels.size() ? ", " : ". ");
        }
    }
    result.message = msg.str();
    return result;
}

CheckResult assert_peak_between(const BufferMetrics& metrics,
                                double min_dbfs, double max_dbfs) {
    CheckResult result;
    result.passed = true;
    std::ostringstream msg;
    for (std::size_t ch = 0; ch < metrics.channels.size(); ++ch) {
        const double peak = metrics.channels[ch].peak_dbfs();
        if (peak < min_dbfs || peak > max_dbfs) {
            result.passed = false;
            msg << "ch" << ch << " peak " << format_db(peak)
                << " outside [" << format_db(min_dbfs) << ", "
                << format_db(max_dbfs) << "]. ";
        }
    }
    result.message = result.passed
        ? "peaks within [" + format_db(min_dbfs) + ", " + format_db(max_dbfs) + "]"
        : msg.str();
    return result;
}

CheckResult assert_rms_between(const BufferMetrics& metrics,
                               double min_dbfs, double max_dbfs) {
    CheckResult result;
    result.passed = true;
    std::ostringstream msg;
    for (std::size_t ch = 0; ch < metrics.channels.size(); ++ch) {
        const double rms = metrics.channels[ch].rms_dbfs();
        if (rms < min_dbfs || rms > max_dbfs) {
            result.passed = false;
            msg << "ch" << ch << " RMS " << format_db(rms) << " outside ["
                << format_db(min_dbfs) << ", " << format_db(max_dbfs)
                << "]. ";
        }
    }
    result.message = result.passed
        ? "RMS within [" + format_db(min_dbfs) + ", " + format_db(max_dbfs) + "]"
        : msg.str();
    return result;
}

CheckResult assert_frequency_near(std::span<const float> samples,
                                  double sample_rate, double expected_hz,
                                  double tolerance_cents) {
    CheckResult result;
    std::ostringstream msg;
    msg.setf(std::ios::fixed);
    msg.precision(1);

    const auto estimate = estimate_frequency(samples, sample_rate);
    if (estimate.hz <= 0.0) {
        result.passed = false;
        msg << "expected ~" << expected_hz << " Hz but the zero-crossing"
            << " estimator found no periodic signal (needs >= 3 positive"
            << " crossings) in " << samples.size() << " samples @ "
            << sample_rate << " Hz";
        result.message = msg.str();
        return result;
    }

    const double cents = 1200.0 * std::log2(estimate.hz / expected_hz);
    result.passed = std::abs(cents) <= tolerance_cents;
    msg << "expected " << expected_hz << " Hz, measured " << estimate.hz
        << " Hz (" << std::showpos << cents << std::noshowpos
        << " cents, tolerance ±" << tolerance_cents
        << " cents, estimator confidence " << std::setprecision(2)
        << estimate.confidence << ")";
    result.message = msg.str();
    return result;
}

CheckResult assert_null_near(const pulp::audio::BufferView<const float>& a,
                             const pulp::audio::BufferView<const float>& b,
                             double tolerance_dbfs) {
    CheckResult result;
    if (a.num_channels() != b.num_channels() ||
        a.num_samples() != b.num_samples()) {
        result.passed = false;
        result.message = "null test shape mismatch: " +
            std::to_string(a.num_channels()) + "ch/" +
            std::to_string(a.num_samples()) + " frames vs " +
            std::to_string(b.num_channels()) + "ch/" +
            std::to_string(b.num_samples()) + " frames";
        return result;
    }

    double residual_peak = 0.0;
    std::size_t worst_channel = 0, worst_frame = 0;
    for (std::size_t ch = 0; ch < a.num_channels(); ++ch) {
        auto sa = a.channel(ch);
        auto sb = b.channel(ch);
        for (std::size_t i = 0; i < sa.size(); ++i) {
            const double diff = std::abs(static_cast<double>(sa[i]) - sb[i]);
            if (diff > residual_peak) {
                residual_peak = diff;
                worst_channel = ch;
                worst_frame = i;
            }
        }
    }

    result.passed = to_dbfs(residual_peak) < tolerance_dbfs;
    std::ostringstream msg;
    msg << "null residual peak " << format_db(to_dbfs(residual_peak))
        << " (worst at ch" << worst_channel << " frame " << worst_frame
        << "), tolerance " << format_db(tolerance_dbfs);
    result.message = msg.str();
    return result;
}

CheckResult assert_null_near(const pulp::audio::Buffer<float>& a,
                             const pulp::audio::Buffer<float>& b,
                             double tolerance_dbfs) {
    return assert_null_near(a.view(), b.view(), tolerance_dbfs);
}

CheckResult assert_channels_independent(
    const pulp::audio::Buffer<float>& buffer) {
    return assert_channels_independent(buffer.view());
}

CheckResult assert_channels_independent(
    const pulp::audio::BufferView<const float>& buffer) {
    CheckResult result;
    result.passed = true;
    constexpr double kIdenticalEpsilon = 1e-9;
    std::ostringstream msg;
    for (std::size_t ch_a = 0; ch_a < buffer.num_channels(); ++ch_a) {
        for (std::size_t ch_b = ch_a + 1; ch_b < buffer.num_channels(); ++ch_b) {
            auto sa = buffer.channel(ch_a);
            auto sb = buffer.channel(ch_b);
            bool identical = true;
            for (std::size_t i = 0; i < sa.size(); ++i) {
                if (std::abs(static_cast<double>(sa[i]) - sb[i]) >
                    kIdenticalEpsilon) {
                    identical = false;
                    break;
                }
            }
            if (identical) {
                result.passed = false;
                msg << "ch" << ch_a << " and ch" << ch_b
                    << " are sample-identical (within 1e-9) across "
                    << sa.size() << " frames — likely a channel-routing"
                    << " duplication. ";
            }
        }
    }
    result.message = result.passed ? "all channel pairs differ" : msg.str();
    return result;
}

} // namespace pulp::test::audio
