// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include "cmd_audio_spectrogram.hpp"

#include "cli_common.hpp"
#include "cmd_audio_image_common.hpp"

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/analysis/spectrogram_raster.hpp>

#include <iostream>
#include <sstream>

namespace {

namespace img = pulp::cli::audio_image;
namespace ana = pulp::test::audio;

constexpr std::uint32_t kDefaultPlotWidth = 1024;
constexpr std::uint32_t kDefaultPlotHeight = 384;

void print_usage() {
    std::cout << "Usage: pulp audio spectrogram <file> --png <out.png> [options]\n"
                 "\n"
                 "Render a display-only spectrogram PNG with a pulp.audio.image.v1 sidecar.\n"
                 "Long files stream through memory-mapped windows; nothing is normalized.\n"
                 "\n"
                 "Options:\n"
                 "  --fft <n>                FFT length, power of two 16..65536 (default: 2048)\n"
                 "  --hop <n>                Hop in samples, 1..fft (default: 512)\n"
                 "  --window <name>          rectangular|hann|hamming|blackman|flat_top|kaiser\n"
                 "                           (default: hann)\n"
                 "  --scale <axis>           linear|log|mel (default: linear; mel is HTK)\n"
                 "  --db-range <db>          Dynamic range below 0 dBFS (default: 90)\n"
              << img::common_flags_help()
              << "\n"
                 "Exit codes: 0 rendered; 1 usage or I/O error; 2 nothing to render\n"
                 "(empty input, or a range shorter than one FFT).\n";
}

std::optional<ana::Window> parse_window(const std::string& text) {
    for (auto w : {ana::Window::rectangular, ana::Window::hann, ana::Window::hamming,
                   ana::Window::blackman, ana::Window::flat_top, ana::Window::kaiser}) {
        if (ana::window_name(w) == text)
            return w;
    }
    return std::nullopt;
}

} // namespace

int cmd_audio_spectrogram(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return args.empty() ? 1 : 0;
    }

    img::ImageArgs common;
    common.width = kDefaultPlotWidth;
    common.height = kDefaultPlotHeight;
    ana::SpectrogramSettings settings;
    ana::SpectrogramImageOptions display;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        const auto consumed = img::parse_common_arg(args, i, common);
        if (consumed == img::CommonArg::error)
            return 1;
        if (consumed == img::CommonArg::consumed)
            continue;

        auto take_value = [&](const char* flag, std::string& value) {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: " << flag << " requires a value\n";
                return false;
            }
            value = args[++i];
            return true;
        };
        std::string value;
        if (arg == "--fft" || arg == "--hop") {
            if (!take_value(arg.c_str(), value))
                return 1;
            std::size_t parsed = 0;
            if (!parse_size_arg(value, arg.c_str(), parsed))
                return 1;
            if (parsed == 0 || parsed > static_cast<std::size_t>(ana::kSpectrogramMaxFftLength)) {
                std::cerr << "Error: " << arg << " must be between 1 and "
                          << ana::kSpectrogramMaxFftLength << "\n";
                return 1;
            }
            (arg == "--fft" ? settings.fft_length : settings.hop) = static_cast<int>(parsed);
        } else if (arg == "--window") {
            if (!take_value("--window", value))
                return 1;
            auto window = parse_window(value);
            if (!window) {
                std::cerr << "Error: --window must be one of rectangular, hann, hamming, "
                             "blackman, flat_top, kaiser\n";
                return 1;
            }
            settings.window = *window;
        } else if (arg == "--scale") {
            if (!take_value("--scale", value))
                return 1;
            auto scale = ana::parse_frequency_scale(value);
            if (!scale) {
                std::cerr << "Error: --scale must be one of linear, log, mel\n";
                return 1;
            }
            display.scale = *scale;
        } else if (arg == "--db-range") {
            if (!take_value("--db-range", value))
                return 1;
            if (!parse_double_arg(value, "--db-range", display.db_range))
                return 1;
            if (display.db_range <= 0.0 || display.db_range > 300.0) {
                std::cerr << "Error: --db-range must be between 0 and 300\n";
                return 1;
            }
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "Error: unknown option " << arg << "\n";
            return 1;
        } else if (common.input.empty()) {
            common.input = arg;
        } else {
            std::cerr << "Error: only one input file may be given\n";
            return 1;
        }
    }

    display.plot_height = common.height;
    if (const std::string reason = ana::validate_spectrogram_settings(settings); !reason.empty()) {
        std::cerr << "Error: " << reason << "\n";
        return 1;
    }
    if (const std::string reason = ana::validate_spectrogram_image_options(display);
        !reason.empty()) {
        std::cerr << "Error: " << reason << "\n";
        return 1;
    }

    // The grid is allocated before the first frame is read; refuse a size that
    // would be gigabytes rather than let bad_alloc answer for the flags.
    const std::uint64_t grid_bytes = ana::spectrogram_grid_bytes(common.width, settings.fft_length);
    if (grid_bytes > ana::kSpectrogramGridBudgetBytes) {
        std::cerr << "Error: --width " << common.width << " x " << (settings.fft_length / 2 + 1)
                  << " bins needs " << (grid_bytes >> 20)
                  << " MiB for the spectrogram grid, over the "
                  << (ana::kSpectrogramGridBudgetBytes >> 20)
                  << " MiB budget; use a smaller --fft or --width\n";
        return 1;
    }

    img::OpenedInput input;
    if (const int rc = img::open_input(common, input); rc != 0)
        return rc;

    const std::uint64_t frames =
        ana::spectrogram_frame_count(input.range_frames(), settings.fft_length, settings.hop);
    if (frames == 0) {
        std::cerr << "Error: the range holds " << input.range_frames()
                  << " frames, shorter than --fft " << settings.fft_length
                  << "; use a smaller --fft or a longer range\n";
        return 2;
    }

    ana::SpectrogramBuilder builder(settings, static_cast<double>(input.info.sample_rate),
                                    input.range_frames(), common.width);
    img::StreamStats stats;
    if (!img::stream_channel(
            input, common.channel, img::kStreamWindowFrames,
            [&](std::span<const float> window) { builder.push(window); }, stats))
        return 1;
    const ana::SpectrogramGrid grid = builder.finish();

    std::ostringstream summary;
    summary << "fft " << settings.fft_length << " hop " << settings.hop << " "
            << ana::window_name(settings.window) << " " << ana::frequency_scale_name(display.scale)
            << " " << display.db_range << "dB ch" << common.channel;
    ana::ImageChrome chrome;
    chrome.footer = img::footer_text(input, summary.str());
    chrome.start_s = input.start_s;
    chrome.end_s = input.end_s;

    const ana::RasterImage image = ana::render_spectrogram_image(grid, display, chrome);

    using choc::value::createFloat64;
    using choc::value::createInt64;
    using choc::value::createString;
    auto settings_json = choc::value::createObject("");
    settings_json.addMember("fft", createInt64(settings.fft_length));
    settings_json.addMember("hop", createInt64(settings.hop));
    settings_json.addMember("window", createString(ana::window_name(settings.window)));
    settings_json.addMember("scale",
                            createString(std::string(ana::frequency_scale_name(display.scale))));
    settings_json.addMember("db_range", createFloat64(display.db_range));
    settings_json.addMember("db_ceiling", createFloat64(display.db_ceiling));
    settings_json.addMember("mel_convention", createString("htk"));
    settings_json.addMember("palette", createString("inferno"));
    settings_json.addMember("column_reduction", createString("peak_hold"));
    settings_json.addMember("stft_frames",
                            createInt64(static_cast<std::int64_t>(grid.frames_analyzed)));
    settings_json.addMember("bins", createInt64(grid.bins));
    settings_json.addMember("plot_width", createInt64(common.width));
    settings_json.addMember("plot_height", createInt64(common.height));

    img::SidecarFields fields;
    fields.verb = "spectrogram";
    fields.settings = settings_json;
    fields.image_width = image.width;
    fields.image_height = image.height;
    return img::write_outputs(common, image, img::build_sidecar(common, input, fields, stats));
}
