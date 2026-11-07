// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include "cmd_audio_inspect.hpp"
#include "cli_fs_util.hpp"

#include <pulp/audio/audio_file_layout.hpp>
#include <pulp/audio/audio_file_layout_json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <variant>

namespace fs = std::filesystem;

namespace {

// Exit code for "the inspector could not say anything about this input":
// the leading bytes match no container it knows. Distinct from 1 (bad
// arguments, unreadable file) so a script can tell them apart, matching the
// could-not-measure convention of `pulp audio validate`.
constexpr int kExitUnrecognized = 2;

void print_usage() {
    std::cout
        << "Usage: pulp audio inspect <file> [options]\n"
           "\n"
           "Report an audio file's byte layout without decoding it: every region\n"
           "with offset and size, the fields the format defines, the stream facts\n"
           "the header states, and any specification violation found.\n"
           "\n"
           "Reads are bounded to headers and small payloads; a multi-gigabyte file\n"
           "costs the same handful of reads as a 44-byte one.\n"
           "\n"
           "Options:\n"
           "  --json                    Emit pulp.audio.file_inspect.v1 JSON\n"
           "  --include-metadata-text   Lift free text (bext, LIST-INFO, iXML, comments, ID3)\n"
           "                            into metadata.text; off by default\n"
           "  --absolute-paths          Report the input path as given instead of its basename\n"
           "  --raw                     Treat the file as headerless PCM. Requires ALL of:\n"
           "    --format <s8|u8|s16|s24|s32|f32|f64>\n"
           "    --channels <n>  --rate <hz>  --endian <little|big>  --offset <bytes>\n"
           "\n"
           "Exit codes: 0 = report written; 1 = bad arguments or unreadable file;\n"
           "  2 = the leading bytes match no container the inspector knows (the JSON\n"
           "      report is still written, with container \"unknown\")\n";
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return error == std::errc{} && end == text.data() + text.size();
}

int usage_error(const std::string& message) {
    std::cerr << "Error: " << message << "\n";
    std::cerr << "Run `pulp audio inspect --help` for usage.\n";
    return 1;
}

std::string field_to_text(const pulp::audio::LayoutFieldValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, double>) {
                std::ostringstream out;
                out << std::setprecision(6) << v;
                return out.str();
            } else
                return std::to_string(v);
        },
        value);
}

void print_table(const pulp::audio::AudioFileLayoutReport& report, const std::string& name) {
    using namespace pulp::audio;
    std::cout << name << "  " << audio_container_name(report.container);
    if (!report.container_detail.empty())
        std::cout << "/" << report.container_detail;
    if (report.profile)
        std::cout << "  profile " << wav_profile_name(*report.profile);
    std::cout << "  " << (report.is_standard() ? "standard" : "violations") << "  "
              << facts_confidence_name(report.facts_confidence) << "  (" << report.file_size
              << " bytes, read " << report.bytes_read << ")\n";
    if (!report.error.empty())
        std::cout << "error: " << report.error << "\n";
    if (report.stream) {
        const auto& s = *report.stream;
        std::cout << "stream: " << s.sample_rate << " Hz, " << s.channels << " ch";
        if (s.bits_per_sample != 0u) {
            std::cout << ", " << s.bits_per_sample << "-bit";
            if (s.valid_bits != s.bits_per_sample)
                std::cout << " (" << s.valid_bits << " valid)";
        }
        std::cout << " " << s.encoding;
        if (s.frames != 0u)
            std::cout << ", " << s.frames << " frames (" << std::fixed << std::setprecision(3)
                      << s.duration_seconds << " s)" << std::defaultfloat;
        else
            std::cout << ", frame count not stated";
        std::cout << "\n";
    }
    if (report.decode) {
        std::cout << "decodable: " << (report.decode->decodable ? "yes" : "no");
        if (!report.decode->reason.empty())
            std::cout << " (" << report.decode->reason << ")";
        std::cout << "\n";
    }
    if (report.decode_limits && report.decode_limits->exceeds())
        std::cout << "exceeds decode limits: " << report.decode_limits->describe() << "\n";
    if (report.truncated_tail_bytes)
        std::cout << "truncated tail bytes: " << *report.truncated_tail_bytes << "\n";

    std::cout << "regions:\n";
    std::cout << "  " << std::left << std::setw(12) << "offset" << std::setw(12) << "size"
              << std::setw(16) << "id" << std::setw(10) << "kind" << "fields\n";
    for (const auto& region : report.regions) {
        std::ostringstream size_text;
        size_text << region.declared_size;
        if (region.actual_size != region.declared_size)
            size_text << "/" << region.actual_size;
        std::cout << "  " << std::left << std::setw(12) << region.offset << std::setw(12)
                  << size_text.str() << std::setw(16) << (region.id.empty() ? "-" : region.id)
                  << std::setw(10) << layout_region_kind_name(region.kind);
        bool first = true;
        for (const auto& field : region.fields) {
            std::cout << (first ? "" : " ") << field.name << "=" << field_to_text(field.value);
            first = false;
        }
        if (!region.note.empty())
            std::cout << (first ? "" : " ") << "(" << region.note << ")";
        std::cout << "\n";
    }
    if (report.regions_truncated)
        std::cout << "  ... region list capped\n";
    if (!report.warnings.empty()) {
        std::cout << "warnings:\n";
        for (const auto& warning : report.warnings)
            std::cout << "  - " << layout_warning_kind_name(warning.kind) << " @" << warning.offset
                      << ": " << warning.detail << "\n";
    }
    const auto metadata = report.metadata_regions();
    if (!metadata.empty()) {
        std::cout << "metadata present:";
        for (const auto* region : metadata)
            std::cout << " " << region->id << "(" << region->offset << "," << region->declared_size
                      << ")";
        std::cout << "\n";
    }
    for (const auto& text : report.metadata_text)
        std::cout << "  " << text.region_id << "." << text.key << ": " << text.text << "\n";
}

} // namespace

int cmd_audio_inspect(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return args.empty() ? 1 : 0;
    }

    std::string file;
    bool json = false;
    bool include_text = false;
    bool absolute_paths = false;
    bool raw = false;
    std::optional<std::string> raw_format;
    std::optional<std::string> raw_endian;
    std::optional<std::uint64_t> raw_channels;
    std::optional<std::uint64_t> raw_rate;
    std::optional<std::uint64_t> raw_offset;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto value = [&](std::string& out) -> bool {
            if (i + 1u >= args.size())
                return false;
            out = args[++i];
            return true;
        };
        std::string text;
        if (arg == "--json") {
            json = true;
        } else if (arg == "--include-metadata-text") {
            include_text = true;
        } else if (arg == "--absolute-paths") {
            absolute_paths = true;
        } else if (arg == "--raw") {
            raw = true;
        } else if (arg == "--format") {
            if (!value(text))
                return usage_error("--format requires a value");
            raw_format = text;
        } else if (arg == "--endian") {
            if (!value(text))
                return usage_error("--endian requires a value");
            raw_endian = text;
        } else if (arg == "--channels" || arg == "--rate" || arg == "--offset") {
            if (!value(text))
                return usage_error(arg + " requires a value");
            std::uint64_t parsed = 0u;
            if (!parse_u64(text, parsed))
                return usage_error(arg + " requires an unsigned integer");
            if (arg == "--channels")
                raw_channels = parsed;
            else if (arg == "--rate")
                raw_rate = parsed;
            else
                raw_offset = parsed;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg.front() == '-') {
            return usage_error("unknown option " + arg);
        } else if (file.empty()) {
            file = arg;
        } else {
            return usage_error("only one file may be inspected per invocation");
        }
    }
    if (file.empty())
        return usage_error("a file path is required");

    pulp::audio::LayoutInspectOptions options;
    options.include_metadata_text = include_text;
    const bool any_raw_field = raw_format || raw_endian || raw_channels || raw_rate || raw_offset;
    if (raw) {
        // Every field is required: the inspector never guesses a raw PCM
        // shape from its content, and a partial description would be one.
        std::vector<std::string> missing;
        if (!raw_format)
            missing.emplace_back("--format");
        if (!raw_channels)
            missing.emplace_back("--channels");
        if (!raw_rate)
            missing.emplace_back("--rate");
        if (!raw_endian)
            missing.emplace_back("--endian");
        if (!raw_offset)
            missing.emplace_back("--offset");
        if (!missing.empty()) {
            std::string list;
            for (const auto& name : missing)
                list += (list.empty() ? "" : ", ") + name;
            return usage_error("--raw requires every one of --format, --channels, --rate, "
                               "--endian, --offset; missing " +
                               list);
        }
        pulp::audio::RawPcmSpec spec;
        const auto format = pulp::audio::parse_raw_pcm_format(*raw_format);
        if (!format)
            return usage_error("--format must be one of s8, u8, s16, s24, s32, f32, f64");
        spec.format = *format;
        if (*raw_endian == "little")
            spec.endian = pulp::audio::RawPcmSpec::Endian::little;
        else if (*raw_endian == "big")
            spec.endian = pulp::audio::RawPcmSpec::Endian::big;
        else
            return usage_error("--endian must be little or big");
        if (*raw_channels == 0u || *raw_channels > std::numeric_limits<std::uint32_t>::max())
            return usage_error("--channels must be between 1 and 4294967295");
        if (*raw_rate == 0u || *raw_rate > std::numeric_limits<std::uint32_t>::max())
            return usage_error("--rate must be between 1 and 4294967295");
        spec.channels = static_cast<std::uint32_t>(*raw_channels);
        spec.sample_rate = static_cast<std::uint32_t>(*raw_rate);
        spec.offset = *raw_offset;
        options.raw = spec;
    } else if (any_raw_field) {
        return usage_error(
            "--format, --channels, --rate, --endian and --offset apply only with --raw");
    }

    const std::string name = pulp::cli::fsutil::display_input_name(file, absolute_paths);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        std::cerr << "Error: not a readable file: " << name << "\n";
        return 1;
    }
    const auto report = pulp::audio::inspect_audio_file_layout(fs::path(file), options);

    if (json) {
        pulp::audio::LayoutJsonOptions json_options;
        json_options.input_name = name;
        json_options.include_metadata_text = include_text;
        try {
            std::cout << pulp::audio::audio_file_layout_to_json(report, json_options) << "\n";
        } catch (const std::exception& error) {
            // The report sanitises file text, so this is a defect, not an
            // input problem; say so instead of aborting with no output.
            std::cerr << "Error: report could not be serialized: " << error.what() << "\n";
            return 1;
        }
    } else {
        print_table(report, name);
    }
    if (!report.error.empty()) {
        std::cerr << "Error: " << report.error << "\n";
        return 1;
    }
    if (report.container == pulp::audio::AudioContainer::unknown) {
        std::cerr << "pulp audio inspect: leading bytes match no known audio container\n";
        return kExitUnrecognized;
    }
    return 0;
}
