#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#if defined(__linux__)
#include "linux_posix_acl_test_helpers.hpp"
#endif

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

void write_text_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

void write_wav_file_native(const std::filesystem::path& path,
                           const audio::AudioFileData& audio) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream);
    REQUIRE(audio::write_wav_stream(stream, audio, audio::WavBitDepth::Float32));
    stream.close();
    REQUIRE(stream.good());
}

void require_no_render_temporaries(const std::filesystem::path& directory,
                                   const std::filesystem::path& destination) {
    const auto prefix = destination.filename().string() + ".tmp.";
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        REQUIRE_FALSE(entry.path().filename().string().starts_with(prefix));
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
        locators.push_back(
            {AssetLocatorKind::ExternalUri,
             tools::timeline::filesystem_path_to_utf8(source)});
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

TEST_CASE("timeline agent accepts explicit inline and file project sources") {
    const auto json = empty_project_json();
    const auto inline_source = tools::timeline::ProjectSource::inline_json(json);
    REQUIRE(tools::timeline::validate(inline_source));

    TempDirectory temp;
    const auto project_path = temp.path() / "project.json";
    write_text_file(project_path, json);
    const auto file_source = tools::timeline::ProjectSource::file(project_path);
    REQUIRE(file_source.kind() == tools::timeline::ProjectSourceKind::File);
    REQUIRE(file_source.file_path() == project_path);
    REQUIRE(tools::timeline::validate(file_source));

    const auto unicode_project_path =
        temp.path() /
        tools::timeline::filesystem_path_from_utf8("proyecto-\xE9\x9F\xB3-\xCE\xA9.json");
    write_text_file(unicode_project_path, json);
    REQUIRE(tools::timeline::validate(tools::timeline::ProjectSource::auto_detect(
        tools::timeline::filesystem_path_to_utf8(unicode_project_path))));

#ifdef _WIN32
    const std::filesystem::path native_path{L"C:\\timeline-\u03a9\\project.json"};
#else
    const std::filesystem::path native_path{"/tmp/timeline-\xCE\xA9/project.json"};
#endif
    REQUIRE(tools::timeline::ProjectSource::file(native_path).file_path() == native_path);
    REQUIRE(tools::timeline::filesystem_path_from_utf8(
                tools::timeline::filesystem_path_to_utf8(native_path)) == native_path);

    const auto wrong_kind =
        tools::timeline::ProjectSource::file(std::filesystem::path{"{\"not\":\"a path\"}"});
    const auto rejected = tools::timeline::validate(wrong_kind);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.json.find(R"("stage":"open")") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("timeline agent rejects native paths that cannot be represented in JSON") {
    const std::filesystem::path invalid_native_path{std::string("timeline-\xFF.json")};
    REQUIRE_THROWS_AS(tools::timeline::filesystem_path_to_utf8(invalid_native_path),
                      std::invalid_argument);

    const auto rejected =
        tools::timeline::validate(tools::timeline::ProjectSource::file(invalid_native_path));
    REQUIRE_FALSE(rejected);
    REQUIRE(parse_json(rejected.json));
    REQUIRE(rejected.json.find(R"("path":)") == std::string::npos);
}
#endif

TEST_CASE("timeline agent preserves native non-ASCII render and UTF-8 asset paths") {
    TempDirectory temp;
    const auto unicode_directory =
        temp.path() / tools::timeline::filesystem_path_from_utf8("m\xC3\xBAsica-\xCE\xA9");
    REQUIRE(std::filesystem::create_directories(unicode_directory));
    const auto source_path =
        unicode_directory /
        tools::timeline::filesystem_path_from_utf8("fuente-\xE9\x9F\xB3.wav");
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.5f)};
    write_wav_file_native(source_path, source);

    const auto output_path =
        unicode_directory /
        tools::timeline::filesystem_path_from_utf8("salida-\xE6\xB8\xB2\xE6\x9F\x93.wav");
    const auto rendered = tools::timeline::render(project_json(source_path), output_path);
    REQUIRE(rendered);
    REQUIRE(std::filesystem::is_regular_file(output_path));
    REQUIRE(rendered.json.find(pulp::timeline::quote_json_string(
                tools::timeline::filesystem_path_to_utf8(output_path))) != std::string::npos);
}

TEST_CASE("timeline agent render constructs every output channel") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f), std::vector<float>(32, -0.25f)};
    const auto source_path = temp.path() / "stereo.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));

    const auto output_path = temp.path() / "rendered.wav";
    REQUIRE(tools::timeline::render(project_json(source_path), output_path.string()));
#ifndef _WIN32
    REQUIRE(std::filesystem::status(output_path).permissions() ==
            (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
#endif
    const auto rendered = audio::read_audio_file(output_path.string());
    REQUIRE(rendered);
    REQUIRE(rendered->channels.size() == 2);
    REQUIRE(rendered->num_frames() == 32);
    REQUIRE_THAT(rendered->channels[0][0], WithinAbs(0.75f, 1e-7f));
    REQUIRE_THAT(rendered->channels[1][0], WithinAbs(-0.25f, 1e-7f));
}

TEST_CASE("timeline agent atomically publishes rendered WAV files") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto project = project_json(source_path);

    const auto output_path = temp.path() / "output.wav";
    write_text_file(output_path, "existing destination");
#ifndef _WIN32
    const auto preserved_permissions = std::filesystem::perms::none;
    std::error_code permission_error;
    std::filesystem::permissions(output_path, preserved_permissions,
                                 std::filesystem::perm_options::replace, permission_error);
    REQUIRE_FALSE(permission_error);
#endif

    REQUIRE(tools::timeline::render(project, output_path.string()));
#ifndef _WIN32
    REQUIRE((std::filesystem::status(output_path).permissions() & std::filesystem::perms::all) ==
            preserved_permissions);
    std::filesystem::permissions(
        output_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, permission_error);
    REQUIRE_FALSE(permission_error);
#endif
    const auto rendered = audio::read_audio_file(output_path.string());
    REQUIRE(rendered);
    REQUIRE_THAT(rendered->channels[0][0], WithinAbs(0.75f, 1e-7f));
    require_no_render_temporaries(temp.path(), output_path);

    const auto blocked_path = temp.path() / "blocked.wav";
    REQUIRE(std::filesystem::create_directory(blocked_path));
    write_text_file(blocked_path / "sentinel", "preserve me");
    const auto failed = tools::timeline::render(project, blocked_path.string());
    REQUIRE_FALSE(failed);
    REQUIRE(std::filesystem::is_directory(blocked_path));
    REQUIRE(std::filesystem::is_regular_file(blocked_path / "sentinel"));
    require_no_render_temporaries(temp.path(), blocked_path);
}

#if defined(__linux__)
TEST_CASE("timeline agent atomic render preserves a Linux POSIX ACL") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));

    const auto output_path = temp.path() / "output.wav";
    write_text_file(output_path, "existing destination");
    const auto acl_result = linux_acl_test::install(output_path);
    if (acl_result == linux_acl_test::InstallResult::Unsupported)
        SKIP("temporary filesystem does not support POSIX ACLs");
    REQUIRE(acl_result == linux_acl_test::InstallResult::Installed);
    const auto expected_acl = linux_acl_test::read(output_path);
    REQUIRE(expected_acl);

    REQUIRE(tools::timeline::render(project_json(source_path), output_path.string()));
    REQUIRE(linux_acl_test::read(output_path) == expected_acl);
    require_no_render_temporaries(temp.path(), output_path);
}
#endif

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

TEST_CASE("timeline agent does not load offline media from non-root sequences") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));

    auto clip = take(Clip::create_absolute({4}, {0}, 32, {48'000, 1}, MediaRef{{5}, {0}, 32},
                                           {.gain_linear = 1.0f}));
    auto track = take(Track::create({3}, "audio", {std::move(clip)}));
    auto sequence = take(Sequence::create(
        {2}, "root", std::nullopt, AbsoluteTimelineDuration{32, {48'000, 1}}, {std::move(track)}));
    MediaAsset reachable{{5},
                         "source.wav",
                         32,
                         {48'000, 1},
                         file_hash(source_path),
                         AssetStoragePolicy::External,
                         {{AssetLocatorKind::ExternalUri, source_path.string()}},
                         {},
                         {}};
    auto offline_hash = ContentHash::from_hex(runtime::sha256_hex("unreachable-offline-media"));
    REQUIRE(offline_hash);
    MediaAsset unreachable{
        {6}, "offline.wav", 64, {48'000, 1}, *offline_hash, AssetStoragePolicy::External, {}, {},
        {}};
    auto offline_clip = take(Clip::create_absolute({9}, {0}, 64, {48'000, 1},
                                                   MediaRef{{6}, {0}, 64}, {.gain_linear = 1.0f}));
    auto offline_track = take(Track::create({10}, "offline", {std::move(offline_clip)}));
    auto offline_sequence = take(Sequence::create({8}, "not selected", std::nullopt,
                                                  AbsoluteTimelineDuration{64, {48'000, 1}},
                                                  {std::move(offline_track)}));
    auto project =
        take(Project::create(ProjectInput{{1},
                                          "offline",
                                          11,
                                          {2},
                                          {std::move(reachable), std::move(unreachable)},
                                          {std::move(sequence), std::move(offline_sequence)}}));

    const auto json = project_to_json(project);
    REQUIRE(tools::timeline::explain(json));
    const auto output_path = temp.path() / "output.wav";
    REQUIRE(tools::timeline::render(json, output_path.string()));
    const auto rendered = audio::read_audio_file(output_path.string());
    REQUIRE(rendered);
    REQUIRE_THAT(rendered->channels[0][0], WithinAbs(0.75f, 1e-7f));
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

TEST_CASE("timeline agent decodes local file URI media locators") {
    TempDirectory temp;
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = temp.path() / "source file.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));

    auto file_uri = "file://" + source_path.generic_string();
    for (auto space = file_uri.find(' '); space != std::string::npos;
         space = file_uri.find(' ', space + 3))
        file_uri.replace(space, 1, "%20");
    std::vector<AssetLocator> locators{
        {AssetLocatorKind::ExternalUri, "https://example.invalid/source.wav"},
        {AssetLocatorKind::ExternalUri, std::move(file_uri)},
    };
    const auto output_path = temp.path() / "output.wav";
    REQUIRE(tools::timeline::render(
        project_json(source_path, 32, file_hash(source_path), true, std::move(locators)),
        output_path.string()));
    const auto rendered = audio::read_audio_file(output_path.string());
    REQUIRE(rendered);
    REQUIRE_THAT(rendered->channels[0][0], WithinAbs(0.75f, 1e-7f));
}

TEST_CASE("timeline agent renders package-relative media beneath the project directory") {
    TempDirectory temp;
    const auto package = temp.path() / "package";
    const auto media = package / "media";
    REQUIRE(std::filesystem::create_directories(media));
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = media / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto project_path = package / "project.json";
    write_text_file(project_path,
                    project_json(source_path, 32, file_hash(source_path), true,
                                 {{AssetLocatorKind::PackageRelative, "media/source.wav"}}));

    const auto output_path = temp.path() / "output.wav";
    REQUIRE(tools::timeline::render(project_path.string(), output_path.string()));
    const auto rendered = audio::read_audio_file(output_path.string());
    REQUIRE(rendered);
    REQUIRE_THAT(rendered->channels[0][0], WithinAbs(0.75f, 1e-7f));

    const auto missing_project_path = package / "missing-project.json";
    write_text_file(missing_project_path,
                    project_json(source_path, 32, file_hash(source_path), true,
                                 {{AssetLocatorKind::PackageRelative, "media/missing.wav"}}));
    const auto missing = tools::timeline::render(missing_project_path.string(),
                                                 (temp.path() / "missing.wav").string());
    REQUIRE_FALSE(missing);
    REQUIRE(missing.json.find(R"("stage":"render")") != std::string::npos);
}

TEST_CASE("timeline agent rejects rooted package-relative media hints") {
    TempDirectory temp;
    const auto package = temp.path() / "package";
    REQUIRE(std::filesystem::create_directory(package));
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = package / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));

    std::vector<std::string> rooted_hints{source_path.string(), R"(\source.wav)", "C:source.wav"};
#ifndef _WIN32
    REQUIRE(std::filesystem::copy_file(source_path, package / R"(\source.wav)"));
    REQUIRE(std::filesystem::copy_file(source_path, package / "C:source.wav"));
#endif
    for (std::size_t index = 0; index < rooted_hints.size(); ++index) {
        const auto project_path = package / ("rooted-" + std::to_string(index) + ".json");
        write_text_file(project_path,
                        project_json(source_path, 32, file_hash(source_path), true,
                                     {{AssetLocatorKind::PackageRelative, rooted_hints[index]}}));
        const auto result = tools::timeline::render(
            project_path.string(),
            (temp.path() / ("output-" + std::to_string(index) + ".wav")).string());
        INFO(rooted_hints[index]);
        REQUIRE_FALSE(result);
        REQUIRE(result.json.find(R"("stage":"render")") != std::string::npos);
    }
}

TEST_CASE("timeline agent rejects parent traversal in either package path syntax") {
    TempDirectory temp;
    const auto package = temp.path() / "package";
    REQUIRE(std::filesystem::create_directory(package));
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto outside_path = temp.path() / "outside.wav";
    REQUIRE(audio::write_wav_file(outside_path.string(), source, audio::WavBitDepth::Float32));
#ifndef _WIN32
    REQUIRE(std::filesystem::copy_file(outside_path, package / R"(..\outside.wav)"));
#endif

    const std::vector<std::string> traversal_hints{"../outside.wav", R"(..\outside.wav)"};
    for (std::size_t index = 0; index < traversal_hints.size(); ++index) {
        const auto project_path = package / ("traversal-" + std::to_string(index) + ".json");
        write_text_file(project_path, project_json(outside_path, 32, file_hash(outside_path), true,
                                                   {{AssetLocatorKind::PackageRelative,
                                                     traversal_hints[index]}}));
        const auto result = tools::timeline::render(
            project_path.string(),
            (temp.path() / ("output-" + std::to_string(index) + ".wav")).string());
        INFO(traversal_hints[index]);
        REQUIRE_FALSE(result);
        REQUIRE(result.json.find(R"("stage":"render")") != std::string::npos);
    }
}

TEST_CASE("timeline agent rejects package-relative media symlinks that escape the package") {
    TempDirectory temp;
    const auto package = temp.path() / "package";
    REQUIRE(std::filesystem::create_directory(package));
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto outside_path = temp.path() / "outside.wav";
    REQUIRE(audio::write_wav_file(outside_path.string(), source, audio::WavBitDepth::Float32));
    std::error_code symlink_error;
    std::filesystem::create_symlink(outside_path, package / "linked.wav", symlink_error);
    if (symlink_error)
        SKIP("file symlinks are unavailable in this environment");

    const auto project_path = package / "project.json";
    write_text_file(project_path,
                    project_json(outside_path, 32, file_hash(outside_path), true,
                                 {{AssetLocatorKind::PackageRelative, "linked.wav"}}));
    const auto result =
        tools::timeline::render(project_path.string(), (temp.path() / "output.wav").string());
    REQUIRE_FALSE(result);
    REQUIRE(result.json.find(R"("stage":"render")") != std::string::npos);
}

TEST_CASE("timeline agent enforces durable hashes for confined package-relative media") {
    TempDirectory temp;
    const auto package = temp.path() / "package";
    REQUIRE(std::filesystem::create_directory(package));
    audio::AudioFileData source;
    source.sample_rate = 48'000;
    source.channels = {std::vector<float>(32, 0.75f)};
    const auto source_path = package / "source.wav";
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto expected_hash = file_hash(source_path);
    const auto project_path = package / "project.json";
    write_text_file(project_path,
                    project_json(source_path, 32, expected_hash, true,
                                 {{AssetLocatorKind::PackageRelative, "source.wav"}}));

    source.channels[0][0] = 0.25f;
    REQUIRE(audio::write_wav_file(source_path.string(), source, audio::WavBitDepth::Float32));
    const auto result =
        tools::timeline::render(project_path.string(), (temp.path() / "output.wav").string());
    REQUIRE_FALSE(result);
    REQUIRE(result.json.find(R"("stage":"render")") != std::string::npos);
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
