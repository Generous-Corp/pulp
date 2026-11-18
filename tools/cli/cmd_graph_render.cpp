// `pulp graph render` and `pulp graph report`: the two offline execution
// commands that prepare a graph and drive it without a device.

#include "cmd_graph_internal.hpp"

#include <algorithm>
#include <charconv>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <iostream>
#include <optional>
#include <pulp/audio/audio_file.hpp>
#include <pulp/host/graph_rt_report.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <string>
#include <vector>

namespace pulp_cli::graph_detail {
namespace {

bool parse_report_int(const std::string& text, int min, int max, int& out) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < min ||
        value > max)
        return false;
    out = value;
    return true;
}

// Render admission: the input decodes once, through the same bounded probe
// as `graph probe`, and its length bounds the output allocation as well.
constexpr std::uint64_t kRenderMaxFrames = 28800000;       // 10 minutes at 48 kHz
constexpr std::uint64_t kRenderMaxInputSamples = 57600000; // frames x channels
constexpr std::uint64_t kRenderDefaultFrames = 48000;

} // namespace

// `pulp graph report`: prepare the graph offline and print the static realtime
// report (declared contracts, latency semantics, and a measured pass) as JSON.
int report_graph(const std::vector<std::string>& args) {
    int sample_rate = 48000;
    int max_block = 512;
    int block = 0;
    int measure_blocks = 16;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const auto& key = args[i];
        if (key == "--json")
            continue;
        int* target = key == "--sample-rate"      ? &sample_rate
                      : key == "--max-block-size" ? &max_block
                      : key == "--block-size"     ? &block
                      : key == "--measure-blocks" ? &measure_blocks
                                                  : nullptr;
        if (target == nullptr) {
            json_error("invalid_option", "unknown report option: " + key);
            return 2;
        }
        const int min = target == &measure_blocks ? 0 : 1;
        const int max = target == &sample_rate ? 768000 : target == &measure_blocks ? 4096 : 16384;
        if (++i == args.size() || !parse_report_int(args[i], min, max, *target)) {
            json_error("invalid_option", key + " requires an integer from " + std::to_string(min) +
                                             " to " + std::to_string(max));
            return 2;
        }
    }
    if (block > max_block) {
        json_error("invalid_option", "--block-size must not exceed --max-block-size");
        return 2;
    }
    pulp::host::SignalGraph graph;
    pulp::host::GraphLoadOptions load_options;
    load_options.load_plugins = false;
    const auto loaded = pulp::host::GraphSerializer::from_file(graph, args[1], load_options);
    if (!loaded.ok) {
        json_error(patch_code(file_load_code(loaded)), loaded.error);
        return 1;
    }
    if (!graph.prepare(static_cast<double>(sample_rate), max_block)) {
        json_error("prepare_failed", "graph preparation failed");
        return 1;
    }
    pulp::host::GraphRtReportOptions report_options;
    report_options.block_size = block;
    auto report = pulp::host::build_graph_rt_report(graph, report_options);
    if (measure_blocks > 0) {
        pulp::host::GraphRtMeasureOptions measure;
        measure.blocks = measure_blocks;
        measure.block_size = report.block_size;
        if (const auto measured = pulp::host::measure_graph_rt_work(graph, measure))
            pulp::host::apply_graph_rt_measurement(report, *measured, report_options);
    }
    std::cout << pulp::host::graph_rt_report_to_json(report) << '\n';
    return 0;
}

std::optional<RenderOptions> parse_render_options(const std::vector<std::string>& args,
                                                  std::string& code, std::string& message) {
    RenderOptions options;
    bool seen_input = false;
    bool seen_output = false;
    bool seen_frames = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const auto& key = args[i];
        if (key == "--json")
            continue;
        bool* seen = key == "--input"    ? &seen_input
                     : key == "--output" ? &seen_output
                     : key == "--frames" ? &seen_frames
                                         : nullptr;
        if (seen == nullptr) {
            code = "invalid_option";
            message = "unknown render option: " + key +
                      " (render accepts --input WAV, --output WAV, --frames N and --json)";
            return std::nullopt;
        }
        if (*seen) {
            code = "invalid_option";
            message = key + " may be given only once";
            return std::nullopt;
        }
        *seen = true;
        if (++i == args.size() || args[i].empty()) {
            code = key == "--output" ? "missing_output" : "invalid_option";
            message = key == "--output"  ? "--output requires a WAV path"
                      : key == "--input" ? "--input requires a WAV path"
                                         : "--frames requires an integer";
            return std::nullopt;
        }
        if (key == "--input") {
            options.input = args[i];
        } else if (key == "--output") {
            options.output = args[i];
        } else {
            const auto& value = args[i];
            std::uint64_t frames = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frames);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                frames < 1 || frames > kRenderMaxFrames) {
                code = "invalid_option";
                message =
                    "--frames requires an integer from 1 to " + std::to_string(kRenderMaxFrames);
                return std::nullopt;
            }
            options.frames = frames;
        }
    }
    return options;
}

// `pulp graph render`: drive the graph offline with the decoded --input (or
// with silence when the graph has no audio_in node) and report what came out.
int render_graph(pulp::host::SignalGraph& graph,
                 const pulp::host::GraphSerializer::LoadResult& loaded,
                 const RenderOptions& options, bool json) {
    const auto fail = [json](const std::string& code, const std::string& message, int status) {
        if (json)
            json_error(code, message);
        else
            std::cerr << "pulp graph: " << message << '\n';
        return status;
    };
    if (!loaded.missing_plugins.empty() || !loaded.missing_custom_node_types.empty())
        return fail("unresolved_node", "cannot render with unresolved node types", 1);
    const bool has_audio_in =
        std::any_of(graph.nodes().begin(), graph.nodes().end(),
                    [](const auto& node) { return node.type == pulp::host::NodeType::AudioInput; });
    // Without this refusal an audio_in graph renders the host's zero padding
    // and reports success, which reads as a working graph that outputs silence.
    if (has_audio_in && options.input.empty())
        return fail("missing_input",
                    "the graph reads audio from an audio_in node, so render needs a source: pass "
                    "--input file.wav (its sample rate, channel count and length drive the render)",
                    2);
    std::optional<pulp::audio::AudioFileData> input;
    if (!options.input.empty()) {
        input = pulp::audio::read_audio_file(options.input, [](const auto& info) {
            return info.sample_rate >= 8000 && info.sample_rate <= 192000 &&
                   info.num_channels >= 1 && info.num_channels <= 8 && info.num_frames >= 1 &&
                   info.num_frames <= kRenderMaxFrames &&
                   info.num_frames * info.num_channels <= kRenderMaxInputSamples;
        });
        if (!input || input->empty())
            return fail("input_limit",
                        "input must decode to 1..8 channels, 8000..192000 Hz, 1.." +
                            std::to_string(kRenderMaxFrames) + " frames and at most " +
                            std::to_string(kRenderMaxInputSamples) + " samples in total",
                        2);
        for (const auto& channel : input->channels)
            for (const auto sample : channel)
                if (!std::isfinite(sample))
                    return fail("non_finite_input", "input samples must be finite", 2);
    }
    pulp::host::OfflineSignalGraphConfig config;
    if (input) {
        config.sample_rate = static_cast<double>(input->sample_rate);
        config.input_channels = static_cast<int>(input->num_channels());
        config.output_channels = std::max(2, config.input_channels);
    }
    pulp::host::OfflineSignalGraphHost host(graph);
    if (!host.prepare(config))
        return fail("prepare_failed", "graph preparation failed", 1);
    const std::uint64_t frames = options.frames != 0 ? options.frames
                                 : input             ? input->num_frames()
                                                     : kRenderDefaultFrames;
    std::vector<const float*> pointers;
    pulp::host::OfflineSignalGraphOptions render_options;
    render_options.frame_count = frames;
    if (input) {
        for (const auto& channel : input->channels)
            pointers.push_back(channel.data());
        render_options.input = {pointers.data(), pointers.size(),
                                static_cast<std::size_t>(input->num_frames())};
    }
    const auto rendered = host.render(render_options);
    if (!rendered.ok)
        return fail("render_failed", "offline render failed", 1);
    float peak = 0.0f;
    for (std::size_t channel = 0; channel < rendered.audio.num_channels(); ++channel)
        for (const auto sample : rendered.audio.channel(channel)) {
            if (!std::isfinite(sample))
                return fail("non_finite_output", "the graph produced non-finite samples", 1);
            peak = std::max(peak, std::abs(sample));
        }
    const auto sample_rate = static_cast<std::int64_t>(config.sample_rate);
    if (!options.output.empty()) {
        pulp::audio::AudioFileData file;
        file.sample_rate = static_cast<std::uint32_t>(sample_rate);
        for (std::size_t channel = 0; channel < rendered.audio.num_channels(); ++channel) {
            const auto samples = rendered.audio.channel(channel);
            file.channels.emplace_back(samples.begin(), samples.end());
        }
        if (!pulp::audio::write_wav_file(options.output, file, pulp::audio::WavBitDepth::Float32))
            return fail("output_failed", "could not write WAV output", 1);
    }
    if (json) {
        auto result = choc::value::createObject("GraphRender");
        result.addMember("valid", true);
        result.addMember("code", patch_code(Code::Ok));
        result.addMember("frames", static_cast<std::int64_t>(frames));
        result.addMember("sample_rate", sample_rate);
        result.addMember("channels", static_cast<std::int64_t>(rendered.audio.num_channels()));
        result.addMember("blocks", static_cast<std::int64_t>(rendered.blocks_rendered));
        result.addMember("peak", static_cast<double>(peak));
        std::cout << choc::json::toString(result) << '\n';
    } else {
        std::cout << "rendered " << frames << " frames at " << sample_rate << " Hz in "
                  << rendered.blocks_rendered << " blocks (peak " << peak << ")\n";
    }
    return 0;
}

} // namespace pulp_cli::graph_detail
