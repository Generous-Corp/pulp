#include "cmd_audio_sampler_mip.hpp"
#include "json_writer.hpp"

#include <pulp/audio/sample_mip_sidecar.hpp>

#include <algorithm>
#include <cstdint>
#include <charconv>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {

void print_usage() {
    std::cout << "Usage: pulp audio sampler-mip build <source> [options]\n"
                 "\n"
                 "Build and atomically publish a 140 dB streamed-sampler mip sidecar.\n"
                 "\n"
                 "Inputs/outputs/runtime:\n"
                 "  Accepts strict ranged WAV or uncompressed AIFF/AIFF-C input.\n"
                 "  Decodes the admitted source in memory within the byte limits.\n"
                 "  Writes <source>.pulpmip and hash-addressed float32 WAV payloads\n"
                 "  beside the source, then transactionally replaces the old manifest\n"
                 "  and removes payloads owned only by that prior manifest.\n"
                 "  PulpSampler still requires 1-2 channels at <=192 kHz; only exact\n"
                 "  positive-octave Hermite/Lagrange forward one-shots select mips.\n"
                 "\n"
                 "Options:\n"
                 "  --levels <1|2>              Number of octave levels (default: 2)\n"
                 "  --max-source-bytes <n>      Input safety limit (default: 536870912)\n"
                 "  --max-output-bytes <n>      Decoded mip safety limit (default: 536870912)\n"
                 "  --json                      Emit a machine-readable result\n";
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](char character) {
            return character >= '0' && character <= '9';
        })) {
        return false;
    }
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value, 10);
    return error == std::errc{} && end == text.data() + text.size();
}

int print_result(bool json, const std::string& source,
                 const pulp::audio::SampleMipBuildResult& built) {
    if (json) {
        std::cout << "{\"ok\":" << (built.ok ? "true" : "false")
                  << ",\"source\":" << pulp::cli::json_string(source)
                  << ",\"manifest\":" << pulp::cli::json_string(built.manifest_path)
                  << ",\"payloads\":[";
        for (std::size_t index = 0; index < built.payload_paths.size(); ++index) {
            if (index != 0) std::cout << ',';
            std::cout << pulp::cli::json_string(built.payload_paths[index]);
        }
        std::cout << "]";
        if (!built.error.empty())
            std::cout << ",\"error\":" << pulp::cli::json_string(built.error);
        std::cout << "}\n";
    } else if (built.ok) {
        std::cout << "Published " << built.manifest_path << "\n";
        for (const auto& payload : built.payload_paths)
            std::cout << "  " << payload << "\n";
    } else {
        std::cerr << "Error: " << built.error << "\n";
    }
    return built.ok ? 0 : 1;
}

int print_error(bool json, const std::string& source, std::string error) {
    pulp::audio::SampleMipBuildResult failed;
    if (!source.empty()) failed.manifest_path = source + ".pulpmip";
    failed.error = std::move(error);
    return print_result(json, source, failed);
}

} // namespace

int cmd_audio_sampler_mip(const std::vector<std::string>& args) {
    // Output mode is a property of the whole invocation, including malformed
    // invocations whose parser fails before it reaches the flag in argv.
    const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return 0;
    }
    if (args[0] != "build") {
        if (!json) print_usage();
        return print_error(json, {}, "expected sampler-mip verb 'build'");
    }
    if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h")) {
        print_usage();
        return 0;
    }

    pulp::audio::SampleMipBuildOptions options;
    std::string source;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto& argument = args[index];
        if (argument == "--json") {
            continue;
        } else if (argument == "--levels" ||
                   argument == "--max-source-bytes" ||
                   argument == "--max-output-bytes") {
            if (++index == args.size()) {
                return print_error(json, source, argument + " requires a value");
            }
            std::uint64_t parsed = 0;
            if (!parse_u64(args[index], parsed))
                return print_error(json, source,
                                   argument + " requires an unsigned integer");
            if (argument == "--levels") {
                if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                    return print_error(json, source, "--levels is too large");
                }
                options.level_count = static_cast<std::uint32_t>(parsed);
            } else if (argument == "--max-source-bytes") {
                options.maximum_source_bytes = parsed;
            } else {
                options.maximum_output_bytes = parsed;
            }
        } else if (!argument.empty() && argument.front() == '-') {
            return print_error(json, source, "unknown option " + argument);
        } else if (source.empty()) {
            source = argument;
        } else {
            return print_error(json, source, "only one source may be provided");
        }
    }
    if (source.empty()) {
        return print_error(json, source, "source path is required");
    }

    pulp::audio::SampleMipBuildResult built;
    try {
        built = pulp::audio::build_sample_mip_sidecar(source, options);
    } catch (const std::exception& error) {
        built.manifest_path = source + ".pulpmip";
        built.error = std::string("sampler mip build failed: ") + error.what();
    } catch (...) {
        built.manifest_path = source + ".pulpmip";
        built.error = "sampler mip build failed: unknown exception";
    }
    return print_result(json, source, built);
}
