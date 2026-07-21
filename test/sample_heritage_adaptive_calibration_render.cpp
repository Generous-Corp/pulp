#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_heritage_record_commit.hpp>

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

bool read_size(int argc, char** argv, int& index, const char* flag,
               std::size_t& destination) {
    const auto* value = value_for(argc, argv, index, flag);
    if (!value)
        return false;
    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (!end || *end != '\0')
        return false;
    destination = static_cast<std::size_t>(parsed);
    return true;
}

void usage(const char* executable) {
    std::fprintf(stderr,
                 "usage: %s --source-out PATH --capture-out PATH --factor N "
                 "--decision-hop N --search-radius N --search-stride N "
                 "--crossfade N --source-frames N\n",
                 executable);
}

}  // namespace

int main(int argc, char** argv) {
    std::string source_out;
    std::string capture_out;
    double factor = 0.0;
    std::size_t decision_hop = 0;
    std::size_t search_radius = 0;
    std::size_t search_stride = 0;
    std::size_t crossfade = 0;
    std::size_t source_frames = 0;
    for (int index = 1; index < argc; ++index) {
        const char* arg = argv[index];
        if (std::strcmp(arg, "--source-out") == 0 ||
            std::strcmp(arg, "--capture-out") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value)
                return 2;
            (std::strcmp(arg, "--source-out") == 0 ? source_out : capture_out) = value;
        } else if (std::strcmp(arg, "--factor") == 0) {
            const auto* value = value_for(argc, argv, index, arg);
            if (!value)
                return 2;
            char* end = nullptr;
            factor = std::strtod(value, &end);
            if (!end || *end != '\0')
                return 2;
        } else if (std::strcmp(arg, "--decision-hop") == 0) {
            if (!read_size(argc, argv, index, arg, decision_hop)) return 2;
        } else if (std::strcmp(arg, "--search-radius") == 0) {
            if (!read_size(argc, argv, index, arg, search_radius)) return 2;
        } else if (std::strcmp(arg, "--search-stride") == 0) {
            if (!read_size(argc, argv, index, arg, search_stride)) return 2;
        } else if (std::strcmp(arg, "--crossfade") == 0) {
            if (!read_size(argc, argv, index, arg, crossfade)) return 2;
        } else if (std::strcmp(arg, "--source-frames") == 0) {
            if (!read_size(argc, argv, index, arg, source_frames)) return 2;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (source_out.empty() || capture_out.empty() || !std::isfinite(factor) ||
        factor < 0.25 || factor > 20.0 || decision_hop < 4 ||
        decision_hop > 1000000 || search_radius > 1000000 ||
        search_stride < 1 || search_stride > 1000000 || crossfade < 2 ||
        crossfade > decision_hop / 2 || source_frames < 512 ||
        source_frames > 10000000) {
        usage(argv[0]);
        return 2;
    }

    pulp::audio::Buffer<float> source(1, source_frames);
    for (std::size_t frame = 0; frame < source_frames; ++frame)
        source.channel(0)[frame] = static_cast<float>(
            0.05 + 0.9 * static_cast<double>(frame) / static_cast<double>(source_frames - 1));

    pulp::audio::SampleHeritageProfile profile;
    profile.profile_id = "neutral.adaptive-calibration-render";
    profile.host_sample_rate = kSampleRate;
    pulp::audio::SampleHeritageRecordCommitBlockSpec stretch;
    stretch.bypass = false;
    stretch.parameters = pulp::audio::SampleHeritageRecordCommitAdaptiveStretchBlock{
        .factor = factor,
        .decision_hop_samples = static_cast<std::uint32_t>(decision_hop),
        .search_radius_samples = static_cast<std::uint32_t>(search_radius),
        .search_stride_samples = static_cast<std::uint32_t>(search_stride),
        .crossfade_samples = static_cast<std::uint32_t>(crossfade),
        .stereo_link = true,
    };
    profile.record_commit.push_back(stretch);
    const auto committed = pulp::audio::commit_sample_heritage_recording(
        profile, static_cast<const pulp::audio::Buffer<float>&>(source).view(), kSampleRate,
        {.source_id = "fixture:indexed-basis",
         .capture_method = "synthetic-product-render",
         .evidence_id = "heritage:adaptive-bootstrap"});
    if (!committed.valid()) {
        std::fprintf(stderr, "adaptive calibration renderer failed: %s\n",
                     committed.detail.c_str());
        return 1;
    }

    pulp::audio::AudioFileData source_file;
    source_file.sample_rate = kSampleRate;
    source_file.channels = {std::vector<float>(source.channel(0).begin(), source.channel(0).end())};
    const auto& captured = committed.asset->audio();
    pulp::audio::AudioFileData capture_file;
    capture_file.sample_rate = kSampleRate;
    capture_file.channels = {
        std::vector<float>(captured.channel(0).begin(), captured.channel(0).end())};
    if (!pulp::audio::write_wav_file(source_out, source_file,
                                     pulp::audio::WavBitDepth::Float32) ||
        !pulp::audio::write_wav_file(capture_out, capture_file,
                                     pulp::audio::WavBitDepth::Float32)) {
        std::fprintf(stderr, "adaptive calibration renderer: WAV write failed\n");
        return 1;
    }
    return 0;
}
