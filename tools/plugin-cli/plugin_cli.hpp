#pragma once

/// @file plugin_cli.hpp
/// Generic CLI harness that turns any Pulp Processor into a command-line
/// audio processing tool. Reads WAV input, processes through the plugin,
/// writes WAV output.
///
/// Usage in a plugin project:
/// @code
/// #include "my_processor.hpp"
/// #include <pulp/tools/plugin_cli.hpp>
///
/// int main(int argc, char* argv[]) {
///     return pulp::tools::run_plugin_cli(my_ns::create_my_plugin, argc, argv);
/// }
/// @endcode

#include <pulp/format/headless.hpp>
#include <pulp/audio/audio_file.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>

namespace pulp::tools {

/// Run a Processor as a command-line audio processing tool.
///
/// Supports:
/// - `--input file.wav` — input audio file
/// - `--output file.wav` — output audio file
/// - `--param id=value` — set parameter by ID
/// - `--sample-rate N` — override sample rate (default: from input file)
/// - `--buffer-size N` — processing buffer size (default: 512)
/// - `--info` — print plugin info and parameters, then exit
/// - `--list-params` — list all parameters with ranges
///
/// @return 0 on success, 1 on error
inline int run_plugin_cli(format::ProcessorFactory factory, int argc, char* argv[]) {
    std::string input_path, output_path;
    std::vector<std::pair<uint32_t, float>> param_overrides;
    int buffer_size = 512;
    int sample_rate_override = 0;
    bool info_only = false;
    bool list_params = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input_path = argv[++i];
        else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            output_path = argv[++i];
        else if (std::strcmp(argv[i], "--param") == 0 && i + 1 < argc) {
            std::string pv = argv[++i];
            auto eq = pv.find('=');
            if (eq != std::string::npos) {
                auto id = static_cast<uint32_t>(std::stoul(pv.substr(0, eq)));
                auto val = std::stof(pv.substr(eq + 1));
                param_overrides.push_back({id, val});
            }
        }
        else if (std::strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc)
            sample_rate_override = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--buffer-size") == 0 && i + 1 < argc)
            buffer_size = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--info") == 0)
            info_only = true;
        else if (std::strcmp(argv[i], "--list-params") == 0)
            list_params = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: <plugin-cli> [options]\n\n"
                      << "Options:\n"
                      << "  --input file.wav     Input audio file\n"
                      << "  --output file.wav    Output audio file\n"
                      << "  --param id=value     Set parameter (repeatable)\n"
                      << "  --sample-rate N      Override sample rate\n"
                      << "  --buffer-size N      Processing block size (default: 512)\n"
                      << "  --info               Print plugin info and exit\n"
                      << "  --list-params        List all parameters with ranges\n"
                      << "  --help               Show this help\n";
            return 0;
        }
    }

    // Create host
    format::HeadlessHost host(factory);
    auto desc = host.descriptor();

    if (info_only) {
        std::cout << "Plugin: " << desc.name << "\n"
                  << "Manufacturer: " << desc.manufacturer << "\n"
                  << "Version: " << desc.version << "\n"
                  << "Category: " << (desc.category == format::PluginCategory::Effect ? "Effect" :
                                      desc.category == format::PluginCategory::Instrument ? "Instrument" : "MIDI Effect") << "\n"
                  << "Accepts MIDI: " << (desc.accepts_midi ? "yes" : "no") << "\n"
                  << "Parameters: " << host.state().param_count() << "\n";
        return 0;
    }

    if (list_params) {
        for (const auto& p : host.state().all_params()) {
            std::cout << "  " << p.id << ": " << p.name
                      << " [" << p.range.min << " .. " << p.range.max
                      << "] default=" << p.range.default_value;
            if (!p.unit.empty()) std::cout << " " << p.unit;
            std::cout << "\n";
        }
        return 0;
    }

    if (input_path.empty() || output_path.empty()) {
        std::cerr << "Error: --input and --output are required\n";
        std::cerr << "Run with --help for usage\n";
        return 1;
    }

    // Read input
    auto audio_data = audio::read_audio_file(input_path);
    if (!audio_data) {
        std::cerr << "Error: failed to read " << input_path << "\n";
        return 1;
    }

    int sr = sample_rate_override > 0 ? sample_rate_override : static_cast<int>(audio_data->sample_rate);
    int channels = static_cast<int>(audio_data->num_channels());
    int total_frames = static_cast<int>(audio_data->num_frames());

    // Prepare
    host.prepare(sr, buffer_size, channels, channels);

    // Apply parameter overrides
    for (auto& [id, val] : param_overrides)
        host.state().set_value(id, val);

    // Process in blocks
    audio::AudioFileData output_data;
    output_data.sample_rate = static_cast<uint32_t>(sr);
    output_data.channels.resize(static_cast<size_t>(channels));
    for (auto& ch : output_data.channels)
        ch.resize(static_cast<size_t>(total_frames));

    audio::Buffer<float> in_buf(static_cast<size_t>(channels), static_cast<size_t>(buffer_size));
    audio::Buffer<float> out_buf(static_cast<size_t>(channels), static_cast<size_t>(buffer_size));

    int pos = 0;
    while (pos < total_frames) {
        int block = std::min(buffer_size, total_frames - pos);

        // Copy input block
        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < block; ++i)
                in_buf.channel(static_cast<size_t>(ch))[static_cast<size_t>(i)] =
                    audio_data->channels[static_cast<size_t>(ch)][static_cast<size_t>(pos + i)];
        }

        auto iv = in_buf.view();
        auto ov = out_buf.view();
        host.process(ov, iv);

        // Copy output block
        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < block; ++i)
                output_data.channels[static_cast<size_t>(ch)][static_cast<size_t>(pos + i)] =
                    out_buf.channel(static_cast<size_t>(ch))[static_cast<size_t>(i)];
        }

        pos += block;
    }

    host.release();

    // Write output
    if (!audio::write_wav_file(output_path, output_data)) {
        std::cerr << "Error: failed to write " << output_path << "\n";
        return 1;
    }

    std::cout << "Processed " << total_frames << " frames (" << channels << " ch, " << sr << " Hz)\n";
    std::cout << "Output: " << output_path << "\n";
    return 0;
}

} // namespace pulp::tools
