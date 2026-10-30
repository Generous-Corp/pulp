#include <pulp/audio/buffer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/runtime/crypto.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

using pulp::audio::Buffer;
using pulp::host::OfflineSignalGraphConfig;
using pulp::host::OfflineSignalGraphHost;
using pulp::host::OfflineSignalGraphOptions;
using pulp::host::PluginInfo;
using pulp::host::PluginScanner;
using pulp::host::PluginSlot;
using pulp::host::SignalGraph;

constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 4096;

std::filesystem::path main_executable(const std::filesystem::path& bundle) {
    std::vector<std::filesystem::path> candidates;
    const auto root = bundle / "Contents" / "MacOS";
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.is_regular_file() && entry.path().extension() != ".json") candidates.push_back(entry.path());
    REQUIRE(candidates.size() == 1);
    return candidates.front();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    return pulp::runtime::sha256_hex(bytes.data(), bytes.size());
}

struct Parts {
    SignalGraph graph;
    pulp::host::NodeId input{};
    pulp::host::NodeId plugin{};
    pulp::host::NodeId output{};
};

std::unique_ptr<Parts> make_graph(const PluginInfo& info) {
    auto parts = std::make_unique<Parts>();
    parts->input = parts->graph.add_input_node(info.num_inputs);
    auto slot = PluginSlot::load(info);
    REQUIRE(slot);
    parts->plugin = parts->graph.add_plugin_node(std::move(slot), info.num_inputs,
                                                 info.num_outputs, "external parity");
    parts->output = parts->graph.add_output_node(info.num_outputs);
    for (int channel = 0; channel < info.num_inputs; ++channel)
        REQUIRE(parts->graph.connect(parts->input, channel, parts->plugin, channel));
    for (int channel = 0; channel < info.num_outputs; ++channel)
        REQUIRE(parts->graph.connect(parts->plugin, channel, parts->output, channel));
    REQUIRE(parts->graph.prepare(kSampleRate, 256));
    return parts;
}

Buffer<float> input_buffer() {
    Buffer<float> input(2, kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        const float sample = 0.2f * std::sin(2.0 * 3.141592653589793 * 220.0 * frame / kSampleRate);
        input.channel(0)[frame] = sample;
        input.channel(1)[frame] = -sample;
    }
    return input;
}

Buffer<float> render_online(Parts& parts, const Buffer<float>& input, int block) {
    Buffer<float> output(2, kFrames);
    std::array<const float*, 2> input_channels{input.channel(0).data(), input.channel(1).data()};
    std::array<float*, 2> output_channels{output.channel(0).data(), output.channel(1).data()};
    pulp::audio::BufferView<const float> input_view(input_channels.data(), 2, block);
    pulp::audio::BufferView<float> output_view(output_channels.data(), 2, block);
    for (int offset = 0; offset < kFrames; offset += block) {
        input_view = pulp::audio::BufferView<const float>(input_channels.data(), 2, block);
        output_view = pulp::audio::BufferView<float>(output_channels.data(), 2, block);
        for (int channel = 0; channel < 2; ++channel) {
            input_channels[channel] = input.channel(channel).data() + offset;
            output_channels[channel] = output.channel(channel).data() + offset;
        }
        parts.graph.process(output_view, input_view, block);
    }
    return output;
}

Buffer<float> render_offline(const PluginInfo& info, const Buffer<float>& input, int block) {
    auto parts = make_graph(info);
    OfflineSignalGraphHost host(parts->graph);
    OfflineSignalGraphConfig config;
    config.sample_rate = kSampleRate;
    config.block_frames = block;
    config.input_channels = 2;
    config.output_channels = 2;
    REQUIRE(host.prepare(config));
    OfflineSignalGraphOptions options;
    options.frame_count = kFrames;
    options.input = input.view();
    auto result = host.render(options);
    REQUIRE(result.ok);
    return std::move(result.audio);
}

void require_equal(const Buffer<float>& lhs, const Buffer<float>& rhs, float tolerance) {
    REQUIRE(lhs.num_channels() == rhs.num_channels());
    REQUIRE(lhs.num_samples() == rhs.num_samples());
    for (std::size_t channel = 0; channel < lhs.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < lhs.num_samples(); ++frame)
            REQUIRE(std::fabs(lhs.channel(channel)[frame] - rhs.channel(channel)[frame]) <= tolerance);
}

} // namespace

TEST_CASE("external CLAP graph matches live and offline rendering", "[host][graph][external][offline]") {
    const char* path = std::getenv("PULP_EXTERNAL_CLAP_PATH");
    const char* expected_sha = std::getenv("PULP_EXTERNAL_CLAP_SHA256");
    if (!path || !expected_sha)
        SKIP("external CLAP artifact identity is not configured");
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(sha256_file(main_executable(path)) == expected_sha);
    const auto plugins = PluginScanner::scan_bundle(path, pulp::host::PluginFormat::CLAP);
    REQUIRE(plugins.size() == 1);
    const auto& info = plugins.front();
    REQUIRE(info.num_inputs == 2);
    REQUIRE(info.num_outputs == 2);

    const auto input = input_buffer();
    auto online_parts = make_graph(info);
    const auto online = render_online(*online_parts, input, 128);
    const auto offline_same = render_offline(info, input, 128);
    require_equal(online, offline_same, 0.0f);

    const auto offline_coarse = render_offline(info, input, 256);
    require_equal(online, offline_coarse, 1.0e-6f);
}
