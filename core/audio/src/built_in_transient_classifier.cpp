#include <pulp/audio/built_in_transient_classifier.hpp>

#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

namespace pulp::audio {

namespace {

constexpr std::uint32_t kFrameSize = 1024;
constexpr double kMinClassifiableEnergy = 1.0e-7;

AnalyzerDescriptor make_descriptor() {
    AnalyzerDescriptor descriptor;
    descriptor.id = "builtin.transient-classifier";
    descriptor.display_name = "Built-in Basic Transient Classifier";
    descriptor.version = "builtin";
    descriptor.license_id = "MIT";
    descriptor.backend = AnalyzerBackend::BuiltIn;
    descriptor.availability = AnalyzerAvailability::Available;
    descriptor.license_policy = AnalyzerLicensePolicy::Permissive;
    descriptor.capabilities = {AnalyzerCapability::TransientClassification};
    descriptor.execution_context = AnalyzerExecutionContext::BackgroundThread;
    descriptor.supports_streaming_input = false;
    descriptor.supports_offline_buffers = true;
    descriptor.is_fallback = true;
    return descriptor;
}

bool valid_source(BufferView<const float> source) noexcept {
    if (source.num_channels() == 0 || source.num_samples() == 0) return false;
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

struct SpectralFeatures {
    double energy = 0.0;
    double low_ratio = 0.0;
    double mid_ratio = 0.0;
    double high_ratio = 0.0;
    double peak_ratio = 0.0;
    bool spectral_valid = false;
};

SpectralFeatures analyze_window(BufferView<const float> source,
                                std::uint64_t center_frame,
                                signal::Fft& fft,
                                std::vector<float>& mono,
                                std::vector<std::complex<float>>& spectrum) {
    SpectralFeatures features;
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    const auto half = static_cast<std::uint64_t>(kFrameSize / 2);
    const auto start = center_frame > half ? center_frame - half : 0;

    constexpr double two_pi = 6.28318530717958647692;
    std::fill(mono.begin(), mono.end(), 0.0f);
    for (std::uint32_t i = 0; i < kFrameSize; ++i) {
        const auto frame = start + i;
        if (frame >= source_frames) break;
        auto sample = mono_sample(source, frame);
        if (!std::isfinite(sample)) sample = 0.0;
        features.energy += sample * sample;
        const auto phase = two_pi * static_cast<double>(i) / static_cast<double>(kFrameSize - 1);
        const auto hann = 0.5 - 0.5 * std::cos(phase);
        mono[i] = static_cast<float>(sample * hann);
    }
    features.energy /= static_cast<double>(kFrameSize);
    if (!(features.energy > 0.0) || !std::isfinite(features.energy)) return features;

    fft.forward_real(mono.data(), spectrum.data());

    double low = 0.0;
    double mid = 0.0;
    double high = 0.0;
    double total = 0.0;
    double peak = 0.0;
    for (std::uint32_t bin = 1; bin < kFrameSize / 2; ++bin) {
        const auto magnitude = static_cast<double>(std::abs(spectrum[bin]));
        if (!std::isfinite(magnitude)) continue;
        total += magnitude;
        peak = std::max(peak, magnitude);
        if (bin <= 6) {
            low += magnitude;
        } else if (bin <= 80) {
            mid += magnitude;
        } else {
            high += magnitude;
        }
    }
    if (!(total > 0.0) || !std::isfinite(total)) return features;

    features.low_ratio = low / total;
    features.mid_ratio = mid / total;
    features.high_ratio = high / total;
    features.peak_ratio = peak / total;
    features.spectral_valid = std::isfinite(features.low_ratio) &&
                              std::isfinite(features.mid_ratio) &&
                              std::isfinite(features.high_ratio) &&
                              std::isfinite(features.peak_ratio);
    return features;
}

TransientClass classify_features(const SpectralFeatures& features) noexcept {
    if (!(features.energy > kMinClassifiableEnergy) || !features.spectral_valid) {
        return TransientClass::Unknown;
    }
    if (features.low_ratio > 0.58) return TransientClass::Kick;
    if (features.high_ratio > 0.58 && features.peak_ratio < 0.12) return TransientClass::Hat;
    if (features.mid_ratio > 0.50 && features.high_ratio > 0.18) return TransientClass::Snare;
    if (features.peak_ratio > 0.24 && features.high_ratio < 0.35) return TransientClass::Tonal;
    if (features.high_ratio > 0.40) return TransientClass::Noise;
    return TransientClass::Percussion;
}

double confidence_for(const SpectralFeatures& features, TransientClass transient_class) noexcept {
    switch (transient_class) {
        case TransientClass::Kick:
            return std::clamp(features.low_ratio, 0.0, 1.0);
        case TransientClass::Hat:
            return std::clamp(features.high_ratio * (1.0 - features.peak_ratio), 0.0, 1.0);
        case TransientClass::Snare:
            return std::clamp((features.mid_ratio + features.high_ratio) * 0.5, 0.0, 1.0);
        case TransientClass::Tonal:
            return std::clamp(features.peak_ratio, 0.0, 1.0);
        case TransientClass::Noise:
            return std::clamp(features.high_ratio, 0.0, 1.0);
        case TransientClass::Percussion:
            return std::clamp(std::max({features.low_ratio, features.mid_ratio, features.high_ratio}), 0.0, 1.0);
        case TransientClass::Unknown:
        case TransientClass::Clap:
        case TransientClass::Cymbal:
        case TransientClass::Tom:
        case TransientClass::Bass:
        case TransientClass::Vocal:
            return 0.0;
    }
    return 0.0;
}

}  // namespace

BuiltInTransientClassifier::BuiltInTransientClassifier()
    : descriptor_(make_descriptor()) {}

const AnalyzerDescriptor& BuiltInTransientClassifier::descriptor() const noexcept {
    return descriptor_;
}

std::vector<TransientClassification> BuiltInTransientClassifier::classify(
    BufferView<const float> source,
    std::span<const std::uint64_t> candidate_frames) {
    std::vector<TransientClassification> classifications;
    if (!valid_source(source) || candidate_frames.empty()) return classifications;

    classifications.reserve(candidate_frames.size());
    signal::Fft fft(static_cast<int>(kFrameSize));
    std::vector<float> mono(kFrameSize, 0.0f);
    std::vector<std::complex<float>> spectrum(kFrameSize);
    const auto provenance = analyzer_provenance_from_descriptor(descriptor_,
                                                                "builtin-transient");

    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    for (std::size_t candidate_index = 0; candidate_index < candidate_frames.size();
         ++candidate_index) {
        const auto frame = candidate_frames[candidate_index];
        if (frame >= source_frames) continue;
        const auto features = analyze_window(source, frame, fft, mono, spectrum);
        const auto transient_class = classify_features(features);
        if (transient_class == TransientClass::Unknown) continue;

        TransientClassification classification;
        classification.frame = frame;
        const auto confidence = confidence_for(features, transient_class);
        if (!(confidence > 0.0) || !std::isfinite(confidence)) continue;

        classification.transient_class = transient_class;
        classification.confidence = confidence;
        classification.provenance = provenance;
        if (candidate_index <= std::numeric_limits<std::uint32_t>::max()) {
            classification.candidate_index = static_cast<std::uint32_t>(candidate_index);
            classification.has_candidate_index = true;
        }
        classifications.push_back(std::move(classification));
    }
    return classifications;
}

}  // namespace pulp::audio
