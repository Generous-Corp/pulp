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

fs::path install_active_clap_model(const TempDir& temp) {
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
    return checkpoint;
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

TEST_CASE("audio model registry resolves checkpoint URLs and lookup misses",
          "[audio][tools][codecov]") {
    const auto& models = registered_models();
    REQUIRE_FALSE(models.empty());
    REQUIRE(models[0].model_id == "clap_music_audioset_v1");
    REQUIRE(models[0].auto_downloadable);
    REQUIRE(models[0].download_url.find("https://huggingface.co/")
            == 0);

    auto* model = find_registered_model("clap_music_audioset_v1");
    REQUIRE(model != nullptr);
    REQUIRE(model->backend == "clap");
    REQUIRE(find_registered_model("missing_model") == nullptr);

    REQUIRE(resolve_checkpoint_url("hf://user/repo/path/to/file.pt")
            == "https://huggingface.co/user/repo/resolve/main/path/to/file.pt");
    REQUIRE(resolve_checkpoint_url("https://example.com/model.pt")
            == "https://example.com/model.pt");
    REQUIRE(resolve_checkpoint_url("http://example.com/model.pt")
            == "http://example.com/model.pt");
    REQUIRE(resolve_checkpoint_url("hf://user-only").empty());
    REQUIRE(resolve_checkpoint_url("manual://model.pt").empty());
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

TEST_CASE("audio model status reports malformed state files",
          "[audio][tools][codecov]") {
    TempDir temp;
    write_text(temp.path / "audio" / "model-state.json", "{ not-json");

    auto status = query_model_status(temp.path);

    REQUIRE(status.state_file_found);
    REQUIRE(status.state_path == temp.path / "audio" / "model-state.json");
    REQUIRE_FALSE(status.loadable());
    REQUIRE(status.message.find("failed to parse") != std::string::npos);
}

TEST_CASE("audio model status reads legacy model.json and installed metadata fallbacks",
          "[audio][tools][codecov]") {
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
    write_text(temp.path / "audio" / "model.json", R"JSON({
  "model_id": "clap_music_audioset_v1"
}
)JSON");

    auto status = query_model_status(temp.path);

    REQUIRE(status.state_file_found);
    REQUIRE(status.state_path == temp.path / "audio" / "model.json");
    REQUIRE(status.configured_model_id == "clap_music_audioset_v1");
    REQUIRE(status.backend == "clap");
    REQUIRE(status.checkpoint_ref == "hf://lukewys/laion_clap/music.pt");
    REQUIRE(status.resolved_checkpoint_path == checkpoint);
    REQUIRE(status.loadable());
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

TEST_CASE("audio model store accepts overrides and legacy checkpoint metadata",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "legacy-clap.pt";
    write_text(checkpoint, "stub");

    REQUIRE(resolve_pulp_home(temp.path) == temp.path);
    REQUIRE(audio_model_state_path(temp.path)
            == temp.path / "audio" / "model-state.json");
    REQUIRE(audio_model_install_path("clap_music_audioset_v1", temp.path)
            == temp.path / "audio" / "models" / "clap_music_audioset_v1.json");

    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "legacy-clap",
  "checkpoint_ref": "manual://legacy",
  "checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");
    auto installed = read_installed_model("clap_music_audioset_v1", temp.path);
    REQUIRE(installed.metadata_found);
    REQUIRE(installed.loadable());
    REQUIRE(installed.backend == "legacy-clap");
    REQUIRE(installed.checkpoint_ref == "manual://legacy");
    REQUIRE(installed.resolved_checkpoint_path == checkpoint);

    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "requested_model_id": "clap_music_audioset_v1"
}
)JSON");
    REQUIRE(read_active_model_id(temp.path) == "clap_music_audioset_v1");
}

TEST_CASE("audio model and bundle JSON serializers include stable fields",
          "[audio][tools][codecov]") {
    ModelListResult list;
    list.pulp_home = "/tmp/pulp-home";
    list.active_model_id = "clap_music_audioset_v1";
    list.error = "sample error";
    ListedModel listed;
    listed.model.model_id = "clap_music_audioset_v1";
    listed.model.display_name = "CLAP Music";
    listed.model.backend = "clap";
    listed.model.checkpoint_ref = "hf://example/model.pt";
    listed.model.task_tags = {"music", "embedding"};
    listed.model.size_bytes = 1234;
    listed.status = "installed";
    listed.active = true;
    listed.resolved_checkpoint_path = "/tmp/model.pt";
    list.models.push_back(listed);

    auto list_json = to_json(list);
    REQUIRE(list_json.find("\"active_model_id\": \"clap_music_audioset_v1\"")
            != std::string::npos);
    REQUIRE(list_json.find("\"status\": \"installed\"") != std::string::npos);
    REQUIRE(list_json.find("\"active\": true") != std::string::npos);
    REQUIRE(list_json.find("\"error\": \"sample error\"") != std::string::npos);

    ActivateModelResult activation;
    activation.ok = true;
    activation.state_path = "/tmp/state.json";
    activation.active_model_id = "clap_music_audioset_v1";
    activation.backend = "clap";
    activation.checkpoint_ref = "hf://example/model.pt";
    activation.resolved_checkpoint_path = "/tmp/model.pt";
    auto activation_json = to_json(activation);
    REQUIRE(activation_json.find("\"ok\": true") != std::string::npos);
    REQUIRE(activation_json.find("\"state_path\": \"/tmp/state.json\"")
            != std::string::npos);

    ModelStatus status;
    status.state_path = "/tmp/state.json";
    status.state_file_found = true;
    status.configured_model_id = "clap_music_audioset_v1";
    status.resolved_checkpoint_path = "/tmp/model.pt";
    status.checkpoint_exists = true;
    status.message = "configured model is loadable";
    auto status_json = to_json(status);
    REQUIRE(status_json.find("\"loadable\": true") != std::string::npos);
    REQUIRE(status_json.find("\"message\": \"configured model is loadable\"")
            != std::string::npos);

    BundleReadResult bundle;
    bundle.ok = true;
    bundle.bundle_path = "/tmp/bundle";
    bundle.bundle_version = 1;
    bundle.tool = "pulp audio excerpt-find";
    bundle.result_count = 1;
    bundle.results.push_back({1, 0.5, "input.wav", 1000.0, 100.0, 200.0,
                              "excerpts/rank-01.wav"});
    auto bundle_json = to_json(bundle);
    REQUIRE(bundle_json.find("\"result_count\": 1") != std::string::npos);
    REQUIRE(bundle_json.find("\"excerpt_file\": \"excerpts/rank-01.wav\"")
            != std::string::npos);
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

TEST_CASE("excerpt bundle reader uses defaults and model/result fallbacks",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto bundle = temp.path / "bundle";
    fs::create_directories(bundle);

    write_text(bundle / "manifest.json", R"JSON({
  "tool": "pulp audio excerpt-find",
  "bundle_version": 1
}
)JSON");
    write_text(bundle / "model.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "null"
}
)JSON");
    write_text(bundle / "ranked_results.json", R"JSON({
  "results": [
    "ignored",
    {
      "score": 0.25,
      "source_path": "legacy-source.wav",
      "start_ms": 10,
      "end_ms": 20
    }
  ]
}
)JSON");

    auto result = read_excerpt_bundle(bundle);

    REQUIRE(result.ok);
    REQUIRE(result.model_path == bundle / "model.json");
    REQUIRE(result.ranked_results_path == bundle / "ranked_results.json");
    REQUIRE(result.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(result.loaded_model_id == "clap_music_audioset_v1");
    REQUIRE(result.backend == "null");
    REQUIRE(result.result_count == 1);
    REQUIRE(result.results.size() == 1);
    REQUIRE(result.results[0].rank == 2);
    REQUIRE(result.results[0].source_file == "legacy-source.wav");
}

TEST_CASE("excerpt bundle reader reports empty and missing ranked inputs",
          "[audio][tools][codecov]") {
    auto empty = read_excerpt_bundle({});
    REQUIRE_FALSE(empty.ok);
    REQUIRE(empty.error == "bundle path is required");

    TempDir temp;
    auto bundle = temp.path / "bundle";
    fs::create_directories(bundle);
    write_text(bundle / "manifest.json", R"JSON({
  "ranked_results_file": "missing.json"
}
)JSON");

    auto missing_ranked = read_excerpt_bundle(bundle);
    REQUIRE_FALSE(missing_ranked.ok);
    REQUIRE(missing_ranked.error.find("missing ranked results file")
            != std::string::npos);
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

TEST_CASE("excerpt find validates required request fields before model loading",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto input = temp.path / "input.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.input_path = input;

    auto missing_text = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(missing_text.ok);
    REQUIRE(missing_text.error == "text query is required");

    request.text = "kick";
    request.input_path.clear();
    auto missing_input = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(missing_input.ok);
    REQUIRE(missing_input.error == "input path is required");

    request.input_path = input;
    request.top_k = 0;
    auto bad_top = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(bad_top.ok);
    REQUIRE(bad_top.error == "top and max_candidates_per_file must be >= 1");

    request.top_k = 1;
    request.max_candidates_per_file = 0;
    auto bad_max = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(bad_max.ok);
    REQUIRE(bad_max.error == "top and max_candidates_per_file must be >= 1");

    request.max_candidates_per_file = 1;
    request.window_ms = 0;
    auto bad_window = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(bad_window.ok);
    REQUIRE(bad_window.error == "window_ms and hop_ms must be >= 1");
}

TEST_CASE("excerpt find reports unsupported inputs after resolving a model",
          "[audio][tools][codecov]") {
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

    auto text_file = temp.path / "notes.txt";
    write_text(text_file, "not audio");

    ExcerptFindRequest request;
    request.text = "texture";
    request.input_path = text_file;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "no supported WAV inputs found");
    REQUIRE(result.scanned_file_count == 0);
    REQUIRE(result.skipped_files.size() == 1);
    REQUIRE(result.skipped_files[0].find("unsupported; WAV only") != std::string::npos);
}

TEST_CASE("excerpt find JSON serializer includes results and skipped files",
          "[audio][tools][codecov]") {
    ExcerptFindResult result;
    result.ok = true;
    result.bundle_path = "/tmp/bundle";
    result.query = "kick";
    result.requested_model_id = "clap_music_audioset_v1";
    result.loaded_model_id = "clap_music_audioset_v1";
    result.backend = "null";
    result.resolved_checkpoint_path = "/tmp/model.pt";
    result.scanned_file_count = 2;
    result.results.push_back({1, 0.75, "input.wav", 1000.0, 0.0, 500.0,
                              "excerpts/rank-01.wav"});
    result.skipped_files.push_back("notes.txt (unsupported; WAV only)");

    auto json = to_json(result);

    REQUIRE(json.find("\"ok\": true") != std::string::npos);
    REQUIRE(json.find("\"query\": \"kick\"") != std::string::npos);
    REQUIRE(json.find("\"scanned_file_count\": 2") != std::string::npos);
    REQUIRE(json.find("\"excerpt_file\": \"excerpts/rank-01.wav\"")
            != std::string::npos);
    REQUIRE(json.find("notes.txt (unsupported; WAV only)") != std::string::npos);
}

TEST_CASE("excerpt find reports explicit unknown models",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto input = temp.path / "input.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.text = "texture";
    request.input_path = input;
    request.model_id = "not_a_model";

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "unknown model_id: not_a_model");
}

TEST_CASE("excerpt find collects uppercase WAV files and records unsupported siblings",
          "[audio][tools][codecov]") {
    TempDir temp;
    install_active_clap_model(temp);

    auto wav = temp.path / "Upper.WAV";
    REQUIRE(pulp::audio::write_wav_file(wav.string(), make_audio(48000, 48000)));
    write_text(temp.path / "notes.mp3", "not really mp3");
    fs::create_directories(temp.path / "nested");
    REQUIRE(pulp::audio::write_wav_file(
        (temp.path / "nested" / "ignored.wav").string(),
        make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.text = "texture";
    request.input_path = temp.path;
    request.recursive = false;
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE(result.results.size() == 1);
    REQUIRE(result.results[0].source_file == wav.string());
    REQUIRE(result.skipped_files.size() == 1);
    REQUIRE(result.skipped_files[0].find("notes.mp3") != std::string::npos);
    REQUIRE(result.skipped_files[0].find("unsupported; WAV only") != std::string::npos);
}

TEST_CASE("excerpt find can dry-run with all candidates below min score",
          "[audio][tools][codecov]") {
    TempDir temp;
    install_active_clap_model(temp);
    auto input = temp.path / "input.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.text = "texture";
    request.input_path = input;
    request.top_k = 3;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 2;
    request.min_score = 2.0;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.bundle_path.empty());
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE(result.results.empty());
    REQUIRE(result.skipped_files.empty());
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
