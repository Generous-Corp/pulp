#include "audio_inspector_demo_processor.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::filesystem::path output;
    std::filesystem::path metadata_json;
    double duration_seconds = 4.0;
    double sample_rate = 48000.0;
    int block_size = 256;
    int channels = 2;
    float frequency_hz = 440.0f;
    float level_db = -12.0f;
};

void usage() {
    std::cerr
        << "Usage: pulp-audio-inspector-demo-render --output <proof.wav> "
           "[--metadata-json <file>] [--duration <seconds>] "
           "[--frequency <hz>] [--level-db <db>] [--sample-rate <hz>] "
           "[--block-size <frames>]\n";
}

std::string require_value(int& i, int argc, char** argv, std::string_view flag) {
    if (i + 1 >= argc)
        throw std::invalid_argument(std::string(flag) + " requires a value");
    return argv[++i];
}

double parse_double(const std::string& raw, std::string_view flag) {
    std::size_t consumed = 0;
    double value = std::stod(raw, &consumed);
    if (consumed != raw.size())
        throw std::invalid_argument(std::string(flag) + " must be numeric");
    return value;
}

int parse_int(const std::string& raw, std::string_view flag) {
    std::size_t consumed = 0;
    int value = std::stoi(raw, &consumed);
    if (consumed != raw.size())
        throw std::invalid_argument(std::string(flag) + " must be an integer");
    return value;
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else if (arg == "--output") {
            opts.output = require_value(i, argc, argv, arg);
        } else if (arg == "--metadata-json") {
            opts.metadata_json = require_value(i, argc, argv, arg);
        } else if (arg == "--duration") {
            opts.duration_seconds = parse_double(require_value(i, argc, argv, arg), arg);
        } else if (arg == "--frequency") {
            opts.frequency_hz = static_cast<float>(
                parse_double(require_value(i, argc, argv, arg), arg));
        } else if (arg == "--level-db") {
            opts.level_db = static_cast<float>(
                parse_double(require_value(i, argc, argv, arg), arg));
        } else if (arg == "--sample-rate") {
            opts.sample_rate = parse_double(require_value(i, argc, argv, arg), arg);
        } else if (arg == "--block-size") {
            opts.block_size = parse_int(require_value(i, argc, argv, arg), arg);
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }

    if (opts.output.empty())
        throw std::invalid_argument("--output is required");
    if (opts.duration_seconds <= 0.0 || opts.duration_seconds > 30.0)
        throw std::invalid_argument("--duration must be in the range (0, 30]");
    if (opts.sample_rate <= 0.0)
        throw std::invalid_argument("--sample-rate must be positive");
    if (opts.block_size <= 0)
        throw std::invalid_argument("--block-size must be positive");
    if (opts.frequency_hz <= 0.0f)
        throw std::invalid_argument("--frequency must be positive");
    if (opts.level_db > 0.0f)
        throw std::invalid_argument("--level-db must be <= 0");

    return opts;
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

void write_metadata(const Options& opts, std::uint64_t frames) {
    if (opts.metadata_json.empty())
        return;
    if (!opts.metadata_json.parent_path().empty())
        std::filesystem::create_directories(opts.metadata_json.parent_path());
    std::ofstream out(opts.metadata_json);
    if (!out)
        throw std::runtime_error("failed to open metadata JSON for write");
    out << "{\n"
        << "  \"schema\": \"pulp.video-proof.audio-render.v1\",\n"
        << "  \"source\": \"AudioInspectorDemoProcessor via HeadlessHost\",\n"
        << "  \"output_wav\": \"" << json_escape(opts.output.string()) << "\",\n"
        << "  \"sample_rate\": " << static_cast<std::uint32_t>(opts.sample_rate) << ",\n"
        << "  \"frames\": " << frames << ",\n"
        << "  \"channels\": " << opts.channels << ",\n"
        << "  \"block_size\": " << opts.block_size << ",\n"
        << "  \"frequency_hz\": " << opts.frequency_hz << ",\n"
        << "  \"level_db\": " << opts.level_db << ",\n"
        << "  \"duration_seconds\": " << opts.duration_seconds << "\n"
        << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto opts = parse_args(argc, argv);
        const auto total_frames = static_cast<std::uint64_t>(
            opts.duration_seconds * opts.sample_rate + 0.5);

        pulp::format::HeadlessHost host(pulp::examples::create_audio_inspector_demo);
        host.prepare(opts.sample_rate, opts.block_size, 0, opts.channels);
        host.state().set_value(pulp::examples::kFrequency, opts.frequency_hz);
        host.state().set_value(pulp::examples::kLevelDb, opts.level_db);

        pulp::audio::AudioFileData rendered;
        rendered.sample_rate = static_cast<std::uint32_t>(opts.sample_rate);
        rendered.channels.resize(static_cast<std::size_t>(opts.channels));
        for (auto& channel : rendered.channels)
            channel.resize(static_cast<std::size_t>(total_frames));

        pulp::midi::MidiBuffer midi_in;
        pulp::midi::MidiBuffer midi_out;
        std::vector<const float*> input_ptrs;
        for (std::uint64_t pos = 0; pos < total_frames;) {
            const auto block = static_cast<std::size_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(opts.block_size), total_frames - pos));
            pulp::audio::Buffer<float> block_output(
                static_cast<std::size_t>(opts.channels), block);
            pulp::audio::BufferView<const float> block_input(
                input_ptrs.data(), 0, block);
            auto output_view = block_output.view();
            host.process(output_view, block_input, midi_in, midi_out);

            for (int ch = 0; ch < opts.channels; ++ch) {
                auto source = block_output.channel(static_cast<std::size_t>(ch));
                auto& dest = rendered.channels[static_cast<std::size_t>(ch)];
                std::copy(source.begin(), source.end(),
                          dest.begin() + static_cast<std::ptrdiff_t>(pos));
            }
            pos += block;
        }

        if (!opts.output.parent_path().empty())
            std::filesystem::create_directories(opts.output.parent_path());
        if (!pulp::audio::write_wav_file(opts.output.string(), rendered))
            throw std::runtime_error("failed to write WAV: " + opts.output.string());
        write_metadata(opts, total_frames);

        std::cout << "wrote " << opts.output << " frames=" << total_frames
                  << " sample_rate=" << static_cast<std::uint32_t>(opts.sample_rate)
                  << " channels=" << opts.channels << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        usage();
        return 2;
    }
}
