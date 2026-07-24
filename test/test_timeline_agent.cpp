#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using Catch::Matchers::WithinAbs;
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
                ("pulp-timeline-agent-" + std::to_string(nonce));
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

ContentHash file_hash(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    const std::string bytes{std::istreambuf_iterator<char>(stream),
                            std::istreambuf_iterator<char>()};
    auto hash = ContentHash::from_hex(runtime::sha256_hex(bytes));
    REQUIRE(hash);
    return *hash;
}

std::string project_json(const std::filesystem::path& source, std::uint64_t frame_count = 32,
                         ContentHash content_hash = {}, bool include_duration = true,
                         std::vector<AssetLocator> locators = {}) {
    auto clip = take(Clip::create_absolute({4}, {0}, frame_count, {48'000, 1},
                                           MediaRef{{5}, {0}, frame_count}, {.gain_linear = 1.0f}));
    auto track = take(Track::create({3}, "audio", {clip}));
    auto sequence = take(Sequence::create(
        {2}, "root", std::nullopt,
        include_duration ? std::optional<AbsoluteTimelineDuration>{AbsoluteTimelineDuration{
                               frame_count, {48'000, 1}}}
                         : std::nullopt,
        {track}));
    if (locators.empty())
        locators.push_back({AssetLocatorKind::ExternalUri, source.string()});
    MediaAsset asset{{5},
                     "source.wav",
                     frame_count,
                     {48'000, 1},
                     content_hash.valid() ? content_hash : file_hash(source),
                     AssetStoragePolicy::External,
                     std::move(locators),
                     {},
                     {}};
    auto project = take(Project::create(ProjectInput{{1}, "agent", 6, {2}, {asset}, {sequence}}));
    auto registry = take(make_builtin_timeline_registry());
    return take(serialize_project(project, registry)).json;
}

std::string empty_project_json(std::uint64_t frame_count = 32) {
    auto sequence = take(Sequence::create({2}, "root", std::nullopt,
                                          AbsoluteTimelineDuration{frame_count, {48'000, 1}}, {}));
    auto project = take(Project::create(ProjectInput{{1}, "empty", 3, {2}, {}, {sequence}}));
    auto registry = take(make_builtin_timeline_registry());
    return take(serialize_project(project, registry)).json;
}

std::string project_to_json(const Project& project) {
    auto registry = take(make_builtin_timeline_registry());
    return take(serialize_project(project, registry)).json;
}

std::string gain_command(std::uint32_t replacement_bits) {
    return R"([{"data":{"clip_id":"4","expected":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":"1065353216"},"replacement":{"fade_in_duration":"0","fade_out_duration":"0","gain_linear_bits":")" +
           std::to_string(replacement_bits) +
           R"("},"sequence_id":"2","track_id":"3"},"type_name":"pulp.timeline.command.set_clip_playback_properties","version":1}])";
}

std::string project_from_result(const std::string& json) {
    auto parsed = take(parse_json(json));
    const auto* project = parsed->root().find("project");
    REQUIRE(project != nullptr);
    return std::string(parsed->raw(*project));
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t offset = 0; (offset = text.find(needle, offset)) != std::string_view::npos;
         offset += needle.size())
        ++count;
    return count;
}

} // namespace

TEST_CASE("timeline agent applies typed commands and renders the resulting project") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto original_project = project_json(source_path);

    const auto opened = tools::timeline::project_open(original_project);
    REQUIRE(opened);
    REQUIRE(project_from_result(opened.json) == original_project);
    REQUIRE(tools::timeline::validate(original_project));

    const auto explained = tools::timeline::explain(original_project);
    REQUIRE(explained);
    REQUIRE(explained.json.find(R"("audio_regions":1)") != std::string::npos);
    REQUIRE(explained.json.find(R"("clip_ids":["4"])") != std::string::npos);
    REQUIRE(explained.json.find(R"("pdc_offset_samples":null)") != std::string::npos);

    const auto changed =
        tools::timeline::command_apply(original_project, gain_command(1'056'964'608));
    REQUIRE(changed);
    REQUIRE(changed.json.find(R"("revision":"1")") != std::string::npos);
    const auto changed_project = project_from_result(changed.json);

    const auto original_path = temp.path() / "original.wav";
    const auto changed_path = temp.path() / "changed.wav";
    REQUIRE(tools::timeline::render(original_project, original_path.string()));
    REQUIRE(tools::timeline::render(changed_project, changed_path.string()));

    const auto original_audio = audio::read_audio_file(original_path.string());
    const auto changed_audio = audio::read_audio_file(changed_path.string());
    REQUIRE(original_audio);
    REQUIRE(changed_audio);
    REQUIRE(original_audio->num_frames() == 32);
    REQUIRE(changed_audio->num_frames() == 32);
    REQUIRE_THAT(original_audio->channels[0][0], WithinAbs(0.75f, 1e-7f));
    REQUIRE_THAT(changed_audio->channels[0][0], WithinAbs(0.375f, 1e-7f));
}

TEST_CASE("timeline agent schema and errors are typed and fail closed") {
    const auto schema = tools::timeline::schema();
    REQUIRE(schema);
    REQUIRE(count_occurrences(schema.json, R"("x-pulp-domain":"Command")") == 19);

    const auto unknown = tools::timeline::command_apply(
        empty_project_json(),
        R"([{"data":{},"type_name":"pulp.timeline.command.unknown","version":1}])");
    REQUIRE_FALSE(unknown);
    REQUIRE(unknown.exit_code == 2);
    REQUIRE(unknown.json.find(R"("stage":"apply")") != std::string::npos);

    const auto invalid_rate = tools::timeline::explain(R"({})", 0);
    REQUIRE_FALSE(invalid_rate);
    REQUIRE(invalid_rate.exit_code == 2);
    REQUIRE(invalid_rate.json.find(R"("stage":"explain")") != std::string::npos);
    REQUIRE(tools::timeline::explain(empty_project_json(), 768'000));
    const auto excessive_rate = tools::timeline::explain(empty_project_json(), 768'001);
    REQUIRE_FALSE(excessive_rate);
    REQUIRE(excessive_rate.exit_code == 2);
    REQUIRE(excessive_rate.json.find("768000") != std::string::npos);

    TempDirectory temp;
    const auto oversized = tools::timeline::render(empty_project_json(200'000'000),
                                                   (temp.path() / "large.wav").string());
    REQUIRE_FALSE(oversized);
    REQUIRE(oversized.json.find("in-memory render budget") != std::string::npos);
}

TEST_CASE("timeline agent refuses media whose bytes do not match its durable identity") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto project = project_json(source_path);

    source.channels[0][0] = 0.25f;
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto result = tools::timeline::render(project, (temp.path() / "output.wav").string());
    REQUIRE_FALSE(result);
    REQUIRE(result.json.find(R"("stage":"render")") != std::string::npos);
}

TEST_CASE("timeline agent rejects path-derived hashes as media identity") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    auto provisional = ContentHash::from_hex(runtime::sha256_hex(source_path.string()));
    REQUIRE(provisional);

    const auto result = tools::timeline::render(project_json(source_path, 32, *provisional),
                                                (temp.path() / "output.wav").string());
    REQUIRE_FALSE(result);
    REQUIRE(result.json.find(R"("stage":"render")") != std::string::npos);
}

TEST_CASE("timeline agent tries later media locators after a stale existing hint") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    source.channels[0][0] = 0.25f;
    const auto stale_path = temp.path() / "stale.wav";
    REQUIRE(audio::write_wav_file(stale_path.string(), source, audio::WavBitDepth::Float32));

    std::vector<AssetLocator> locators{
        {AssetLocatorKind::ExternalUri, stale_path.string()},
        {AssetLocatorKind::ExternalUri, source_path.string()},
    };
    const auto output_path = temp.path() / "output.wav";
    REQUIRE(tools::timeline::render(
        project_json(source_path, 32, file_hash(source_path), true, std::move(locators)),
        output_path.string()));
    const auto rendered = audio::read_audio_file(output_path.string());
    REQUIRE(rendered);
    REQUIRE_THAT(rendered->channels[0][0], WithinAbs(0.75f, 1e-7f));
}

TEST_CASE("timeline agent derives duration-less render length from active content") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto output_path = temp.path() / "output.wav";
    const auto result =
        tools::timeline::render(project_json(source_path, 32, {}, false), output_path.string());
    REQUIRE(result);
    REQUIRE(result.json.find(R"("frames":"32")") != std::string::npos);
    REQUIRE(audio::read_audio_file(output_path.string()));
}

TEST_CASE("timeline agent derives duration-less render length from musical clips") {
    auto notes =
        take(NoteContent::create({{{5}, {0}, {timebase::kTicksPerQuarter}, 0x8000, 60, 0}}));
    auto clip = take(Clip::create({4}, {0}, {2 * timebase::kTicksPerQuarter}, std::move(notes)));
    auto track = take(Track::create({3}, "notes", {std::move(clip)}));
    auto sequence =
        take(Sequence::create({2}, "root", std::nullopt, std::nullopt, {std::move(track)}));
    auto project =
        take(Project::create(ProjectInput{{1}, "musical", 6, {2}, {}, {std::move(sequence)}}));

    TempDirectory temp;
    const auto output_path = temp.path() / "output.wav";
    const auto result =
        tools::timeline::render(project_to_json(project), output_path.string(), 48'000);
    REQUIRE(result);
    REQUIRE(result.json.find(R"("frames":"48000")") != std::string::npos);
    const auto rendered = audio::read_audio_file(output_path.string());
    REQUIRE(rendered);
    REQUIRE(rendered->num_frames() == 48'000);
}

TEST_CASE("timeline agent derives duration-less render length from a selected freeze") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto hash = file_hash(source_path);

    auto hidden_arrangement = take(Clip::create_absolute(
        {4}, {48'000}, 32, {48'000, 1}, MediaRef{{5}, {0}, 32}, {.gain_linear = 1.0f}));
    TrackFreeze freeze{MediaRef{{5}, {0}, 32}, {0}, {48'000, 1}, hash};
    auto track = take(Track::create(TrackInput{
        .id = {3},
        .name = "frozen",
        .clips = {std::move(hidden_arrangement)},
        .freeze = freeze,
    }));
    auto sequence =
        take(Sequence::create({2}, "root", std::nullopt, std::nullopt, {std::move(track)}));
    MediaAsset asset{{5},
                     "source.wav",
                     32,
                     {48'000, 1},
                     hash,
                     AssetStoragePolicy::External,
                     {{AssetLocatorKind::ExternalUri, source_path.string()}},
                     {},
                     {}};
    auto project = take(Project::create(
        ProjectInput{{1}, "freeze", 6, {2}, {std::move(asset)}, {std::move(sequence)}}));

    const auto output_path = temp.path() / "output.wav";
    const auto result =
        tools::timeline::render(project_to_json(project), output_path.string(), 48'000);
    REQUIRE(result);
    REQUIRE(result.json.find(R"("frames":"32")") != std::string::npos);
    const auto rendered = audio::read_audio_file(output_path.string());
    REQUIRE(rendered);
    REQUIRE(rendered->num_frames() == 32);
}
