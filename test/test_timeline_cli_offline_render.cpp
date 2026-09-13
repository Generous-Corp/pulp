#include "../tools/timeline/src/timeline_agent_internal.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/host/timeline_offline_graph.hpp>
#include <pulp/host/timeline_offline_renderer.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

using namespace pulp;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

class TempDirectory {
  public:
    TempDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("pulp-timeline-cli-render-" + std::to_string(nonce));
        REQUIRE(std::filesystem::create_directories(path_));
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

constexpr std::uint32_t kSampleRate = 48'000;
constexpr std::uint64_t kFrameCount = 640;

ContentHash file_hash(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    const std::string bytes{std::istreambuf_iterator<char>(stream),
                            std::istreambuf_iterator<char>()};
    auto hash = ContentHash::from_hex(runtime::sha256_hex(bytes));
    REQUIRE(hash);
    return *hash;
}

std::filesystem::path write_source_wav(const std::filesystem::path& directory) {
    audio::AudioFileData source;
    source.sample_rate = kSampleRate;
    source.channels.assign(2, std::vector<float>(static_cast<std::size_t>(kFrameCount), 0.0f));
    // A moving signal, so a render that drops or shifts a block differs from one
    // that does not. Constant channels would compare equal under either.
    for (std::size_t frame = 0; frame < source.channels[0].size(); ++frame) {
        source.channels[0][frame] = static_cast<float>(frame % 97) / 97.0f - 0.5f;
        source.channels[1][frame] = 0.25f - static_cast<float>(frame % 53) / 53.0f;
    }
    const auto path = directory / "source.wav";
    REQUIRE(audio::write_wav_file(path.string(), source, audio::WavBitDepth::Float32));
    return path;
}

std::string project_json(const std::filesystem::path& source) {
    auto clip = take(Clip::create_absolute({4}, {0}, kFrameCount, {kSampleRate, 1},
                                           MediaRef{{5}, {0}, kFrameCount}, {.gain_linear = 1.0f}));
    auto track = take(Track::create({3}, "audio", {clip}));
    auto sequence =
        take(Sequence::create({2}, "root", std::nullopt,
                              AbsoluteTimelineDuration{kFrameCount, {kSampleRate, 1}}, {track}));
    MediaAsset asset{{5},
                     "source.wav",
                     kFrameCount,
                     {kSampleRate, 1},
                     file_hash(source),
                     AssetStoragePolicy::External,
                     {{AssetLocatorKind::ExternalUri,
                       tools::timeline::filesystem_path_to_utf8(source)}},
                     {},
                     {}};
    auto project = take(Project::create(ProjectInput{{1}, "agent", 6, {2}, {asset}, {sequence}}));
    auto registry = take(make_builtin_timeline_registry());
    return take(serialize_project(project, registry)).json;
}

/// The smallest tick whose sample mapping covers `frames`, found by bisection
/// over the tempo map's monotonic tick-to-sample curve. Derived here from the
/// tempo map alone so the expectation does not restate the renderer's own
/// arithmetic.
timebase::TickPosition covering_tick(const timebase::CompiledTempoMap& tempo_map,
                                     std::uint64_t frames) {
    std::int64_t low = 0;
    std::int64_t high = 1;
    const auto target = static_cast<std::int64_t>(frames);
    while (tempo_map.ticks_to_samples({high}).value < target)
        high *= 2;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (tempo_map.ticks_to_samples({middle}).value < target)
            low = middle + 1;
        else
            high = middle;
    }
    return timebase::TickPosition{low};
}

} // namespace

TEST_CASE("timeline render verb produces the offline renderer's audio bit for bit",
          "[timeline][render]") {
    TempDirectory temp;
    const auto source_path = write_source_wav(temp.path());
    const auto json = project_json(source_path);
    const auto output_path = temp.path() / "rendered.wav";

    REQUIRE(tools::timeline::render(json, output_path.string()));
    const auto through_cli = audio::read_audio_file(output_path.string());
    REQUIRE(through_cli);

    auto registry = take(make_builtin_timeline_registry());
    auto loaded = take(
        tools::timeline::detail::load_project(tools::timeline::ProjectSource::inline_json(json), registry));
    auto compiled = take(tools::timeline::detail::compile_project(loaded, kSampleRate));
    auto program = compiled->store.read();
    REQUIRE(program);

    host::SignalGraph graph;
    host::TimelineGraphPlaybackBinding binding(graph, compiled->store);
    const auto topology = host::build_device_free_timeline_graph(graph, *program, 2);
    REQUIRE(topology);
    REQUIRE_FALSE(topology.routes.empty());

    host::TimelineOfflineRenderConfig config;
    config.sample_rate = static_cast<double>(kSampleRate);
    config.block_frames = 512;
    config.output_channels = 2;
    config.max_output_frames = tools::timeline::detail::kMaxRenderPcmBytes / (2 * sizeof(float));
    config.maximum_note_events_per_track_per_block =
        host::TimelineGraphBindingConfig{}.maximum_note_events_per_track_per_block;

    host::TimelineOfflineRenderOptions options;
    options.end_tick = covering_tick(*compiled->tempo_map, kFrameCount);
    options.tail_frames = 0;

    const auto through_library =
        host::render_timeline_offline(graph, binding, *program, topology.routes, config, options);
    REQUIRE(through_library);

    REQUIRE(through_cli->channels.size() == through_library.audio.channels.size());
    for (std::size_t channel = 0; channel < through_library.audio.channels.size(); ++channel) {
        const auto& expected = through_library.audio.channels[channel];
        const auto& actual = through_cli->channels[channel];
        REQUIRE(actual.size() == expected.size());
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
            REQUIRE(actual[frame] == expected[frame]);
    }
}

TEST_CASE("every offline render outcome carries its own render failure message",
          "[timeline][render]") {
    constexpr host::TimelineOfflineRenderCode codes[] = {
        host::TimelineOfflineRenderCode::Ok,
        host::TimelineOfflineRenderCode::InvalidRange,
        host::TimelineOfflineRenderCode::InvalidLimits,
        host::TimelineOfflineRenderCode::SampleRateMismatch,
        host::TimelineOfflineRenderCode::InvalidProgram,
        host::TimelineOfflineRenderCode::BindingRejected,
        host::TimelineOfflineRenderCode::TransportRejected,
        host::TimelineOfflineRenderCode::ProcessFailed,
    };

    std::set<std::string> messages;
    for (const auto code : codes) {
        host::TimelineOfflineRenderResult result;
        result.code = code;
        const auto message = tools::timeline::detail::offline_render_message(result);
        REQUIRE_FALSE(message.empty());
        REQUIRE(messages.insert(message).second);
    }
    REQUIRE(messages.size() == std::size(codes));
}

TEST_CASE("offline render failure messages keep the rejecting subsystem's own code",
          "[timeline][render]") {
    host::TimelineOfflineRenderResult admission;
    admission.code = host::TimelineOfflineRenderCode::BindingRejected;
    admission.admission.code = host::TimelineGraphAdmissionCode::SampleRateMismatch;
    REQUIRE(tools::timeline::detail::offline_render_message(admission).find(std::to_string(
                static_cast<unsigned>(host::TimelineGraphAdmissionCode::SampleRateMismatch))) !=
            std::string::npos);

    host::TimelineOfflineRenderResult transport;
    transport.code = host::TimelineOfflineRenderCode::TransportRejected;
    transport.transport_error = playback::TransportError::NotPrepared;
    REQUIRE(tools::timeline::detail::offline_render_message(transport).find(std::to_string(
                static_cast<unsigned>(playback::TransportError::NotPrepared))) !=
            std::string::npos);
}
