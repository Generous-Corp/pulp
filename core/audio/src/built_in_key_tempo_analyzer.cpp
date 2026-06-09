#include <pulp/audio/built_in_key_tempo_analyzer.hpp>

#include "built_in_analyzer_descriptors.hpp"

#include <pulp/audio/onset_detector.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace pulp::audio {

namespace {

constexpr std::array<double, 12> kMajorProfile = {
    6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile = {
    6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

bool valid_config(BufferView<const float> source,
                  const KeyTempoAnalysisConfig& config) noexcept {
    if (source.num_channels() == 0 || source.num_samples() == 0 ||
        (config.channels != 0 && config.channels != source.num_channels()) ||
        !(config.source_sample_rate > 0.0) || !std::isfinite(config.source_sample_rate) ||
        (!config.estimate_key && !config.estimate_tempo)) {
        return false;
    }
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        if (source.channel_ptr(ch) == nullptr) return false;
    }
    return true;
}

double mono_sample(BufferView<const float> source, std::uint64_t frame) noexcept {
    double sum = 0.0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        sum += static_cast<double>(source.channel_ptr(ch)[frame]);
    }
    return sum / static_cast<double>(source.num_channels());
}

double fold_tempo(double bpm) noexcept {
    while (bpm < 60.0) bpm *= 2.0;
    while (bpm > 180.0) bpm *= 0.5;
    return bpm;
}

void estimate_tempo(BufferView<const float> source,
                    const KeyTempoAnalysisConfig& config,
                    KeyTempoAnalysisResult& result) {
    OnsetDetectionConfig onset_config;
    onset_config.method = OnsetDetectionMethod::EnergyFlux;
    onset_config.frame_size = 1024;
    onset_config.hop_size = 256;
    onset_config.adaptive_window_frames = 8;
    onset_config.min_spacing_frames = static_cast<std::uint64_t>(
        std::max(1.0, config.source_sample_rate * 60.0 / 240.0));
    onset_config.threshold_multiplier = 1.15;
    onset_config.min_confidence = 0.03;
    // Until the public result carries multiple tempo candidates, this setting
    // only controls the internal onset-search budget for the baseline analyzer.
    onset_config.max_markers = std::max<std::size_t>(8, config.max_tempo_candidates * 256);

    const auto onsets = OnsetDetector{}.detect(source, onset_config);
    if (!onsets.ok || onsets.markers.size() < 2) return;

    std::array<double, 241> scores{};
    double total_score = 0.0;
    for (std::size_t i = 1; i < onsets.markers.size(); ++i) {
        const auto previous = onsets.markers[i - 1].frame;
        const auto current = onsets.markers[i].frame;
        if (current <= previous) continue;
        const auto delta = static_cast<double>(current - previous);
        auto bpm = 60.0 * config.source_sample_rate / delta;
        if (!std::isfinite(bpm) || bpm <= 0.0) continue;
        bpm = fold_tempo(bpm);
        if (bpm < 40.0 || bpm > 240.0) continue;
        const auto bin = static_cast<std::size_t>(std::clamp(std::lround(bpm), 0l, 240l));
        const auto weight = std::max(0.001, onsets.markers[i].confidence);
        scores[bin] += weight;
        total_score += weight;
    }
    if (!(total_score > 0.0)) return;

    const auto best = std::max_element(scores.begin(), scores.end());
    if (best == scores.end() || !(*best > 0.0)) return;
    result.tempo_bpm = static_cast<double>(std::distance(scores.begin(), best));
    result.tempo_confidence = std::clamp(*best / total_score, 0.0, 1.0);
}

int pitch_class_for_frequency(double frequency_hz) noexcept {
    if (!(frequency_hz > 0.0) || !std::isfinite(frequency_hz)) return -1;
    const auto midi = 69.0 + 12.0 * std::log2(frequency_hz / 440.0);
    if (!std::isfinite(midi)) return -1;
    auto pitch_class = static_cast<int>(std::lround(midi)) % 12;
    if (pitch_class < 0) pitch_class += 12;
    return pitch_class;
}

double profile_score(const std::array<double, 12>& chroma,
                     const std::array<double, 12>& profile,
                     int root) noexcept {
    double score = 0.0;
    for (int pc = 0; pc < 12; ++pc) {
        const auto relative = (pc - root + 12) % 12;
        score += chroma[static_cast<std::size_t>(pc)] *
                 profile[static_cast<std::size_t>(relative)];
    }
    return score;
}

// Lightweight spectral pitch-class heuristic for a permissive fallback. This is
// intentionally not a mature MIR key detector; package analyzers should be
// preferred when policy allows them.
void estimate_key(BufferView<const float> source,
                  const KeyTempoAnalysisConfig& config,
                  KeyTempoAnalysisResult& result) {
    constexpr std::uint32_t frame_size = 2048;
    constexpr std::uint32_t max_windows = 256;
    if (source.num_samples() < frame_size) return;

    const auto available_windows =
        1 + (static_cast<std::uint64_t>(source.num_samples()) - frame_size) / frame_size;
    const auto window_stride = std::max<std::uint64_t>(
        1, (available_windows + max_windows - 1) / max_windows);

    signal::Fft fft(static_cast<int>(frame_size));
    std::vector<float> mono(frame_size, 0.0f);
    std::vector<std::complex<float>> spectrum(frame_size);
    std::array<double, 12> chroma{};

    for (std::uint64_t window = 0; window < available_windows; window += window_stride) {
        const auto start = window * frame_size;
        for (std::uint32_t i = 0; i < frame_size; ++i) {
            const auto sample = mono_sample(source, start + i);
            constexpr double two_pi = 6.28318530717958647692;
            const auto phase = two_pi * static_cast<double>(i) /
                               static_cast<double>(frame_size - 1);
            const auto hann = 0.5 - 0.5 * std::cos(phase);
            mono[i] = static_cast<float>(sample * hann);
        }
        fft.forward_real(mono.data(), spectrum.data());

        for (std::uint32_t bin = 1; bin < frame_size / 2; ++bin) {
            const auto magnitude = static_cast<double>(std::abs(spectrum[bin]));
            const auto left = static_cast<double>(std::abs(spectrum[bin - 1]));
            const auto right = static_cast<double>(std::abs(spectrum[bin + 1]));
            if (magnitude < left || magnitude < right) continue;

            const auto frequency = static_cast<double>(bin) * config.source_sample_rate /
                                   static_cast<double>(frame_size);
            if (frequency < 55.0 || frequency > 5000.0) continue;
            const auto pitch_class = pitch_class_for_frequency(frequency);
            if (pitch_class < 0) continue;
            chroma[static_cast<std::size_t>(pitch_class)] += magnitude;
        }
    }

    const auto total = std::accumulate(chroma.begin(), chroma.end(), 0.0);
    if (!(total > 0.0)) return;
    for (auto& value : chroma) value /= total;

    double best_score = -std::numeric_limits<double>::infinity();
    double second_score = -std::numeric_limits<double>::infinity();
    int best_root = -1;
    MusicalKeyMode best_mode = MusicalKeyMode::Unknown;
    for (int root = 0; root < 12; ++root) {
        const auto major = profile_score(chroma, kMajorProfile, root);
        const auto minor = profile_score(chroma, kMinorProfile, root);
        const auto consider = [&](double score, MusicalKeyMode mode) {
            if (score > best_score) {
                second_score = best_score;
                best_score = score;
                best_root = root;
                best_mode = mode;
            } else if (score > second_score) {
                second_score = score;
            }
        };
        consider(major, MusicalKeyMode::Major);
        consider(minor, MusicalKeyMode::Minor);
    }

    if (best_root < 0 || !(best_score > 0.0) || !std::isfinite(second_score)) return;
    result.key_root = best_root;
    result.key_mode = best_mode;
    result.key_confidence = std::clamp((best_score - second_score) / best_score, 0.0, 1.0);
}

}  // namespace

BuiltInKeyTempoAnalyzer::BuiltInKeyTempoAnalyzer()
    : descriptor_(detail::make_built_in_key_tempo_analyzer_descriptor()) {}

const AnalyzerDescriptor& BuiltInKeyTempoAnalyzer::descriptor() const noexcept {
    return descriptor_;
}

KeyTempoAnalysisResult BuiltInKeyTempoAnalyzer::analyze(
    BufferView<const float> source,
    const KeyTempoAnalysisConfig& config) {
    KeyTempoAnalysisResult result;
    result.provenance = analyzer_provenance_from_descriptor(descriptor_,
                                                            "builtin-key-tempo");
    if (!valid_config(source, config)) return result;

    if (config.estimate_tempo) estimate_tempo(source, config, result);
    if (config.estimate_key) estimate_key(source, config, result);

    result.ok = (!config.estimate_tempo || result.tempo_bpm > 0.0) &&
                (!config.estimate_key || result.key_root >= 0);
    return result;
}

}  // namespace pulp::audio
