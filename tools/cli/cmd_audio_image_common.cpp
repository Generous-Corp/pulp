// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include "cmd_audio_image_common.hpp"

#include "cli_common.hpp"
#include "json_writer.hpp"

#include <pulp/audio/format_registry.hpp>
#include <pulp/render/headless_surface.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>

extern bool parse_size_arg(const std::string& text, const char* flag, std::size_t& out);
extern bool parse_double_arg(const std::string& text, const char* flag, double& out);

namespace fs = std::filesystem;

namespace pulp::cli::audio_image {

namespace {

bool parse_u32_arg(const std::string& text, const char* flag, std::uint32_t& out, bool positive) {
    std::size_t parsed = 0;
    if (!parse_size_arg(text, flag, parsed))
        return false;
    if (positive && parsed == 0) {
        std::cerr << "Error: " << flag << " must be a positive integer\n";
        return false;
    }
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "Error: " << flag << " is too large\n";
        return false;
    }
    out = static_cast<std::uint32_t>(parsed);
    return true;
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec))
        return true;
    const auto na = fs::absolute(a, ec).lexically_normal();
    const auto nb = fs::absolute(b, ec).lexically_normal();
    return !na.empty() && na == nb;
}

std::string json_number(double value) {
    if (!std::isfinite(value))
        return "null";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

std::string platform_name() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

} // namespace

const char* common_flags_help() {
    return "  --png <out.png>          Output image (required)\n"
           "  --json <sidecar.json>    Sidecar path (default: <out.png>.json)\n"
           "  --start <seconds>        Range start (default: 0)\n"
           "  --end <seconds>          Range end (default: end of file)\n"
           "  --channel <n>            Channel index to render (default: 0)\n"
           "  --width <px>             Plot width in pixels\n"
           "  --height <px>            Plot height in pixels\n"
           "  --absolute-paths         Write full paths into the sidecar instead of basenames\n";
}

CommonArg parse_common_arg(const std::vector<std::string>& args, std::size_t& index,
                           ImageArgs& out) {
    const std::string& arg = args[index];
    auto take_value = [&](const char* flag, std::string& value) {
        if (index + 1 >= args.size()) {
            std::cerr << "Error: " << flag << " requires a value\n";
            return false;
        }
        value = args[++index];
        return true;
    };
    std::string value;
    if (arg == "--absolute-paths") {
        out.absolute_paths = true;
        return CommonArg::consumed;
    }
    if (arg == "--png") {
        if (!take_value("--png", out.png))
            return CommonArg::error;
        return CommonArg::consumed;
    }
    if (arg == "--json") {
        if (!take_value("--json", out.json))
            return CommonArg::error;
        return CommonArg::consumed;
    }
    if (arg == "--start" || arg == "--end") {
        if (!take_value(arg.c_str(), value))
            return CommonArg::error;
        double seconds = 0.0;
        if (!parse_double_arg(value, arg.c_str(), seconds))
            return CommonArg::error;
        if (seconds < 0.0) {
            std::cerr << "Error: " << arg << " must not be negative\n";
            return CommonArg::error;
        }
        (arg == "--start" ? out.start_s : out.end_s) = seconds;
        return CommonArg::consumed;
    }
    if (arg == "--channel") {
        if (!take_value("--channel", value))
            return CommonArg::error;
        return parse_u32_arg(value, "--channel", out.channel, false) ? CommonArg::consumed
                                                                     : CommonArg::error;
    }
    if (arg == "--width" || arg == "--height") {
        if (!take_value(arg.c_str(), value))
            return CommonArg::error;
        std::uint32_t& target = arg == "--width" ? out.width : out.height;
        if (!parse_u32_arg(value, arg.c_str(), target, true))
            return CommonArg::error;
        if (target > 16384) {
            std::cerr << "Error: " << arg << " must not exceed 16384\n";
            return CommonArg::error;
        }
        return CommonArg::consumed;
    }
    return CommonArg::not_common;
}

std::string sidecar_path(const ImageArgs& args) {
    return args.json.empty() ? args.png + ".json" : args.json;
}

std::string display_path(const ImageArgs& args, const std::string& path) {
    if (args.absolute_paths) {
        std::error_code ec;
        const auto absolute = fs::absolute(path, ec);
        return ec ? path : absolute.lexically_normal().string();
    }
    return fs::path(path).filename().string();
}

int open_input(const ImageArgs& args, OpenedInput& out) {
    if (args.input.empty()) {
        std::cerr << "Error: input audio file is required\n";
        return 1;
    }
    if (args.png.empty()) {
        std::cerr << "Error: --png <out.png> is required\n";
        return 1;
    }
    std::error_code ec;
    if (!fs::is_regular_file(args.input, ec)) {
        std::cerr << "Error: input is not a readable file: " << args.input << "\n";
        return 1;
    }
    const std::string sidecar = sidecar_path(args);
    if (same_file(args.png, args.input)) {
        std::cerr << "Error: --png must not be the input file\n";
        return 1;
    }
    if (same_file(sidecar, args.input)) {
        std::cerr << "Error: --json must not be the input file\n";
        return 1;
    }
    if (same_file(sidecar, args.png)) {
        std::cerr << "Error: --json must not be the PNG path\n";
        return 1;
    }
    if (args.start_s && args.end_s && *args.end_s <= *args.start_s) {
        std::cerr << "Error: --end must be greater than --start\n";
        return 1;
    }

    if (!out.reader.open(args.input)) {
        std::cerr << "Error: could not open audio file: " << args.input << "\n";
        return 1;
    }
    out.path = args.input;
    out.info = out.reader.info();
    if (out.info.sample_rate == 0 || out.info.num_channels == 0) {
        std::cerr << "Error: audio file has no usable format information: " << args.input << "\n";
        return 1;
    }
    if (out.info.num_frames == 0) {
        std::cerr << "Error: audio file has no frames to render: " << args.input << "\n";
        return 2;
    }
    if (args.channel >= out.info.num_channels) {
        std::cerr << "Error: --channel " << args.channel << " is out of range; the file has "
                  << out.info.num_channels << " channel" << (out.info.num_channels == 1 ? "" : "s")
                  << " (0.." << out.info.num_channels - 1 << ")\n";
        return 1;
    }

    const double sample_rate = static_cast<double>(out.info.sample_rate);
    const double duration_s = static_cast<double>(out.info.num_frames) / sample_rate;
    const double start_s = args.start_s.value_or(0.0);
    const double end_s = std::min(args.end_s.value_or(duration_s), duration_s);
    if (start_s >= duration_s) {
        std::cerr << "Error: --start " << start_s << " s is at or past the end of the file ("
                  << duration_s << " s)\n";
        return 1;
    }
    out.start_frame = static_cast<std::uint64_t>(std::floor(start_s * sample_rate));
    out.end_frame = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(std::ceil(end_s * sample_rate)), out.info.num_frames);
    if (out.end_frame <= out.start_frame) {
        std::cerr << "Error: the requested range contains no frames\n";
        return 1;
    }
    out.start_s = static_cast<double>(out.start_frame) / sample_rate;
    out.end_s = static_cast<double>(out.end_frame) / sample_rate;

    const auto extension = fs::path(args.input).extension().string();
    if (auto* reader = pulp::audio::FormatRegistry::instance().find_reader(extension))
        out.decoder_name = reader->format_name();
    else
        out.decoder_name = out.info.format;

    const auto file_size = fs::file_size(args.input, ec);
    auto hash = pulp::runtime::sha256_file_hex(
        args.input, ec ? std::numeric_limits<std::uint64_t>::max() : file_size);
    if (!hash) {
        std::cerr << "Error: could not hash input file: " << args.input << "\n";
        return 1;
    }
    out.sha256 = *hash;
    return 0;
}

bool stream_channel(OpenedInput& input, std::uint32_t channel, std::uint64_t window_frames,
                    const std::function<void(std::span<const float>)>& sink, StreamStats& stats) {
    window_frames = std::max<std::uint64_t>(window_frames, 1);
    // read_frames fills the first N channels, so N buffers are needed to reach
    // channel N-1. Windows bound the cost regardless of file length.
    const std::uint32_t buffers = channel + 1;
    std::vector<std::vector<float>> storage(
        buffers, std::vector<float>(static_cast<std::size_t>(window_frames)));
    std::vector<float*> pointers(buffers);
    for (std::uint32_t c = 0; c < buffers; ++c)
        pointers[c] = storage[c].data();

    for (std::uint64_t frame = input.start_frame; frame < input.end_frame;) {
        const std::uint64_t count = std::min(window_frames, input.end_frame - frame);
        if (!input.reader.read_frames(pointers.data(), buffers, frame, count)) {
            std::cerr << "Error: failed to read frames " << frame << ".." << frame + count << "\n";
            return false;
        }
        std::span<const float> window(storage[channel].data(), static_cast<std::size_t>(count));
        for (float sample : window) {
            const double magnitude = std::abs(static_cast<double>(sample));
            if (magnitude > stats.peak_abs)
                stats.peak_abs = magnitude;
        }
        stats.frames += count;
        sink(window);
        frame += count;
    }
    return true;
}

std::string footer_text(const OpenedInput& input, const std::string& settings_summary) {
    // The footer is always the basename: it is burned into pixels that may be
    // shared anywhere, so the --absolute-paths opt-in does not apply here.
    std::string footer = fs::path(input.path).filename().string();
    footer += "  sha256:" + input.sha256.substr(0, 12);
    if (!settings_summary.empty())
        footer += "  " + settings_summary;
    footer += "  pulp ";
    footer += PULP_SDK_VERSION;
    return footer;
}

std::string build_sidecar_json(const ImageArgs& args, const OpenedInput& input,
                               const SidecarFields& fields, const StreamStats& stats) {
    std::ostringstream json;
    const double sample_rate = static_cast<double>(input.info.sample_rate);
    const double peak_dbfs = stats.peak_abs > 0.0 ? 20.0 * std::log10(stats.peak_abs)
                                                  : -std::numeric_limits<double>::infinity();

    json << "{\n";
    json << "  \"schema\": " << json_string(kSidecarSchema) << ",\n";
    json << "  \"display_only\": true,\n";
    json << "  \"verb\": " << json_string(fields.verb) << ",\n";
    json << "  \"tool_version\": " << json_string(PULP_SDK_VERSION) << ",\n";
    json << "  \"input\": {\n";
    json << "    \"name\": " << json_string(display_path(args, args.input)) << ",\n";
    json << "    \"sha256\": " << json_string(input.sha256) << ",\n";
    json << "    \"sample_rate\": " << input.info.sample_rate << ",\n";
    json << "    \"channels\": " << input.info.num_channels << ",\n";
    json << "    \"frames\": " << input.info.num_frames << ",\n";
    json << "    \"bits_per_sample\": " << input.info.bits_per_sample << "\n";
    json << "  },\n";
    json << "  \"decoder\": {\n";
    json << "    \"name\": " << json_string(input.decoder_name);
    if (input.decoder_name == "CoreAudio")
        json << ",\n    \"platform\": " << json_string(platform_name());
    json << "\n  },\n";
    json << "  \"time_range\": {\n";
    json << "    \"start_seconds\": " << json_number(input.start_s) << ",\n";
    json << "    \"end_seconds\": " << json_number(input.end_s) << ",\n";
    json << "    \"start_frame\": " << input.start_frame << ",\n";
    json << "    \"end_frame\": " << input.end_frame << ",\n";
    json << "    \"frames\": " << input.range_frames() << ",\n";
    json << "    \"duration_seconds\": " << json_number(input.range_frames() / sample_rate) << "\n";
    json << "  },\n";
    json << "  \"channel\": " << args.channel << ",\n";
    json << "  \"settings\": " << fields.settings_json << ",\n";
    json << "  \"image\": {\n";
    json << "    \"png\": " << json_string(display_path(args, args.png)) << ",\n";
    json << "    \"width\": " << fields.image_width << ",\n";
    json << "    \"height\": " << fields.image_height << "\n";
    json << "  },\n";
    // Sample peak over the rendered range, unclamped: float above full scale
    // is reported here as-is and shown clamped in the image, never normalized.
    json << "  \"true_peak\": {\n";
    json << "    \"method\": \"sample_peak\",\n";
    json << "    \"linear\": " << json_number(stats.peak_abs) << ",\n";
    json << "    \"dbfs\": " << json_number(peak_dbfs) << ",\n";
    json << "    \"clipped\": " << (stats.peak_abs >= 1.0 ? "true" : "false") << "\n";
    json << "  }\n";
    json << "}\n";
    return json.str();
}

int write_outputs(const ImageArgs& args, const pulp::test::audio::RasterImage& image,
                  const std::string& sidecar_json) {
    pulp::render::HeadlessSurface::Rgba rgba;
    rgba.width = image.width;
    rgba.height = image.height;
    rgba.pixels = image.rgba;
    std::string png_error;
    const auto png = pulp::render::HeadlessSurface::encode_png(rgba, &png_error);
    if (png.empty()) {
        std::cerr << "Error: failed to encode PNG";
        if (!png_error.empty())
            std::cerr << ": " << png_error;
        std::cerr << "\n";
        return 1;
    }

    auto write_file = [](const fs::path& path, const char* bytes, std::size_t size, bool binary) {
        std::error_code ec;
        const auto parent = path.parent_path();
        if (!parent.empty())
            fs::create_directories(parent, ec);
        std::ofstream out(path, binary ? std::ios::binary : std::ios::out);
        if (!out.is_open())
            return false;
        out.write(bytes, static_cast<std::streamsize>(size));
        return out.good();
    };

    if (!write_file(args.png, reinterpret_cast<const char*>(png.data()), png.size(), true)) {
        std::cerr << "Error: failed to write PNG " << args.png << "\n";
        return 1;
    }
    const std::string sidecar = sidecar_path(args);
    if (!write_file(sidecar, sidecar_json.data(), sidecar_json.size(), false)) {
        // A PNG without its sidecar is an image nobody can account for; take
        // it back so the failure leaves no half-written pair behind.
        std::error_code ec;
        fs::remove(args.png, ec);
        std::cerr << "Error: failed to write sidecar " << sidecar << "\n";
        return 1;
    }
    return 0;
}

} // namespace pulp::cli::audio_image
