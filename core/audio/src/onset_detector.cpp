#include <pulp/audio/onset_detector.hpp>

#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>

namespace pulp::audio {

namespace {

bool valid_config(const OnsetDetectionConfig& config) noexcept {
    return config.frame_size > 1 && config.hop_size > 0 &&
           config.threshold_multiplier > 0.0 && config.min_confidence >= 0.0 &&
           config.max_markers > 0;
}

bool power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1u)) == 0;
}

double mono_sample(BufferView<const float> source, std::uint64_t frame) noexcept {
    double sum = 0.0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        sum += static_cast<double>(source.channel_ptr(ch)[frame]);
    }
    return sum / static_cast<double>(source.num_channels());
}

double frame_energy(BufferView<const float> source,
                    std::uint64_t start,
                    std::uint32_t frame_size) noexcept {
    double energy = 0.0;
    std::uint64_t count = 0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        const auto* data = source.channel_ptr(ch);
        for (std::uint32_t i = 0; i < frame_size; ++i) {
            const auto value = static_cast<double>(data[start + i]);
            energy += value * value;
            ++count;
        }
    }
    return count == 0 ? 0.0 : energy / static_cast<double>(count);
}

void spectral_magnitudes(BufferView<const float> source,
                         std::uint64_t start,
                         std::uint32_t frame_size,
                         signal::Fft& fft,
                         std::vector<float>& mono_frame,
                         std::vector<std::complex<float>>& spectrum,
                         std::vector<double>& magnitudes) {
    const auto bins = static_cast<std::size_t>(frame_size / 2 + 1);
    if (mono_frame.size() != frame_size) mono_frame.assign(frame_size, 0.0f);
    if (spectrum.size() != frame_size) spectrum.assign(frame_size, {});
    if (magnitudes.size() != bins) magnitudes.assign(bins, 0.0);

    for (std::uint32_t n = 0; n < frame_size; ++n) {
        mono_frame[n] = static_cast<float>(mono_sample(source, start + n));
    }
    fft.forward_real(mono_frame.data(), spectrum.data());
    for (std::size_t bin = 0; bin < bins; ++bin) {
        magnitudes[bin] = static_cast<double>(std::abs(spectrum[bin]));
    }
}

double adaptive_threshold(const std::vector<double>& novelty,
                          std::size_t index,
                          std::uint32_t radius,
                          double multiplier) noexcept {
    const auto begin = index > radius ? index - radius : 0;
    const auto end = std::min(novelty.size(), index + static_cast<std::size_t>(radius) + 1);
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) sum += novelty[i];
    const auto count = end > begin ? end - begin : 1;
    return (sum / static_cast<double>(count)) * multiplier;
}

void retain_onset(std::vector<OnsetMarker>& markers,
                  OnsetMarker marker,
                  std::uint64_t min_spacing,
                  std::size_t max_markers) {
    if (markers.empty()) {
        markers.push_back(marker);
        return;
    }

    auto& previous = markers.back();
    if (marker.frame < previous.frame + min_spacing) {
        if (marker.confidence > previous.confidence) previous = marker;
        return;
    }

    if (markers.size() < max_markers) markers.push_back(marker);
}

}  // namespace

OnsetDetectionResult OnsetDetector::detect(BufferView<const float> source,
                                           const OnsetDetectionConfig& config) const {
    OnsetDetectionResult result;
    if (source.num_channels() == 0 || source.num_samples() == 0 ||
        !valid_config(config) ||
        source.num_samples() < static_cast<std::size_t>(config.frame_size)) {
        return result;
    }
    if (config.method != OnsetDetectionMethod::EnergyFlux &&
        !power_of_two(config.frame_size)) {
        return result;
    }

    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    const auto frame_count =
        1 + (source_frames - config.frame_size) / config.hop_size;
    std::vector<double> novelty(static_cast<std::size_t>(frame_count), 0.0);

    if (config.method == OnsetDetectionMethod::EnergyFlux) {
        double previous_energy = frame_energy(source, 0, config.frame_size);
        for (std::uint64_t i = 1; i < frame_count; ++i) {
            const auto start = i * config.hop_size;
            const auto energy = frame_energy(source, start, config.frame_size);
            novelty[static_cast<std::size_t>(i)] =
                std::max(0.0, energy - previous_energy);
            previous_energy = energy;
        }
    } else {
        signal::Fft fft(static_cast<int>(config.frame_size));
        std::vector<float> mono_frame(config.frame_size, 0.0f);
        std::vector<std::complex<float>> spectrum(config.frame_size);
        std::vector<double> previous(static_cast<std::size_t>(config.frame_size / 2 + 1),
                                     0.0);
        std::vector<double> current(previous.size(), 0.0);
        spectral_magnitudes(source,
                            0,
                            config.frame_size,
                            fft,
                            mono_frame,
                            spectrum,
                            previous);
        for (std::uint64_t i = 1; i < frame_count; ++i) {
            const auto start = i * config.hop_size;
            spectral_magnitudes(source,
                                start,
                                config.frame_size,
                                fft,
                                mono_frame,
                                spectrum,
                                current);
            double value = 0.0;
            for (std::size_t bin = 0; bin < current.size(); ++bin) {
                const auto positive = std::max(0.0, current[bin] - previous[bin]);
                if (config.method == OnsetDetectionMethod::HighFrequencyContent) {
                    value += positive * static_cast<double>(bin + 1);
                } else {
                    value += positive;
                }
            }
            novelty[static_cast<std::size_t>(i)] = value;
            std::swap(previous, current);
        }
    }

    const auto max_novelty = *std::max_element(novelty.begin(), novelty.end());
    if (!(max_novelty > 0.0)) {
        result.ok = true;
        result.analyzed_frames = source_frames;
        return result;
    }

    result.markers.reserve(std::min<std::size_t>(config.max_markers, novelty.size()));
    for (std::size_t i = 1; i < novelty.size(); ++i) {
        const auto confidence = novelty[i] / max_novelty;
        const auto threshold = adaptive_threshold(novelty,
                                                  i,
                                                  config.adaptive_window_frames,
                                                  config.threshold_multiplier);
        if (confidence < config.min_confidence || novelty[i] < threshold) continue;

        OnsetMarker marker;
        marker.frame = static_cast<std::uint64_t>(i) * config.hop_size;
        marker.confidence = confidence;
        marker.method = config.method;
        retain_onset(result.markers,
                     marker,
                     config.min_spacing_frames,
                     config.max_markers);
    }

    result.ok = true;
    result.analyzed_frames = source_frames;
    return result;
}

}  // namespace pulp::audio
