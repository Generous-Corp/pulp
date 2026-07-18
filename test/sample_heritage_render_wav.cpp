#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_heritage.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <numbers>
#include <string>
#include <vector>

namespace {

constexpr double kHostRate = 48000.0;
constexpr float kAmplitude = 0.5f;

const char* value_for(int argc, char** argv, int& index, const char* flag) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "pulp-sampler-heritage-render-wav: %s needs a value\n",
                     flag);
        return nullptr;
    }
    return argv[++index];
}

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --source-out PATH --candidate-out PATH "
        "--input-ratio A --return-ratio B [--source-frequency CYCLES] "
        "[--impulse-index N] [--frames N] [--block-size N]\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string source_out;
    std::string candidate_out;
    double input_ratio = 0.0;
    double return_ratio = 0.0;
    double source_frequency = 0.03125;
    int impulse_index = -1;
    int frames = 16384;
    int block_size = 64;

    for (int index = 1; index < argc; ++index) {
        const char* arg = argv[index];
        if (std::strcmp(arg, "--source-out") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            source_out = value;
        } else if (std::strcmp(arg, "--candidate-out") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            candidate_out = value;
        } else if (std::strcmp(arg, "--input-ratio") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            input_ratio = std::strtod(value, nullptr);
        } else if (std::strcmp(arg, "--return-ratio") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            return_ratio = std::strtod(value, nullptr);
        } else if (std::strcmp(arg, "--source-frequency") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            source_frequency = std::strtod(value, nullptr);
        } else if (std::strcmp(arg, "--impulse-index") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            impulse_index = std::atoi(value);
        } else if (std::strcmp(arg, "--frames") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            frames = std::atoi(value);
        } else if (std::strcmp(arg, "--block-size") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            block_size = std::atoi(value);
        } else if (std::strcmp(arg, "--help") == 0 ||
                   std::strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr,
                         "pulp-sampler-heritage-render-wav: unknown argument '%s'\n",
                         arg);
            usage(argv[0]);
            return 2;
        }
    }

    if (source_out.empty() || candidate_out.empty() ||
        !(input_ratio >= 0.25 && input_ratio <= 4.0) ||
        !(return_ratio >= 0.25 && return_ratio <= 4.0) ||
        !(source_frequency > 0.0 && source_frequency < 0.5) ||
        frames < 1024 || block_size < 1 || block_size > frames ||
        impulse_index < -1) {
        std::fprintf(stderr,
                     "pulp-sampler-heritage-render-wav: invalid render contract\n");
        usage(argv[0]);
        return 2;
    }

    try {
        const double machine_rate = kHostRate / input_ratio;
        const double clock_ratio = input_ratio * return_ratio;
        pulp::audio::SampleHeritageProfile profile{
            .schema_version = pulp::audio::kSampleHeritageProfileSchemaVersion,
            .profile_id = "neutral.quality-lab-heritage",
            .host_sample_rate = kHostRate,
            .stages = {
                {false, pulp::audio::SampleHeritageMachineDomainStage{machine_rate}},
                {false, pulp::audio::SampleHeritageClockPitchStage{clock_ratio}},
            },
        };
        const auto validated = pulp::audio::validate_sample_heritage_profile(profile);
        if (!validated.valid()) {
            std::fprintf(stderr,
                         "pulp-sampler-heritage-render-wav: profile validation failed\n");
            return 1;
        }

        pulp::audio::SampleHeritageEngine engine;
        if (engine.prepare({validated.profile, 1,
                            static_cast<std::size_t>(block_size)}) !=
            pulp::audio::SampleHeritagePrepareStatus::Ok) {
            std::fprintf(stderr,
                         "pulp-sampler-heritage-render-wav: prepare failed\n");
            return 1;
        }

        std::vector<float> input(engine.maximum_input_frames());
        std::vector<float> block(static_cast<std::size_t>(block_size));
        std::vector<float> source;
        std::vector<float> candidate;
        candidate.reserve(static_cast<std::size_t>(frames));
        std::uint64_t source_cursor = 0;
        for (int output_cursor = 0; output_cursor < frames;) {
            const auto count = static_cast<std::size_t>(
                std::min(block_size, frames - output_cursor));
            const auto plan = engine.plan_exact(count);
            if (!plan.valid() || plan.input_frames > input.size()) {
                std::fprintf(stderr,
                             "pulp-sampler-heritage-render-wav: plan failed\n");
                return 1;
            }
            for (std::size_t frame = 0; frame < plan.input_frames; ++frame) {
                const auto absolute = source_cursor + frame;
                input[frame] = impulse_index >= 0
                    ? (absolute == static_cast<std::uint64_t>(impulse_index)
                           ? 1.0f : 0.0f)
                    : static_cast<float>(
                          kAmplitude * std::sin(2.0 * std::numbers::pi *
                                                source_frequency *
                                                static_cast<double>(absolute)));
            }
            source.insert(source.end(), input.begin(),
                          input.begin() + static_cast<std::ptrdiff_t>(plan.input_frames));
            const float* input_pointer = input.data();
            float* output_pointer = block.data();
            const pulp::audio::BufferView<const float> input_view(
                &input_pointer, 1, plan.input_frames);
            pulp::audio::BufferView<float> output_view(&output_pointer, 1, count);
            if (engine.process_exact(plan, input_view, output_view) !=
                pulp::audio::SampleHeritageProcessStatus::Ok) {
                std::fprintf(stderr,
                             "pulp-sampler-heritage-render-wav: process failed\n");
                return 1;
            }
            candidate.insert(candidate.end(), block.begin(),
                             block.begin() + static_cast<std::ptrdiff_t>(count));
            source_cursor += plan.input_frames;
            output_cursor += static_cast<int>(count);
        }

        pulp::audio::AudioFileData source_file;
        source_file.sample_rate = static_cast<std::uint32_t>(kHostRate);
        source_file.channels = {std::move(source)};
        pulp::audio::AudioFileData candidate_file;
        candidate_file.sample_rate = static_cast<std::uint32_t>(kHostRate);
        candidate_file.channels = {std::move(candidate)};
        if (!pulp::audio::write_wav_file(
                source_out, source_file, pulp::audio::WavBitDepth::Float32) ||
            !pulp::audio::write_wav_file(
                candidate_out, candidate_file,
                pulp::audio::WavBitDepth::Float32)) {
            std::fprintf(stderr,
                         "pulp-sampler-heritage-render-wav: WAV write failed\n");
            return 1;
        }
        std::printf(
            "wrote source=%s candidate=%s A=%.9g B=%.9g clock=%.9g "
            "latency-output=%.9g frames=%d block=%d\n",
            source_out.c_str(), candidate_out.c_str(), input_ratio, return_ratio,
            clock_ratio, engine.latency_output_frames(), frames, block_size);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "pulp-sampler-heritage-render-wav: %s\n",
                     error.what());
        return 1;
    }
    return 0;
}
