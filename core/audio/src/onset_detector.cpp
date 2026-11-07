#include <pulp/audio/onset_detector.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

namespace pulp::audio {

namespace {

constexpr std::size_t kMaxAnalysisChannels = 256;
constexpr std::size_t kMaxAnalysisFrameSize = 65536;

bool valid_config(const OnsetDetectionConfig& config) noexcept {
    return config.frame_size > 1 && config.hop_size > 0 && config.threshold_multiplier > 0.0 &&
           config.min_confidence >= 0.0 && config.max_markers > 0 &&
           std::isfinite(config.sample_rate) && config.sample_rate > 0.0;
}

bool power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1u)) == 0;
}

double offline_frame_energy(BufferView<const float> source, std::uint64_t start,
                            std::uint32_t frame_size) noexcept {
    long double mean = 0.0L;
    const auto divisor =
        static_cast<long double>(frame_size) * static_cast<long double>(source.num_channels());
    for (std::size_t channel = 0; channel < source.num_channels(); ++channel) {
        const auto* data = source.channel_ptr(channel) + start;
        for (std::uint32_t frame = 0; frame < frame_size; ++frame) {
            const auto sample = static_cast<long double>(data[frame]);
            mean += (sample * sample) / divisor;
        }
    }
    return static_cast<double>(mean);
}

bool offline_spectral_magnitudes(BufferView<const float> source, std::uint64_t start,
                                 std::uint32_t frame_size, signal::Fft& fft,
                                 std::vector<float>& mono,
                                 std::vector<std::complex<float>>& spectrum,
                                 std::vector<double>& magnitudes) {
    for (std::uint32_t frame = 0; frame < frame_size; ++frame) {
        double sum = 0.0;
        for (std::size_t channel = 0; channel < source.num_channels(); ++channel)
            sum += static_cast<double>(source.channel_ptr(channel)[start + frame]);
        mono[frame] = static_cast<float>(sum / static_cast<double>(source.num_channels()));
        if (!std::isfinite(mono[frame]))
            return false;
    }
    fft.forward_real(mono.data(), spectrum.data());
    for (std::size_t bin = 0; bin < magnitudes.size(); ++bin) {
        magnitudes[bin] = static_cast<double>(std::abs(spectrum[bin]));
        if (!std::isfinite(magnitudes[bin]))
            return false;
    }
    return true;
}

bool fill_offline_novelty(BufferView<const float> source, const OnsetDetectionConfig& config,
                          std::vector<double>& novelty) {
    if (config.method == OnsetDetectionMethod::EnergyFlux) {
        auto previous = offline_frame_energy(source, 0, config.frame_size);
        if (!std::isfinite(previous))
            return false;
        for (std::size_t index = 1; index < novelty.size(); ++index) {
            const auto current = offline_frame_energy(
                source, static_cast<std::uint64_t>(index) * config.hop_size, config.frame_size);
            if (!std::isfinite(current))
                return false;
            novelty[index] = signal::onset_energy_flux(previous, current);
            previous = current;
        }
        return true;
    }

    signal::Fft fft(static_cast<int>(config.frame_size));
    std::vector<float> mono(config.frame_size);
    std::vector<std::complex<float>> spectrum(config.frame_size);
    const auto bins = static_cast<std::size_t>(config.frame_size / 2u + 1u);
    std::vector<double> previous(bins);
    std::vector<double> current(bins);
    if (!offline_spectral_magnitudes(source, 0, config.frame_size, fft, mono,
                                     spectrum, previous))
        return false;
    for (std::size_t index = 1; index < novelty.size(); ++index) {
        if (!offline_spectral_magnitudes(
                source, static_cast<std::uint64_t>(index) * config.hop_size,
                config.frame_size, fft, mono, spectrum, current))
            return false;
        double value = 0.0;
        for (std::size_t bin = 0; bin < bins; ++bin)
            value +=
                signal::onset_spectral_bin_flux(config.method, previous[bin], current[bin], bin);
        if (!std::isfinite(value))
            return false;
        novelty[index] = value;
        std::swap(previous, current);
    }
    return true;
}

// Leading silence SuperFlux is given: half a window, so the first reported
// frame is centred on source sample 0, plus `lag` hops, so that frame has an
// earlier frame to compare with. The first `lag` frames are dropped.
struct StartPadding {
    std::uint64_t samples = 0;
    std::size_t dropped_frames = 0;
};

StartPadding superflux_start_padding(const OnsetDetectionConfig& config, std::size_t lag) {
    if (config.method != OnsetDetectionMethod::SuperFlux)
        return {};
    return {config.frame_size / 2u + static_cast<std::uint64_t>(lag) * config.hop_size, lag};
}

// The SuperFlux lag the front end will use, without preparing one.
std::size_t superflux_lag(const OnsetDetectionConfig& config) {
    if (config.superflux.lag_frames != 0)
        return config.superflux.lag_frames;
    const double derived = std::round(0.25 * static_cast<double>(config.frame_size) /
                                      static_cast<double>(config.hop_size));
    return static_cast<std::size_t>(std::clamp(derived, 1.0, 8.0));
}

template <std::size_t Capacity>
bool fill_streaming_novelty(BufferView<const float> source, const OnsetDetectionConfig& config,
                            std::vector<double>& novelty) {
    using FrontEnd = signal::OnsetNoveltyFrontEndT<float, Capacity, kMaxAnalysisChannels>;
    auto front_end = std::make_unique<FrontEnd>();
    signal::StreamingAnalysisConfig stream_config;
    // Only SuperFlux lays out its filterbank in hertz; the other novelty
    // functions are indexed in samples and the default rate leaves them as
    // they were.
    stream_config.sample_rate = config.sample_rate;
    stream_config.channels = source.num_channels();
    stream_config.fft_size = config.frame_size;
    stream_config.hop_size = config.hop_size;
    if (!front_end->prepare(stream_config, config.method, config.superflux))
        return false;

    const auto padding = superflux_start_padding(config, front_end->superflux_lag_frames());
    if (padding.dropped_frames != superflux_lag(config) &&
        config.method == OnsetDetectionMethod::SuperFlux)
        return false; // the frame count was sized with a different lag

    std::size_t seen = 0;
    std::size_t emitted = 0;
    const auto sink = [&](const FrontEnd::Frame& frame) noexcept {
        if (seen++ < padding.dropped_frames)
            return;
        if (emitted < novelty.size())
            novelty[emitted++] = frame.novelty;
    };
    bool finite = true;
    if (padding.samples > 0) {
        constexpr std::size_t kSilenceChunk = 4096;
        const std::vector<float> silence(kSilenceChunk, 0.0f);
        const std::vector<const float*> silent(source.num_channels(), silence.data());
        for (std::uint64_t fed = 0; fed < padding.samples;) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(kSilenceChunk, padding.samples - fed));
            finite = front_end->process(silent.data(), silent.size(), count, sink) && finite;
            fed += count;
        }
    }

    std::vector<const float*> channels(source.num_channels());
    for (std::size_t channel = 0; channel < source.num_channels(); ++channel)
        channels[channel] = source.channel_ptr(channel);
    const auto analyzed_samples =
        (novelty.size() + padding.dropped_frames - 1u) * static_cast<std::size_t>(config.hop_size) +
        static_cast<std::size_t>(config.frame_size) - static_cast<std::size_t>(padding.samples);
    finite = front_end->process(channels.data(), channels.size(), analyzed_samples, sink) && finite;
    return finite && emitted == novelty.size();
}

double adaptive_threshold(const std::vector<double>& novelty, std::size_t index,
                          std::uint32_t radius, double multiplier) noexcept {
    const auto begin = index > radius ? index - radius : 0;
    const auto end = std::min(novelty.size(), index + static_cast<std::size_t>(radius) + 1);
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += novelty[i];
    const auto count = end > begin ? end - begin : 1;
    return (sum / static_cast<double>(count)) * multiplier;
}

void retain_onset(std::vector<OnsetMarker>& markers, OnsetMarker marker, std::uint64_t min_spacing,
                  std::size_t max_markers) {
    if (markers.empty()) {
        markers.push_back(marker);
        return;
    }

    auto& previous = markers.back();
    if (marker.frame < previous.frame + min_spacing) {
        if (marker.confidence > previous.confidence)
            previous = marker;
        return;
    }

    if (markers.size() < max_markers)
        markers.push_back(marker);
}

} // namespace

OnsetDetectionResult OnsetDetector::detect(BufferView<const float> source,
                                           const OnsetDetectionConfig& config) const {
    OnsetDetectionResult result;
    if (source.num_channels() == 0 || source.num_samples() == 0 || !valid_config(config) ||
        source.num_samples() < static_cast<std::size_t>(config.frame_size)) {
        return result;
    }
    if (config.method != OnsetDetectionMethod::EnergyFlux && !power_of_two(config.frame_size)) {
        return result;
    }

    for (std::size_t channel = 0; channel < source.num_channels(); ++channel) {
        if (source.channel_ptr(channel) == nullptr)
            return result;
    }
    if (config.method == OnsetDetectionMethod::SuperFlux) {
        result.marker_timing = OnsetMarkerTiming::window_centre;
        result.marker_offset_frames = config.frame_size / 2u;
    }

    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    const bool superflux = config.method == OnsetDetectionMethod::SuperFlux;
    // SuperFlux frames are counted over the padded source, less the dropped
    // lead-in frames (see superflux_start_padding()).
    const auto padding =
        superflux ? superflux_start_padding(config, superflux_lag(config)) : StartPadding{};
    const auto frame_count =
        1 + (source_frames + padding.samples - config.frame_size) / config.hop_size -
        padding.dropped_frames;
    std::vector<double> novelty(static_cast<std::size_t>(frame_count), 0.0);

    const auto needs_dynamic_offline =
        source.num_channels() > kMaxAnalysisChannels ||
        config.frame_size > kMaxAnalysisFrameSize || config.hop_size > config.frame_size;
    if (needs_dynamic_offline) {
        // The fixed-capacity SuperFlux front end is the only SuperFlux
        // implementation; frames or channel counts beyond it are refused.
        if (config.method == OnsetDetectionMethod::SuperFlux)
            return result;
        if (!fill_offline_novelty(source, config, novelty))
            return result;
    } else {
        bool filled = false;
        if (config.frame_size <= 1024)
            filled = fill_streaming_novelty<1024>(source, config, novelty);
        else if (config.frame_size <= 4096)
            filled = fill_streaming_novelty<4096>(source, config, novelty);
        else if (config.frame_size <= 16384)
            filled = fill_streaming_novelty<16384>(source, config, novelty);
        else
            filled = fill_streaming_novelty<kMaxAnalysisFrameSize>(source, config, novelty);
        if (!filled)
            return result;
    }

    const auto max_novelty = *std::max_element(novelty.begin(), novelty.end());
    if (!(max_novelty > 0.0)) {
        result.ok = true;
        result.analyzed_frames = source_frames;
        return result;
    }

    result.markers.reserve(std::min<std::size_t>(config.max_markers, novelty.size()));
    // The legacy methods' first frame has no predecessor and novelty 0; the
    // first SuperFlux frame is compared with the padded silence before it.
    for (std::size_t i = superflux ? 0 : 1; i < novelty.size(); ++i) {
        const auto confidence = novelty[i] / max_novelty;
        const auto threshold = adaptive_threshold(novelty, i, config.adaptive_window_frames,
                                                  config.threshold_multiplier);
        if (confidence < config.min_confidence || novelty[i] < threshold)
            continue;

        OnsetMarker marker;
        // Window starts for the legacy methods; window centres for SuperFlux,
        // whose padded frame i is centred on source sample i * hop.
        marker.frame = static_cast<std::uint64_t>(i) * config.hop_size;
        marker.confidence = confidence;
        marker.method = config.method;
        retain_onset(result.markers, marker, config.min_spacing_frames, config.max_markers);
    }

    result.ok = true;
    result.analyzed_frames = source_frames;
    return result;
}

} // namespace pulp::audio
