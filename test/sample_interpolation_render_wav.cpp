#include "support/sample_interpolation_render.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/loop_renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Policy = pulp::audio::SampleInterpolationPolicy;

std::vector<float> render_production_loop(
    Policy policy, double ratio, std::span<const float> source,
    std::size_t output_frames, std::size_t block_frames) {
    pulp::audio::SampleSincKernelBank sinc_bank;
    if (!sinc_bank.build_dense_for_maximum_consumption(4.0))
        throw std::runtime_error("could not build sampler sinc bank");

    pulp::audio::PreparedSampleInterpolation interpolation{.policy = policy};
    if (policy == Policy::RatioTrackingSinc)
        interpolation.sinc = sinc_bank.view().select(ratio);
    if (!interpolation.valid())
        throw std::runtime_error("interpolation is not valid for ratio");

    pulp::audio::LoopRegion region;
    region.start_frame = 0;
    region.end_frame = source.size();
    region.source_sample_rate = 48000.0;
    region.playback_mode = pulp::audio::LoopPlaybackMode::OneShot;
    region.interpolation = pulp::audio::LoopInterpolationMode::Linear;

    pulp::audio::LoopRenderer renderer;
    if (!renderer.set_region(region, source.size()) ||
        !renderer.set_interpolation(interpolation)) {
        throw std::runtime_error("could not prepare production loop renderer");
    }
    renderer.set_playback_rate(ratio);
    renderer.start();

    const float* source_pointer = source.data();
    const pulp::audio::BufferView<const float> source_view(
        &source_pointer, 1, source.size());
    std::vector<float> output(output_frames, 0.0f);
    for (std::size_t start = 0; start < output_frames; start += block_frames) {
        const auto count = std::min(block_frames, output_frames - start);
        float* output_pointer = output.data() + start;
        pulp::audio::BufferView<float> output_view(&output_pointer, 1, count);
        const auto rendered = renderer.render(source_view, output_view, count);
        if (rendered.rendered_frames != count)
            throw std::runtime_error("production loop renderer ended early");
    }
    return output;
}

std::optional<Policy> parse_policy(const char* text) {
    for (const auto policy : {Policy::Hold, Policy::Nearest, Policy::Linear,
                              Policy::CubicHermite, Policy::CubicLagrange,
                              Policy::RatioTrackingSinc}) {
        if (std::strcmp(
                text,
                pulp::test::audio::sample_interpolation_policy_cli_id(policy)) == 0)
            return policy;
    }
    return std::nullopt;
}

const char* value_for(int argc, char** argv, int& index, const char* flag) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "pulp-sampler-render-wav: %s needs a value\n", flag);
        return nullptr;
    }
    return argv[++index];
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --source-out PATH --candidate-out PATH --ratio R "
        "--source-frequency CYCLES [--frames N] [--block-size N] "
        "[--policy hold|nearest|linear|cubic-hermite|cubic-lagrange|ratio-sinc]\n",
        argv0);
}

} // namespace

int main(int argc, char** argv) {
    std::string source_out;
    std::string candidate_out;
    Policy policy = Policy::RatioTrackingSinc;
    double ratio = 0.0;
    double source_frequency = 0.0;
    int frames = 16385;
    int block_size = 64;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--source-out") == 0) {
            const auto* value = value_for(argc, argv, i, arg);
            if (!value) return 2;
            source_out = value;
        } else if (std::strcmp(arg, "--candidate-out") == 0) {
            const auto* value = value_for(argc, argv, i, arg);
            if (!value) return 2;
            candidate_out = value;
        } else if (std::strcmp(arg, "--ratio") == 0) {
            const auto* value = value_for(argc, argv, i, arg);
            if (!value) return 2;
            ratio = std::strtod(value, nullptr);
        } else if (std::strcmp(arg, "--source-frequency") == 0) {
            const auto* value = value_for(argc, argv, i, arg);
            if (!value) return 2;
            source_frequency = std::strtod(value, nullptr);
        } else if (std::strcmp(arg, "--frames") == 0) {
            const auto* value = value_for(argc, argv, i, arg);
            if (!value) return 2;
            frames = std::atoi(value);
        } else if (std::strcmp(arg, "--block-size") == 0) {
            const auto* value = value_for(argc, argv, i, arg);
            if (!value) return 2;
            block_size = std::atoi(value);
        } else if (std::strcmp(arg, "--policy") == 0) {
            const auto* value = value_for(argc, argv, i, arg);
            if (!value) return 2;
            const auto parsed = parse_policy(value);
            if (!parsed) {
                std::fprintf(stderr, "pulp-sampler-render-wav: unknown policy '%s'\n", value);
                return 2;
            }
            policy = *parsed;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "pulp-sampler-render-wav: unknown argument '%s'\n", arg);
            usage(argv[0]);
            return 2;
        }
    }

    const auto source_intervals = ratio * static_cast<double>(frames - 1);
    const auto source_frames = static_cast<int>(std::llround(source_intervals)) + 1;
    if (source_out.empty() || candidate_out.empty() || frames < 1024 || block_size < 1 ||
        !(ratio > 0.0 && ratio <= 4.0) ||
        !(source_frequency > 0.0 && source_frequency < 0.5) ||
        std::abs(source_intervals - std::round(source_intervals)) > 1e-9) {
        std::fprintf(stderr, "pulp-sampler-render-wav: invalid contract or non-integral source length\n");
        usage(argv[0]);
        return 2;
    }

    try {
        pulp::audio::AudioFileData source;
        source.sample_rate = 48000;
        source.channels.resize(1);
        source.channels[0].resize(static_cast<std::size_t>(source_frames));
        pulp::audio::AudioFileData candidate;
        candidate.sample_rate = 48000;
        candidate.channels.resize(1);
        for (int i = 0; i < source_frames; ++i) {
            source.channels[0][static_cast<std::size_t>(i)] = static_cast<float>(
                pulp::test::audio::kSamplerQualityAmplitude *
                std::sin(2.0 * std::numbers::pi * source_frequency * i));
        }
        candidate.channels[0] = render_production_loop(
            policy, ratio, source.channels[0], static_cast<std::size_t>(frames),
            static_cast<std::size_t>(block_size));

        if (!pulp::audio::write_wav_file(
                source_out, source, pulp::audio::WavBitDepth::Float32) ||
            !pulp::audio::write_wav_file(
                candidate_out, candidate, pulp::audio::WavBitDepth::Float32)) {
            std::fprintf(stderr, "pulp-sampler-render-wav: WAV write failed\n");
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pulp-sampler-render-wav: render failed: %s\n", e.what());
        return 1;
    }

    std::printf("wrote source=%s candidate=%s ratio=%.8g source-frequency=%.8g "
                "frames=%d block=%d policy=%s\n",
                source_out.c_str(), candidate_out.c_str(), ratio, source_frequency,
                frames, block_size,
                pulp::test::audio::sample_interpolation_policy_cli_id(
                    policy));
    return 0;
}
