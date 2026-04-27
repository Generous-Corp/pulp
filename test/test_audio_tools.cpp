#include <pulp/audio/audio_file.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/service.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace pulp::tools::audio;

namespace {

uint64_t current_process_id() {
#ifdef _WIN32
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<uint64_t> next_id{0};
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto unique_id = next_id.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path()
             / ("pulp-audio-tools-" + std::to_string(current_process_id()) + "-"
                + std::to_string(stamp) + "-" + std::to_string(unique_id));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    REQUIRE(output.good());
    output << text;
}

pulp::audio::AudioFileData make_audio(uint32_t sample_rate, uint64_t frame_count) {
    pulp::audio::AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(1);
    auto& samples = data.channels[0];
    samples.resize(static_cast<std::size_t>(frame_count));
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>((i % 97) / 97.0);
    return data;
}

uint64_t stable_hash64(std::string_view text) {
    constexpr uint64_t offset_basis = 14695981039346656037ull;
    constexpr uint64_t prime = 1099511628211ull;
    uint64_t hash = offset_basis;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= prime;
    }
    return hash;
}

} // namespace

TEST_CASE("audio model list reports registry and install state", "[audio][tools]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "checkpoint_ref": "hf://lukewys/laion_clap/music.pt",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "active_model_id": "clap_music_audioset_v1"
}
)JSON");

    auto result = list_models(temp.path);

    REQUIRE(result.error.empty());
    REQUIRE(result.active_model_id == "clap_music_audioset_v1");
    REQUIRE(result.models.size() == 1);
    REQUIRE(result.models[0].model.model_id == "clap_music_audioset_v1");
    REQUIRE(result.models[0].status == "installed");
    REQUIRE(result.models[0].active);
}

TEST_CASE("audio model status reports missing config cleanly", "[audio][tools]") {
    TempDir temp;

    auto status = query_model_status(temp.path);

    REQUIRE_FALSE(status.state_file_found);
    REQUIRE(status.configured_model_id.empty());
    REQUIRE_FALSE(status.loadable());
    REQUIRE(status.message == "no configured audio model");
}

TEST_CASE("audio model status reports configured checkpoint loadability", "[audio][tools]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "configured_model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "checkpoint_ref": "hf://lukewys/laion_clap/music.pt",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");

    auto status = query_model_status(temp.path);

    REQUIRE(status.state_file_found);
    REQUIRE(status.configured_model_id == "clap_music_audioset_v1");
    REQUIRE(status.backend == "clap");
    REQUIRE(status.checkpoint_exists);
    REQUIRE(status.loadable());
    REQUIRE(status.message == "configured model is loadable");
}

TEST_CASE("audio model activate writes state from installed metadata", "[audio][tools]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "checkpoint_ref": "hf://lukewys/laion_clap/music.pt",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");

    auto activation = activate_model("clap_music_audioset_v1", temp.path);

    REQUIRE(activation.ok);
    REQUIRE(activation.active_model_id == "clap_music_audioset_v1");
    REQUIRE(fs::exists(activation.state_path));

    auto status = query_model_status(temp.path);
    REQUIRE(status.loadable());
    REQUIRE(status.configured_model_id == "clap_music_audioset_v1");
    REQUIRE(status.checkpoint_exists);
}

TEST_CASE("audio model activate rejects unknown or uninstalled models", "[audio][tools]") {
    TempDir temp;

    auto unknown = activate_model("not_a_model", temp.path);
    REQUIRE_FALSE(unknown.ok);
    REQUIRE(unknown.error.find("unknown model_id") != std::string::npos);

    auto missing = activate_model("clap_music_audioset_v1", temp.path);
    REQUIRE_FALSE(missing.ok);
    REQUIRE(missing.error.find("not installed") != std::string::npos);
}

TEST_CASE("audio model activate rejects installed metadata with missing checkpoint", "[audio][tools]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "missing-clap.pt";
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "checkpoint_ref": "hf://lukewys/laion_clap/music.pt",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");

    auto list = list_models(temp.path);
    REQUIRE(list.error.empty());
    REQUIRE(list.models.size() == 1);
    REQUIRE(list.models[0].status == "missing_checkpoint");
    REQUIRE_FALSE(list.models[0].resolved_checkpoint_path.empty());

    auto activation = activate_model("clap_music_audioset_v1", temp.path);
    REQUIRE_FALSE(activation.ok);
    REQUIRE(activation.error.find("checkpoint does not exist") != std::string::npos);
}

TEST_CASE("excerpt bundle reader summarizes manifest and ranked results", "[audio][tools]") {
    TempDir temp;
    auto bundle = temp.path / "bundle";
    fs::create_directories(bundle);

    write_text(bundle / "manifest.json", R"JSON({
  "tool": "pulp audio excerpt-find",
  "bundle_version": 1,
  "model_file": "model.json",
  "ranked_results_file": "ranked_results.json",
  "requested_model_id": "clap_music_audioset_v1",
  "loaded_model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "result_count": 2
}
)JSON");

    write_text(bundle / "model.json", R"JSON({
  "requested_model_id": "clap_music_audioset_v1",
  "loaded_model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "resolved_checkpoint_path": "/tmp/checkpoint.pt"
}
)JSON");

    write_text(bundle / "ranked_results.json", R"JSON({
  "query": "airy vocal texture",
  "results": [
    {
      "rank": 1,
      "score": 0.4182,
      "source_file": "/tmp/chops-01.wav",
      "source_duration_ms": 12873,
      "start_ms": 12400,
      "end_ms": 14000,
      "excerpt_file": "excerpts/rank-01.wav"
    },
    {
      "rank": 2,
      "score": 0.4011,
      "source_file": "/tmp/chops-02.wav",
      "source_duration_ms": 10102,
      "start_ms": 3600,
      "end_ms": 5200,
      "excerpt_file": "excerpts/rank-02.wav"
    }
  ]
}
)JSON");

    auto result = read_excerpt_bundle(bundle);

    REQUIRE(result.ok);
    REQUIRE(result.tool == "pulp audio excerpt-find");
    REQUIRE(result.bundle_version == 1);
    REQUIRE(result.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(result.loaded_model_id == "clap_music_audioset_v1");
    REQUIRE(result.backend == "clap");
    REQUIRE(result.result_count == 2);
    REQUIRE(result.results.size() == 2);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[0].excerpt_file == "excerpts/rank-01.wav");
}

TEST_CASE("excerpt bundle reader fails clearly without manifest", "[audio][tools]") {
    TempDir temp;
    auto result = read_excerpt_bundle(temp.path / "missing-bundle");

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("bundle path does not exist") != std::string::npos);
}

TEST_CASE("excerpt find writes a deterministic WAV-first bundle", "[audio][tools]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "checkpoint_ref": "hf://lukewys/laion_clap/music.pt",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "active_model_id": "clap_music_audioset_v1"
}
)JSON");

    auto input = temp.path / "input.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 96000)));

    ExcerptFindRequest request;
    request.text = "airy vocal texture";
    request.input_path = input;
    request.bundle_out = temp.path / "bundles";
    request.top_k = 3;
    request.window_ms = 500;
    request.hop_ms = 250;
    request.max_candidates_per_file = 2;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.backend == "null");
    REQUIRE(result.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(result.loaded_model_id == "clap_music_audioset_v1");
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE_FALSE(result.bundle_path.empty());
    REQUIRE(fs::exists(result.bundle_path / "manifest.json"));
    REQUIRE(fs::exists(result.bundle_path / "query.json"));
    REQUIRE(fs::exists(result.bundle_path / "model.json"));
    REQUIRE(fs::exists(result.bundle_path / "inputs.json"));
    REQUIRE(fs::exists(result.bundle_path / "ranked_results.json"));
    REQUIRE(fs::exists(result.bundle_path / "logs" / "runtime.log"));
    REQUIRE_FALSE(result.results.empty());
    REQUIRE(fs::exists(result.bundle_path / result.results[0].excerpt_file));

    auto bundle = read_excerpt_bundle(result.bundle_path);
    REQUIRE(bundle.ok);
    REQUIRE(bundle.backend == "null");
    REQUIRE(bundle.results.size() == result.results.size());
}

TEST_CASE("excerpt find deterministic scores use a stable hash", "[audio][tools]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "checkpoint_ref": "hf://lukewys/laion_clap/music.pt",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "active_model_id": "clap_music_audioset_v1"
}
)JSON");

    auto input = temp.path / "single-window.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.text = "stable score";
    request.input_path = input;
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.results.size() == 1);
    const auto key = request.text + "|" + input.string() + "|0|48000";
    const auto expected = static_cast<double>(stable_hash64(key) % 1000000ULL) / 1000000.0;
    REQUIRE(result.results.front().score == Catch::Approx(expected));
}

#if !defined(_WIN32)
TEST_CASE("excerpt find skips unreadable recursive subdirectories", "[audio][tools]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "checkpoint_ref": "hf://lukewys/laion_clap/music.pt",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "active_model_id": "clap_music_audioset_v1"
}
)JSON");

    auto readable = temp.path / "readable.wav";
    REQUIRE(pulp::audio::write_wav_file(readable.string(), make_audio(48000, 48000)));

    auto blocked_dir = temp.path / "blocked";
    fs::create_directories(blocked_dir);
    auto blocked_file = blocked_dir / "secret.wav";
    REQUIRE(pulp::audio::write_wav_file(blocked_file.string(), make_audio(48000, 48000)));

    fs::permissions(
        blocked_dir,
        fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all,
        fs::perm_options::remove);

    ExcerptFindRequest request;
    request.text = "skip blocked";
    request.input_path = temp.path;
    request.recursive = true;
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    fs::permissions(
        blocked_dir,
        fs::perms::owner_all,
        fs::perm_options::replace);

    REQUIRE(result.ok);
    REQUIRE(result.scanned_file_count == 1);
}
#endif

TEST_CASE("excerpt bundle reader rejects non-array ranked results", "[audio][tools]") {
    TempDir temp;
    auto bundle = temp.path / "bundle";
    fs::create_directories(bundle);

    write_text(bundle / "manifest.json", R"JSON({
  "tool": "pulp audio excerpt-find",
  "bundle_version": 1,
  "model_file": "model.json",
  "ranked_results_file": "ranked_results.json"
}
)JSON");

    write_text(bundle / "model.json", R"JSON({
  "requested_model_id": "clap_music_audioset_v1",
  "loaded_model_id": "clap_music_audioset_v1",
  "backend": "clap"
}
)JSON");

    write_text(bundle / "ranked_results.json", R"JSON({
  "query": "airy vocal texture",
  "results": {
    "rank": 1
  }
}
)JSON");

    auto result = read_excerpt_bundle(bundle);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "ranked results file does not contain a results array");
}
