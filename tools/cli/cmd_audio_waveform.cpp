// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include "cmd_audio_waveform.hpp"

#include "cmd_audio_image_common.hpp"
#include "json_writer.hpp"

#include <pulp/audio/analysis/waveform_raster.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/waveform_overview.hpp>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>

extern bool parse_double_arg(const std::string& text, const char* flag, double& out);

namespace {

namespace img = pulp::cli::audio_image;
namespace ana = pulp::test::audio;

constexpr std::uint32_t kDefaultPlotWidth = 1024;
constexpr std::uint32_t kDefaultPlotHeight = 192;
constexpr std::uint64_t kStreamWindowFrames = 1u << 20;

void print_usage() {
    std::cout << "Usage: pulp audio waveform <file> --png <out.png> [options]\n"
                 "\n"
                 "Render a display-only peak waveform PNG with a pulp.audio.image.v1 sidecar.\n"
                 "Peaks come from WaveformOverview; columns at or beyond full scale carry a\n"
                 "red clip marker and the sidecar reports the unclamped sample peak.\n"
                 "\n"
                 "Options:\n"
                 "  --select <start>,<end>   Highlight a selection, in seconds within the range\n"
              << img::common_flags_help()
              << "\n"
                 "Exit codes: 0 rendered; 1 usage or I/O error; 2 nothing to render.\n";
}

std::string format_seconds(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

bool parse_selection(const std::string& text, double& start_s, double& end_s) {
    const auto comma = text.find(',');
    if (comma == std::string::npos) {
        std::cerr << "Error: --select expects <start>,<end> in seconds\n";
        return false;
    }
    if (!parse_double_arg(text.substr(0, comma), "--select", start_s) ||
        !parse_double_arg(text.substr(comma + 1), "--select", end_s))
        return false;
    if (start_s < 0.0 || end_s <= start_s) {
        std::cerr << "Error: --select end must be greater than its start, and both non-negative\n";
        return false;
    }
    return true;
}

} // namespace

int cmd_audio_waveform(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return args.empty() ? 1 : 0;
    }

    img::ImageArgs common;
    std::optional<std::pair<double, double>> selection_s;

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

        if (arg == "--select") {
            if (i + 1 >= args.size()) {
                std::cerr << "Error: --select requires a value\n";
                return 1;
            }
            double start_s = 0.0;
            double end_s = 0.0;
            if (!parse_selection(args[++i], start_s, end_s))
                return 1;
            selection_s = std::make_pair(start_s, end_s);
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

    img::OpenedInput input;
    if (const int rc = img::open_input(common, input); rc != 0)
        return rc;

    const std::uint32_t plot_width = common.width ? common.width : kDefaultPlotWidth;
    const std::uint32_t plot_height = common.height ? common.height : kDefaultPlotHeight;
    const std::uint32_t sample_rate = input.info.sample_rate;

    // Each streamed window becomes its own level-0 overview at a shared base
    // resolution; the chunks concatenate into one level because every window
    // but the last is a whole multiple of samples_per_peak long.
    const std::uint32_t spp = ana::choose_samples_per_peak(input.range_frames(), plot_width);
    const std::uint64_t window_frames = (kStreamWindowFrames / spp) * spp;
    std::vector<pulp::audio::OverviewLevel> chunks;
    img::StreamStats stats;
    const bool streamed = img::stream_channel(
        input, common.channel, std::max<std::uint64_t>(window_frames, spp),
        [&](std::span<const float> window) {
            const float* channel_ptr = window.data();
            pulp::audio::BufferView<const float> view(&channel_ptr, 1, window.size());
            auto overview =
                pulp::audio::WaveformOverview::build_from_buffer_view(view, sample_rate, spp);
            if (!overview.empty())
                chunks.push_back(overview.level(0));
        },
        stats);
    if (!streamed)
        return 1;

    const pulp::audio::OverviewLevel level = ana::concatenate_levels(chunks);
    if (level.peaks_per_channel == 0) {
        std::cerr << "Error: no peaks could be built from the range\n";
        return 2;
    }
    ana::WaveformColumns columns = ana::columns_from_level(level, 0, plot_width);

    ana::WaveformImageOptions image_options;
    image_options.plot_height = plot_height;
    if (selection_s) {
        if (selection_s->first < input.start_s || selection_s->second > input.end_s) {
            std::cerr << "Error: --select " << format_seconds(selection_s->first) << ","
                      << format_seconds(selection_s->second) << " lies outside the rendered range "
                      << format_seconds(input.start_s) << ".." << format_seconds(input.end_s)
                      << " s\n";
            return 1;
        }
        const double span = input.end_s - input.start_s;
        const double a = (selection_s->first - input.start_s) / span;
        const double b = (selection_s->second - input.start_s) / span;
        image_options.selection_unit = std::make_pair(a, b);
    }

    std::ostringstream summary;
    summary << "spp " << spp << " ch" << common.channel;
    if (stats.peak_abs >= 1.0)
        summary << " CLIP";
    ana::ImageChrome chrome;
    chrome.footer = img::footer_text(input, summary.str());
    chrome.start_s = input.start_s;
    chrome.end_s = input.end_s;

    const ana::RasterImage image = ana::render_waveform_image(columns, image_options, chrome);

    std::ostringstream settings_json;
    settings_json << "{\"samples_per_peak\": " << spp << ", \"peaks\": " << level.peaks_per_channel
                  << ", \"plot_width\": " << plot_width << ", \"plot_height\": " << plot_height
                  << ", \"clip_marker\": \"abs_sample_at_or_above_1\"";
    if (selection_s)
        settings_json << ", \"selection_seconds\": [" << format_seconds(selection_s->first) << ", "
                      << format_seconds(selection_s->second) << "]";
    settings_json << "}";

    img::SidecarFields fields;
    fields.verb = "waveform";
    fields.settings_json = settings_json.str();
    fields.image_width = image.width;
    fields.image_height = image.height;
    return img::write_outputs(common, image, img::build_sidecar_json(common, input, fields, stats));
}
