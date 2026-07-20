#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_heritage_live_cyclic.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kSampleRate = 48000;

const char* value_for(int argc, char** argv, int& index, const char* flag) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", flag);
        return nullptr;
    }
    return argv[++index];
}

void usage(const char* executable) {
    std::fprintf(stderr,
                 "usage: %s --source-out PATH --capture-out PATH "
                 "--fixture indexed|sparse --factor N --cycle-frames N "
                 "--splice-frames N --output-frames N\n",
                 executable);
}

float fixture_sample(const std::string& fixture, std::uint64_t frame,
                     std::uint64_t source_extent) {
    if (fixture == "indexed") {
        if (source_extent < 2)
            return 0.0f;
        return static_cast<float>(0.05 + 0.9 * static_cast<double>(frame) /
                                             static_cast<double>(source_extent - 1));
    }
    if (frame < 19 || (frame - 19) % 37 != 0)
        return 0.0f;
    constexpr float amplitudes[] = {1.0f, -0.75f, 0.5f, -0.25f};
    return amplitudes[((frame - 19) / 37) % 4];
}

} // namespace

int main(int argc, char** argv) {
    std::string source_out;
    std::string capture_out;
    std::string fixture;
    double factor = 0.0;
    std::size_t cycle_frames = 0;
    std::size_t splice_frames = 0;
    std::size_t output_frames = 0;
    for (int index = 1; index < argc; ++index) {
        const char* arg = argv[index];
        const auto read_string = [&](std::string& destination) {
            const auto* value = value_for(argc, argv, index, arg);
            if (value)
                destination = value;
            return value != nullptr;
        };
        const auto read_size = [&](std::size_t& destination) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value)
                return false;
            char* end = nullptr;
            const auto parsed = std::strtoull(value, &end, 10);
            if (!end || *end != '\0')
                return false;
            destination = static_cast<std::size_t>(parsed);
            return true;
        };
        if (std::strcmp(arg, "--source-out") == 0) {
            if (!read_string(source_out)) return 2;
        } else if (std::strcmp(arg, "--capture-out") == 0) {
            if (!read_string(capture_out)) return 2;
        } else if (std::strcmp(arg, "--fixture") == 0) {
            if (!read_string(fixture)) return 2;
        } else if (std::strcmp(arg, "--factor") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value) return 2;
            char* end = nullptr;
            factor = std::strtod(value, &end);
            if (!end || *end != '\0') return 2;
        } else if (std::strcmp(arg, "--cycle-frames") == 0) {
            if (!read_size(cycle_frames)) return 2;
        } else if (std::strcmp(arg, "--splice-frames") == 0) {
            if (!read_size(splice_frames)) return 2;
        } else if (std::strcmp(arg, "--output-frames") == 0) {
            if (!read_size(output_frames)) return 2;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (source_out.empty() || capture_out.empty() ||
        (fixture != "indexed" && fixture != "sparse") ||
        !std::isfinite(factor) || factor < 0.25 || factor > 20.0 ||
        cycle_frames < 4 || cycle_frames > 1000000 ||
        splice_frames > cycle_frames / 2 || output_frames < 128 ||
        output_frames > 10000000) {
        usage(argv[0]);
        return 2;
    }

    pulp::audio::SampleHeritageLiveCyclicConfig config;
    config.factor = factor;
    config.cycle_samples = cycle_frames;
    config.crossfade_samples = splice_frames;
    config.max_block_samples = 257;
    config.channel_count = 1;
    pulp::audio::SampleHeritageLiveCyclicStretch processor;
    if (processor.prepare(config) != pulp::audio::SampleHeritageLiveCyclicStatus::Ok) {
        std::fprintf(stderr, "cyclic calibration renderer: prepare failed\n");
        return 1;
    }

    const auto source_extent = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(output_frames * (factor + 1.0)),
        static_cast<std::uint64_t>(output_frames / factor + cycle_frames * 2));
    std::vector<float> source;
    std::vector<float> capture;
    capture.reserve(output_frames);
    std::uint64_t source_cursor = 0;
    for (std::size_t output_cursor = 0; output_cursor < output_frames;) {
        const auto count = std::min<std::size_t>(257, output_frames - output_cursor);
        const auto plan = processor.plan(count);
        if (!plan.valid()) {
            std::fprintf(stderr, "cyclic calibration renderer: plan failed\n");
            return 1;
        }
        std::vector<float> input(plan.input_frames);
        std::vector<float> output(count);
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            input[frame] = fixture_sample(fixture, source_cursor + frame, source_extent);
        source.insert(source.end(), input.begin(), input.end());
        const float* input_pointer = input.data();
        float* output_pointer = output.data();
        const pulp::audio::BufferView<const float> input_view(
            &input_pointer, 1, input.size());
        pulp::audio::BufferView<float> output_view(&output_pointer, 1, output.size());
        const auto status = processor.process(input_view, output_view);
        if (status != pulp::audio::SampleHeritageLiveCyclicStatus::Ok) {
            std::fprintf(stderr,
                         "cyclic calibration renderer: process failed "
                         "status=%u output-cursor=%zu input-frames=%zu\n",
                         static_cast<unsigned>(status), output_cursor, input.size());
            return 1;
        }
        capture.insert(capture.end(), output.begin(), output.end());
        source_cursor += input.size();
        output_cursor += count;
    }

    pulp::audio::AudioFileData source_file;
    source_file.sample_rate = kSampleRate;
    source_file.channels = {std::move(source)};
    pulp::audio::AudioFileData capture_file;
    capture_file.sample_rate = kSampleRate;
    capture_file.channels = {std::move(capture)};
    if (!pulp::audio::write_wav_file(source_out, source_file,
                                     pulp::audio::WavBitDepth::Float32) ||
        !pulp::audio::write_wav_file(capture_out, capture_file,
                                     pulp::audio::WavBitDepth::Float32)) {
        std::fprintf(stderr, "cyclic calibration renderer: WAV write failed\n");
        return 1;
    }
    return 0;
}
