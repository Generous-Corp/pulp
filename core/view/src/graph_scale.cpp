#include <pulp/view/graph_scale.hpp>

#include <pulp/signal/frequency_response.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::view {

namespace {

// Guard the degenerate ranges that would divide by zero or take log10 of a
// non-positive frequency. A zero-width axis or an inverted range is a layout
// bug, not something to propagate as NaN into every drawn point.
bool usable(const LogFrequencyScale& s) {
    return s.width > 0.0f && s.min_hz > 0.0f && s.max_hz > s.min_hz;
}

} // namespace

float LogFrequencyScale::to_x(float hz) const {
    if (!usable(*this)) return x;
    const float log_min = std::log10(min_hz);
    const float log_max = std::log10(max_hz);
    const float log_hz = std::log10(std::max(hz, 1e-6f));
    return x + width * (log_hz - log_min) / (log_max - log_min);
}

float LogFrequencyScale::to_frequency(float px) const {
    if (!usable(*this)) return min_hz;
    const float log_min = std::log10(min_hz);
    const float log_max = std::log10(max_hz);
    const float t = (px - x) / width;
    return std::pow(10.0f, log_min + t * (log_max - log_min));
}

float LogFrequencyScale::frequency_at(std::size_t index, std::size_t count) const {
    return static_cast<float>(signal::log_frequency_at(index, count, min_hz, max_hz));
}

std::vector<float> LogFrequencyScale::ticks() const {
    std::vector<float> out;
    if (!usable(*this)) return out;

    // 1-2-5 per decade: 20, 30, 50, 100, 200, 300, 500, 1k … The mantissas
    // below are the ones an audio engineer expects to see labeled; a plain
    // every-integer-multiple sweep crowds the top of each decade.
    static constexpr float mantissas[] = {1, 2, 3, 5, 7};
    const int first_decade = static_cast<int>(std::floor(std::log10(min_hz)));
    const int last_decade = static_cast<int>(std::ceil(std::log10(max_hz)));

    for (int d = first_decade; d <= last_decade; ++d) {
        const float decade = std::pow(10.0f, static_cast<float>(d));
        for (float m : mantissas) {
            const float hz = m * decade;
            if (hz >= min_hz && hz <= max_hz) out.push_back(hz);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool LogFrequencyScale::is_major_tick(float hz) {
    // Decade boundaries: 10, 100, 1k, 10k …
    if (!(hz > 0.0f)) return false;
    const float l = std::log10(hz);
    return std::abs(l - std::round(l)) < 1e-4f;
}

float DecibelScale::to_y(float db) const {
    if (!(height > 0.0f) || !(max_db > min_db)) return y;
    const float t = (db - max_db) / (min_db - max_db);
    return y + t * height;
}

float DecibelScale::to_decibels(float py) const {
    if (!(height > 0.0f) || !(max_db > min_db)) return max_db;
    const float t = (py - y) / height;
    return max_db + t * (min_db - max_db);
}

std::vector<float> DecibelScale::ticks(float step_db) const {
    std::vector<float> out;
    if (!(step_db > 0.0f) || !(max_db > min_db)) return out;

    // Anchor the sequence on 0 dB rather than on min_db, so unity always lands
    // exactly on a gridline instead of falling between two of them.
    const float first = std::ceil(min_db / step_db) * step_db;
    for (float db = first; db <= max_db + 1e-4f; db += step_db) out.push_back(db);
    return out;
}

void resample_spectrum_log(std::span<const float> bins_db,
                           float sample_rate,
                           const LogFrequencyScale& scale,
                           std::span<float> out) {
    const std::size_t n = out.size();
    if (n == 0) return;

    if (bins_db.size() < 2 || !(sample_rate > 0.0f)) {
        std::fill(out.begin(), out.end(), signal::min_response_db);
        return;
    }

    const double nyquist = sample_rate * 0.5;
    const std::size_t last_bin = bins_db.size() - 1;

    for (std::size_t i = 0; i < n; ++i) {
        const double hz = signal::log_frequency_at(i, n, scale.min_hz, scale.max_hz);

        if (hz >= nyquist) {
            // Above Nyquist the FFT has nothing to say. Report the floor rather
            // than clamping to the top bin, which would draw a phantom shelf.
            out[i] = signal::min_response_db;
            continue;
        }

        // Fractional bin index, then linear interpolation in dB between the two
        // neighbours. Interpolating in dB (not linear magnitude) is what keeps
        // the drawn curve smooth on a dB axis.
        const double pos = hz / nyquist * static_cast<double>(last_bin);
        const auto lo = static_cast<std::size_t>(pos);
        if (lo >= last_bin) {
            out[i] = bins_db[last_bin];
            continue;
        }
        const auto frac = static_cast<float>(pos - static_cast<double>(lo));
        out[i] = bins_db[lo] + (bins_db[lo + 1] - bins_db[lo]) * frac;
    }
}

} // namespace pulp::view
