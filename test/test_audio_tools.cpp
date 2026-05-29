#include <pulp/audio/audio_file.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/tools/audio/excerpt_service.hpp>
#include <pulp/tools/audio/model_registry.hpp>
#include <pulp/tools/audio/model_store.hpp>
#include <pulp/tools/audio/service.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/stat.h>
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

std::string read_text(const fs::path& path) {
    std::ifstream input(path);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string json_escape(std::string_view text) {
    std::string out;
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
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

pulp::audio::AudioFileData make_stereo_audio(uint32_t sample_rate, uint64_t frame_count) {
    pulp::audio::AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(2);
    data.channels[0].resize(static_cast<std::size_t>(frame_count));
    data.channels[1].resize(static_cast<std::size_t>(frame_count));
    for (std::size_t i = 0; i < static_cast<std::size_t>(frame_count); ++i) {
        data.channels[0][i] = static_cast<float>(i) / static_cast<float>(frame_count);
        data.channels[1][i] = 1.0f - data.channels[0][i];
    }
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

void write_installed_model_metadata(const fs::path& pulp_home,
                                    const fs::path& checkpoint,
                                    std::string_view checkpoint_key = "resolved_checkpoint_path") {
    write_text(pulp_home / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "backend": "clap",
  "checkpoint_ref": "hf://lukewys/laion_clap/music.pt",
  ")JSON" + std::string(checkpoint_key) + R"JSON(": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");
}

} // namespace

TEST_CASE("audio model registry resolves known models and checkpoint URLs",
          "[audio][tools][issue-643]") {
    const auto& models = registered_models();
    REQUIRE_FALSE(models.empty());

    auto* model = find_registered_model("clap_music_audioset_v1");
    REQUIRE(model != nullptr);
    REQUIRE(model->backend == "clap");
    REQUIRE(model->auto_downloadable);
    REQUIRE(find_registered_model("not_a_model") == nullptr);

    REQUIRE(resolve_checkpoint_url("hf://user/repo/path/to/model.pt")
            == "https://huggingface.co/user/repo/resolve/main/path/to/model.pt");
    REQUIRE(resolve_checkpoint_url("https://example.test/model.bin")
            == "https://example.test/model.bin");
    REQUIRE(resolve_checkpoint_url("http://example.test/model.bin")
            == "http://example.test/model.bin");
    REQUIRE(resolve_checkpoint_url("hf://user-only").empty());
    REQUIRE(resolve_checkpoint_url("hf://user/repo").empty());
    REQUIRE(resolve_checkpoint_url("hf://user/repo/").empty());
    REQUIRE(resolve_checkpoint_url("file:///tmp/model.pt").empty());
}

TEST_CASE("audio model registry preserves direct URLs and rejects incomplete refs",
          "[audio][tools][codecov]") {
    REQUIRE(resolve_checkpoint_url("https://cdn.example.test/models/clap.pt?download=1")
            == "https://cdn.example.test/models/clap.pt?download=1");
    REQUIRE(resolve_checkpoint_url("hf://org/repo/checkpoints/music.pt")
            == "https://huggingface.co/org/repo/resolve/main/checkpoints/music.pt");
    REQUIRE(resolve_checkpoint_url("").empty());
    REQUIRE(resolve_checkpoint_url("HF://org/repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("ftp://example.test/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo").empty());
}

TEST_CASE("audio model registry exposes stable CLAP model metadata",
          "[audio][tools][model-registry][coverage]") {
    const auto& first = registered_models();
    const auto& second = registered_models();
    REQUIRE(&first == &second);
    REQUIRE_FALSE(first.empty());

    const auto* lookup = find_registered_model("clap_music_audioset_v1");
    REQUIRE(lookup != nullptr);
    const auto& model = *lookup;
    REQUIRE(model.model_id == "clap_music_audioset_v1");
    REQUIRE(model.display_name == "CLAP Music AudioSet");
    REQUIRE(model.backend == "clap");
    REQUIRE(model.checkpoint_ref == "hf://lukewys/laion_clap/music.pt");
    REQUIRE(model.download_url
            == "https://huggingface.co/lukewys/laion_clap/resolve/main/music.pt");
    REQUIRE(model.sha256.empty());
    REQUIRE(model.auto_downloadable);
    REQUIRE(model.size_bytes == 2523293286ULL);
    REQUIRE(model.size_bytes > 2'000'000'000ULL);
    REQUIRE(model.task_tags.size() == 2);
    REQUIRE(model.task_tags[0] == "music");
    REQUIRE(model.task_tags[1] == "excerpt_find");

    REQUIRE(lookup->download_url == resolve_checkpoint_url(model.checkpoint_ref));
}

TEST_CASE("audio model registry lookup is exact and non-mutating",
          "[audio][tools][model-registry][coverage]") {
    const auto& models = registered_models();
    const auto* model = find_registered_model("clap_music_audioset_v1");
    REQUIRE(model != nullptr);
    REQUIRE(model == &models.front());

    REQUIRE(find_registered_model("") == nullptr);
    REQUIRE(find_registered_model("clap_music_audioset_v1 ") == nullptr);
    REQUIRE(find_registered_model(" clap_music_audioset_v1") == nullptr);
    REQUIRE(find_registered_model("CLAP_MUSIC_AUDIOSET_V1") == nullptr);
    REQUIRE(find_registered_model("clap_music_audioset") == nullptr);

    const auto* again = find_registered_model("clap_music_audioset_v1");
    REQUIRE(again == model);
    REQUIRE(registered_models().size() == models.size());
    REQUIRE(registered_models().front().model_id == model->model_id);
}

TEST_CASE("audio model registry resolves Hugging Face refs with nested files",
          "[audio][tools][model-registry][coverage]") {
    REQUIRE(resolve_checkpoint_url("hf://org/repo/model.pt")
            == "https://huggingface.co/org/repo/resolve/main/model.pt");
    REQUIRE(resolve_checkpoint_url("hf://org/repo/checkpoints/music/model.bin")
            == "https://huggingface.co/org/repo/resolve/main/checkpoints/music/model.bin");
    REQUIRE(resolve_checkpoint_url("hf://org-name/repo.name/sub.dir/file.safetensors")
            == "https://huggingface.co/org-name/repo.name/resolve/main/sub.dir/file.safetensors");
    REQUIRE(resolve_checkpoint_url("hf://user/repo/path%20with%20spaces/model.pt")
            == "https://huggingface.co/user/repo/resolve/main/path%20with%20spaces/model.pt");
    REQUIRE(resolve_checkpoint_url("hf://u/r/a/b/c")
            == "https://huggingface.co/u/r/resolve/main/a/b/c");
}

TEST_CASE("audio model registry rejects malformed checkpoint refs",
          "[audio][tools][model-registry][coverage]") {
    REQUIRE(resolve_checkpoint_url("hf://").empty());
    REQUIRE(resolve_checkpoint_url("hf:///repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo/").empty());
    REQUIRE(resolve_checkpoint_url(" hf://org/repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("HF://org/repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("s3://bucket/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("file:///tmp/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("https:/example.test/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("http:/example.test/model.pt").empty());
}

TEST_CASE("audio model registry rejects unsafe Hugging Face file paths",
          "[audio][tools][model-registry][coverage][requested]") {
    REQUIRE(resolve_checkpoint_url(std::string("hf://org/repo/path/")
                                   + static_cast<char>(0x1f) + "model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo/\rmodel.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo//model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo/../model.pt")
            == "https://huggingface.co/org/repo/resolve/main/../model.pt");
}

TEST_CASE("audio model registry rejects unsafe Hugging Face owner and repo names",
          "[audio][tools][model-registry][coverage][requested]") {
    REQUIRE(resolve_checkpoint_url(std::string("hf://org/re")
                                   + static_cast<char>(0x01)
                                   + "po/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org\nname/repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo\tname/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo/model.pt")
            == "https://huggingface.co/org/repo/resolve/main/model.pt");
}

TEST_CASE("audio model registry preserves direct HTTP URLs byte-for-byte",
          "[audio][tools][model-registry][coverage]") {
    REQUIRE(resolve_checkpoint_url("https://example.test/model.pt")
            == "https://example.test/model.pt");
    REQUIRE(resolve_checkpoint_url("http://example.test/model.pt")
            == "http://example.test/model.pt");
    REQUIRE(resolve_checkpoint_url("https://example.test/model.pt?download=1#sha256")
            == "https://example.test/model.pt?download=1#sha256");
    REQUIRE(resolve_checkpoint_url("https://huggingface.co/org/repo/resolve/main/model.pt")
            == "https://huggingface.co/org/repo/resolve/main/model.pt");
    REQUIRE(resolve_checkpoint_url("https://example.test/").ends_with('/'));
}

TEST_CASE("audio model registry preserves URL bytes and rejects partial protocols",
          "[audio][tools][model-registry][coverage]") {
    REQUIRE(resolve_checkpoint_url("https://cdn.example.test/model.pt#sha256")
            == "https://cdn.example.test/model.pt#sha256");
    REQUIRE(resolve_checkpoint_url("http://localhost:8080/checkpoints/music.pt?download=1")
            == "http://localhost:8080/checkpoints/music.pt?download=1");
    REQUIRE(resolve_checkpoint_url("hf://org/repo/a/b/c/model.ckpt")
            == "https://huggingface.co/org/repo/resolve/main/a/b/c/model.ckpt");

    REQUIRE(resolve_checkpoint_url("hf:/org/repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("://org/repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("org/repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org//model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo/").empty());
}

TEST_CASE("audio model registry preserves concrete Hugging Face file paths",
          "[audio][tools][model-registry][coverage][requested]") {
    REQUIRE(resolve_checkpoint_url(" hf://org/repo/model.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo//").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo/\t").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo/path/\nmodel.pt").empty());
    REQUIRE(resolve_checkpoint_url("hf://org/repo/model.pt")
            == "https://huggingface.co/org/repo/resolve/main/model.pt");
    REQUIRE(resolve_checkpoint_url("hf://org/repo/path/to/model.pt")
            == "https://huggingface.co/org/repo/resolve/main/path/to/model.pt");
}

TEST_CASE("excerpt service validates request shape before scanning inputs",
          "[audio][tools][excerpt-service][coverage][requested]") {
    TempDir temp;
    install_active_clap_model(temp);
    fs::create_directories(temp.path / "inputs");

    ExcerptFindRequest request;
    request.input_path = temp.path / "inputs";
    request.text = "";
    auto missing_text = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(missing_text.ok);
    REQUIRE(missing_text.error == "text query is required");

    request.text = "kick transient";
    request.top_k = 0;
    auto bad_top = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(bad_top.ok);
    REQUIRE(bad_top.error == "top and max_candidates_per_file must be >= 1");

    request.top_k = 1;
    request.max_candidates_per_file = 0;
    auto bad_limit = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(bad_limit.ok);
    REQUIRE(bad_limit.error == "top and max_candidates_per_file must be >= 1");

    request.max_candidates_per_file = 1;
    request.window_ms = 0;
    auto bad_window = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(bad_window.ok);
    REQUIRE(bad_window.error == "window_ms and hop_ms must be >= 1");

    request.window_ms = 1000;
    request.hop_ms = 250;
    auto no_wavs = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(no_wavs.ok);
    REQUIRE(no_wavs.scanned_file_count == 0);
    REQUIRE(no_wavs.error == "no supported WAV inputs found");
}

TEST_CASE("audio model store reads legacy metadata and malformed records fail closed",
          "[audio][tools][issue-643]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_installed_model_metadata(temp.path, checkpoint, "checkpoint_path");

    auto record = read_installed_model("clap_music_audioset_v1", temp.path);
    REQUIRE(record.metadata_found);
    REQUIRE(record.model_id == "clap_music_audioset_v1");
    REQUIRE(record.backend == "clap");
    REQUIRE(record.checkpoint_ref == "hf://lukewys/laion_clap/music.pt");
    REQUIRE(record.resolved_checkpoint_path == checkpoint);
    REQUIRE(record.checkpoint_exists);
    REQUIRE(record.loadable());

    write_text(temp.path / "audio" / "models" / "bad_model.json", "{");
    auto bad = read_installed_model("bad_model", temp.path);
    REQUIRE(bad.metadata_found);
    REQUIRE(bad.model_id == "bad_model");
    REQUIRE(bad.backend.empty());
    REQUIRE_FALSE(bad.checkpoint_exists);
    REQUIRE_FALSE(bad.loadable());
}

TEST_CASE("audio model list and activation JSON report inactive and error states",
          "[audio][tools][issue-643]") {
    TempDir temp;

    auto list = list_models(temp.path);
    REQUIRE(list.error.empty());
    REQUIRE(list.active_model_id.empty());
    REQUIRE(list.models.size() == 1);
    REQUIRE(list.models[0].status == "not_installed");
    REQUIRE_FALSE(list.models[0].active);

    auto list_json = to_json(list);
    REQUIRE(list_json.find("\"status\": \"not_installed\"") != std::string::npos);
    REQUIRE(list_json.find("\"active\": false") != std::string::npos);

    auto activation = activate_model("not_a_model", temp.path);
    REQUIRE_FALSE(activation.ok);
    auto activation_json = to_json(activation);
    REQUIRE(activation_json.find("\"ok\": false") != std::string::npos);
    REQUIRE(activation_json.find("unknown model_id: not_a_model") != std::string::npos);
}

TEST_CASE("audio model status falls back to legacy model.json and installed metadata",
          "[audio][tools][issue-643]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_installed_model_metadata(temp.path, checkpoint, "checkpoint_path");
    write_text(temp.path / "audio" / "model.json", R"JSON({
  "model_id": "clap_music_audioset_v1"
}
)JSON");

    auto status = query_model_status(temp.path);

    REQUIRE(status.state_file_found);
    REQUIRE(status.state_path.filename() == "model.json");
    REQUIRE(status.configured_model_id == "clap_music_audioset_v1");
    REQUIRE(status.backend == "clap");
    REQUIRE(status.checkpoint_ref == "hf://lukewys/laion_clap/music.pt");
    REQUIRE(status.resolved_checkpoint_path == checkpoint);
    REQUIRE(status.checkpoint_exists);
    REQUIRE(status.loadable());

    auto json = to_json(status);
    REQUIRE(json.find("\"loadable\": true") != std::string::npos);
    REQUIRE(json.find("\"message\": \"configured model is loadable\"") != std::string::npos);
}

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
    REQUIRE(resolve_checkpoint_url("hf://user/repo/").empty());
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

TEST_CASE("audio model status reports incomplete state shapes",
          "[audio][tools][codecov]") {
    TempDir temp;
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "backend": "clap"
}
)JSON");

    auto missing_model = query_model_status(temp.path);
    REQUIRE(missing_model.state_file_found);
    REQUIRE_FALSE(missing_model.loadable());
    REQUIRE(missing_model.message
            == "model state file does not declare a configured model");

    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "configured_model_id": "clap_music_audioset_v1"
}
)JSON");

    auto missing_checkpoint = query_model_status(temp.path);
    REQUIRE(missing_checkpoint.configured_model_id == "clap_music_audioset_v1");
    REQUIRE_FALSE(missing_checkpoint.loadable());
    REQUIRE(missing_checkpoint.message
            == "configured model has no resolved checkpoint path");

    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "configured_model_id": "clap_music_audioset_v1",
  "resolved_checkpoint_path": ")JSON" + (temp.path / "missing.pt").generic_string() + R"JSON("
}
)JSON");

    auto missing_file = query_model_status(temp.path);
    REQUIRE(missing_file.resolved_checkpoint_path == temp.path / "missing.pt");
    REQUIRE_FALSE(missing_file.checkpoint_exists);
    REQUIRE(missing_file.message == "resolved checkpoint path does not exist");
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

TEST_CASE("audio model activate falls back to registered metadata",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");

    auto activation = activate_model("clap_music_audioset_v1", temp.path);

    REQUIRE(activation.ok);
    REQUIRE(activation.backend == "clap");
    REQUIRE(activation.checkpoint_ref == "hf://lukewys/laion_clap/music.pt");
    REQUIRE(activation.resolved_checkpoint_path == checkpoint);

    auto status = query_model_status(temp.path);
    REQUIRE(status.loadable());
    REQUIRE(status.backend == "clap");
    REQUIRE(status.checkpoint_ref == "hf://lukewys/laion_clap/music.pt");
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

TEST_CASE("audio model store treats malformed install metadata as unloadable",
          "[audio][tools][codecov]") {
    TempDir temp;
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json",
               "{ not-json");

    auto installed = read_installed_model("clap_music_audioset_v1", temp.path);

    REQUIRE(installed.metadata_found);
    REQUIRE(installed.model_id == "clap_music_audioset_v1");
    REQUIRE(installed.backend.empty());
    REQUIRE_FALSE(installed.checkpoint_exists);
    REQUIRE_FALSE(installed.loadable());
}

TEST_CASE("audio model store reads active id aliases in priority order",
          "[audio][tools][codecov]") {
    TempDir temp;
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "configured_model_id": "configured",
  "active_model_id": "active",
  "requested_model_id": "requested",
  "model_id": "model"
}
)JSON");
    REQUIRE(read_active_model_id(temp.path) == "active");

    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "configured_model_id": "configured",
  "requested_model_id": "requested",
  "model_id": "model"
}
)JSON");
    REQUIRE(read_active_model_id(temp.path) == "configured");

    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "requested_model_id": "requested",
  "model_id": "model"
}
)JSON");
    REQUIRE(read_active_model_id(temp.path) == "requested");

    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "model_id": "model"
}
)JSON");
    REQUIRE(read_active_model_id(temp.path) == "model");
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

TEST_CASE("excerpt bundle reader fills defaults from model and ranked result files",
          "[audio][tools][issue-643]") {
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
  "backend": "clap"
}
)JSON");

    write_text(bundle / "ranked_results.json", R"JSON({
  "results": [
    {
      "score": 0.25,
      "source_path": "/tmp/source.wav",
      "end_ms": 100
    },
    42,
    {
      "rank": 5,
      "source_file": "/tmp/other.wav"
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
    REQUIRE(result.backend == "clap");
    REQUIRE(result.result_count == 2);
    REQUIRE(result.results.size() == 2);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[0].score == Catch::Approx(0.25));
    REQUIRE(result.results[0].source_file == "/tmp/source.wav");
    REQUIRE(result.results[1].rank == 5);

    auto json = to_json(result);
    REQUIRE(json.find("\"result_count\": 2") != std::string::npos);
    REQUIRE(json.find("\"source_file\": \"/tmp/source.wav\"") != std::string::npos);
}

TEST_CASE("excerpt bundle reader honors manifest file overrides and numeric fallbacks",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto bundle = temp.path / "bundle";
    fs::create_directories(bundle / "metadata");

    write_text(bundle / "manifest.json", R"JSON({
  "tool": "custom excerpt bundle",
  "bundle_version": 7,
  "model_file": "metadata/model-info.json",
  "ranked_results_file": "metadata/results.json",
  "requested_model_id": "manifest-request",
  "result_count": 9
}
)JSON");

    write_text(bundle / "metadata" / "model-info.json", R"JSON({
  "model_id": "model-fallback",
  "loaded_model_id": "loaded-from-model",
  "backend": "null"
}
)JSON");

    write_text(bundle / "metadata" / "results.json", R"JSON({
  "results": [
    {
      "score": "not-a-number",
      "source_path": "/tmp/a.wav",
      "source_duration_ms": 1500,
      "start_ms": 250,
      "end_ms": 750
    },
    {
      "rank": 4,
      "score": 0.91,
      "source_file": "/tmp/b.wav",
      "excerpt_file": "excerpts/rank-04.wav"
    }
  ]
}
)JSON");

    auto result = read_excerpt_bundle(bundle);

    REQUIRE(result.ok);
    REQUIRE(result.tool == "custom excerpt bundle");
    REQUIRE(result.bundle_version == 7);
    REQUIRE(result.ranked_results_path == bundle / "metadata" / "results.json");
    REQUIRE(result.requested_model_id == "manifest-request");
    REQUIRE(result.loaded_model_id == "loaded-from-model");
    REQUIRE(result.result_count == 9);
    REQUIRE(result.results.size() == 2);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[0].score == Catch::Approx(0.0));
    REQUIRE(result.results[0].source_file == "/tmp/a.wav");
    REQUIRE(result.results[0].source_duration_ms == Catch::Approx(1500.0));
    REQUIRE(result.results[0].start_ms == Catch::Approx(250.0));
    REQUIRE(result.results[0].end_ms == Catch::Approx(750.0));
    REQUIRE(result.results[1].rank == 4);
    REQUIRE(result.results[1].excerpt_file == "excerpts/rank-04.wav");
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

TEST_CASE("excerpt bundle reader reports malformed manifests and result objects",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto bundle = temp.path / "bundle";
    fs::create_directories(bundle);

    write_text(bundle / "manifest.json", "{ not-json");
    auto bad_manifest = read_excerpt_bundle(bundle);
    REQUIRE_FALSE(bad_manifest.ok);
    REQUIRE(bad_manifest.error.find("failed to parse") != std::string::npos);

    write_text(bundle / "manifest.json", R"JSON({
  "ranked_results_file": "ranked_results.json"
}
)JSON");
    write_text(bundle / "ranked_results.json", "{ not-json");
    auto bad_ranked = read_excerpt_bundle(bundle);
    REQUIRE_FALSE(bad_ranked.ok);
    REQUIRE(bad_ranked.error.find("failed to parse") != std::string::npos);

    write_text(bundle / "ranked_results.json", R"JSON({
  "query": "missing results"
}
)JSON");
    auto missing_results = read_excerpt_bundle(bundle);
    REQUIRE_FALSE(missing_results.ok);
    REQUIRE(missing_results.error
            == "ranked results file does not contain a results array");
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

TEST_CASE("excerpt find materializes bundle metadata, skipped inputs, and excerpt WAVs",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_installed_model_metadata(temp.path, checkpoint);

    auto input = temp.path / "Alpha Loop!.WAV";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));
    write_text(temp.path / "ignore.aiff", "unsupported");

    ExcerptFindRequest request;
    request.text = "Bright Kick ++";
    request.input_path = temp.path;
    request.model_id = "clap_music_audioset_v1";
    request.bundle_out = temp.path / "bundles";
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.query == "Bright Kick ++");
    REQUIRE(result.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(result.loaded_model_id == "clap_music_audioset_v1");
    REQUIRE(result.backend == "null");
    REQUIRE(result.resolved_checkpoint_path == checkpoint);
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE(result.skipped_files.size() == 1);
    REQUIRE(result.skipped_files[0].find("ignore.aiff") != std::string::npos);
    REQUIRE(result.results.size() == 1);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[0].source_file == input.string());
    REQUIRE(result.results[0].start_ms == Catch::Approx(0.0));
    REQUIRE(result.results[0].excerpt_file.find("alpha-loop") != std::string::npos);
    REQUIRE(fs::exists(result.bundle_path / result.results[0].excerpt_file));

    auto excerpt = pulp::audio::read_audio_file((result.bundle_path / result.results[0].excerpt_file).string());
    REQUIRE(excerpt.has_value());
    REQUIRE(excerpt->sample_rate == 48000);
    REQUIRE(excerpt->num_frames() == 48000);

    auto manifest = read_excerpt_bundle(result.bundle_path);
    REQUIRE(manifest.ok);
    REQUIRE(manifest.result_count == 1);
    REQUIRE(manifest.results.size() == 1);
    REQUIRE(manifest.results[0].excerpt_file == result.results[0].excerpt_file);

    std::ifstream query_file(result.bundle_path / "query.json");
    std::stringstream query_text;
    query_text << query_file.rdbuf();
    REQUIRE(query_text.str().find("\"text\": \"Bright Kick ++\"") != std::string::npos);

    std::ifstream inputs_file(result.bundle_path / "inputs.json");
    std::stringstream inputs_text;
    inputs_text << inputs_file.rdbuf();
    REQUIRE(inputs_text.str().find("ignore.aiff (unsupported; WAV only)") != std::string::npos);

    std::ifstream log_file(result.bundle_path / "logs" / "runtime.log");
    std::stringstream log_text;
    log_text << log_file.rdbuf();
    REQUIRE(log_text.str().find("backend=null") != std::string::npos);
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

TEST_CASE("excerpt find reports inactive and unavailable installed models",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto input = temp.path / "input.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.text = "texture";
    request.input_path = input;
    request.dry_run = true;

    auto inactive = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(inactive.ok);
    REQUIRE(inactive.error.find("no active audio model") != std::string::npos);

    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "active_model_id": "clap_music_audioset_v1"
}
)JSON");
    auto not_installed = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(not_installed.ok);
    REQUIRE(not_installed.error == "model is not installed: clap_music_audioset_v1");

    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "model_id": "clap_music_audioset_v1",
  "resolved_checkpoint_path": ")JSON" + (temp.path / "missing.pt").generic_string() + R"JSON("
}
)JSON");
    auto missing_checkpoint = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(missing_checkpoint.ok);
    REQUIRE(missing_checkpoint.error.find("installed model checkpoint does not exist")
            != std::string::npos);
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

TEST_CASE("excerpt find recursively scans sorted WAV inputs and caps final ranks",
          "[audio][tools][codecov]") {
    TempDir temp;
    install_active_clap_model(temp);

    fs::create_directories(temp.path / "nested");
    auto first = temp.path / "b.wav";
    auto nested = temp.path / "nested" / "a.wav";
    REQUIRE(pulp::audio::write_wav_file(first.string(), make_audio(48000, 96000)));
    REQUIRE(pulp::audio::write_wav_file(nested.string(), make_audio(48000, 96000)));

    ExcerptFindRequest request;
    request.text = "texture";
    request.input_path = temp.path;
    request.recursive = true;
    request.top_k = 2;
    request.window_ms = 500;
    request.hop_ms = 500;
    request.max_candidates_per_file = 3;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.scanned_file_count == 2);
    REQUIRE(result.results.size() == 2);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[1].rank == 2);
    REQUIRE(result.results[0].score >= result.results[1].score);
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

TEST_CASE("excerpt find skips WAV files that cannot produce excerpt windows",
          "[audio][tools][codecov]") {
    TempDir temp;
    install_active_clap_model(temp);

    auto short_file = temp.path / "short.wav";
    REQUIRE(pulp::audio::write_wav_file(short_file.string(), make_audio(48000, 24000)));

    ExcerptFindRequest request;
    request.text = "texture";
    request.input_path = short_file;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;
    request.top_k = 1;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE(result.results.empty());
    REQUIRE(result.skipped_files.size() == 1);
    REQUIRE(result.skipped_files[0].find("no excerpt windows") != std::string::npos);
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

TEST_CASE("excerpt find dry-run returns ranked metadata without bundle writes",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto checkpoint = install_active_clap_model(temp);
    auto input = temp.path / "dry-run.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 96000)));

    ExcerptFindRequest request;
    request.text = "dry run texture";
    request.input_path = input;
    request.top_k = 2;
    request.window_ms = 500;
    request.hop_ms = 500;
    request.max_candidates_per_file = 4;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.bundle_path.empty());
    REQUIRE(result.resolved_checkpoint_path == checkpoint);
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE(result.skipped_files.empty());
    REQUIRE(result.results.size() == 2);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[1].rank == 2);
    REQUIRE(result.results[0].source_file == input.string());
    REQUIRE(result.results[0].source_duration_ms == Catch::Approx(500.0));
    REQUIRE(result.results[0].end_ms > result.results[0].start_ms);
    REQUIRE(result.results[0].excerpt_file.empty());
}

TEST_CASE("excerpt find explicit model id overrides unrelated active state",
          "[audio][tools][codecov]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_installed_model_metadata(temp.path, checkpoint);
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "active_model_id": "unknown_active_model"
}
)JSON");
    auto input = temp.path / "explicit.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.text = "explicit model";
    request.input_path = input;
    request.model_id = "clap_music_audioset_v1";
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(result.loaded_model_id == "clap_music_audioset_v1");
    REQUIRE(result.resolved_checkpoint_path == checkpoint);
    REQUIRE(result.results.size() == 1);
}

TEST_CASE("excerpt find nonrecursive directories ignore nested WAV-only inputs",
          "[audio][tools][codecov]") {
    TempDir temp;
    install_active_clap_model(temp);
    fs::create_directories(temp.path / "nested");
    REQUIRE(pulp::audio::write_wav_file(
        (temp.path / "nested" / "only-nested.wav").string(),
        make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.text = "nested texture";
    request.input_path = temp.path;
    request.recursive = false;
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "no supported WAV inputs found");
    REQUIRE(result.scanned_file_count == 0);
    REQUIRE(result.results.empty());
    REQUIRE(result.skipped_files.empty());
}

TEST_CASE("excerpt find applies per-file candidate caps before global ranking",
          "[audio][tools][codecov]") {
    TempDir temp;
    install_active_clap_model(temp);
    auto first = temp.path / "first.wav";
    auto second = temp.path / "second.wav";
    REQUIRE(pulp::audio::write_wav_file(first.string(), make_audio(48000, 144000)));
    REQUIRE(pulp::audio::write_wav_file(second.string(), make_audio(48000, 144000)));

    ExcerptFindRequest request;
    request.text = "ranked texture";
    request.input_path = temp.path;
    request.top_k = 4;
    request.window_ms = 500;
    request.hop_ms = 500;
    request.max_candidates_per_file = 1;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.scanned_file_count == 2);
    REQUIRE(result.results.size() == 2);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[1].rank == 2);
    REQUIRE(result.results[0].score >= result.results[1].score);
    REQUIRE(result.results[0].source_file != result.results[1].source_file);
    REQUIRE(result.results[0].start_ms >= 0.0);
    REQUIRE(result.results[1].end_ms > result.results[1].start_ms);
}

TEST_CASE("excerpt find validates request guard fields before model resolution",
          "[audio][tools][issue-643]") {
    TempDir temp;
    ExcerptFindRequest request;
    request.input_path = temp.path;
    request.top_k = 1;
    request.max_candidates_per_file = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;

    auto empty_text = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(empty_text.ok);
    REQUIRE(empty_text.error == "text query is required");

    request.text = "query";
    request.input_path = fs::path{};
    auto empty_input = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(empty_input.ok);
    REQUIRE(empty_input.error == "input path is required");

    request.input_path = temp.path / "missing.wav";
    auto missing_input = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(missing_input.ok);
    REQUIRE(missing_input.error.find("input path does not exist") != std::string::npos);

    request.input_path = temp.path;
    request.top_k = 0;
    auto zero_top = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(zero_top.ok);
    REQUIRE(zero_top.error == "top and max_candidates_per_file must be >= 1");

    request.top_k = 1;
    request.max_candidates_per_file = 0;
    auto zero_candidates = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(zero_candidates.ok);
    REQUIRE(zero_candidates.error == "top and max_candidates_per_file must be >= 1");

    request.max_candidates_per_file = 1;
    request.window_ms = 0;
    auto zero_window = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(zero_window.ok);
    REQUIRE(zero_window.error == "window_ms and hop_ms must be >= 1");

    request.window_ms = 1000;
    request.hop_ms = 0;
    auto zero_hop = run_excerpt_find(request, temp.path);
    REQUIRE_FALSE(zero_hop.ok);
    REQUIRE(zero_hop.error == "window_ms and hop_ms must be >= 1");
}

TEST_CASE("excerpt find reports unsupported inputs after model resolution",
          "[audio][tools][issue-643]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_installed_model_metadata(temp.path, checkpoint);
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "active_model_id": "clap_music_audioset_v1"
}
)JSON");

    auto unsupported = temp.path / "notes.txt";
    write_text(unsupported, "not audio");

    ExcerptFindRequest request;
    request.text = "query";
    request.input_path = unsupported;
    request.top_k = 1;
    request.max_candidates_per_file = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(result.error == "no supported WAV inputs found");
    REQUIRE(result.scanned_file_count == 0);
    REQUIRE(result.skipped_files.size() == 1);
    REQUIRE(result.skipped_files[0].find("unsupported; WAV only") != std::string::npos);

    auto json = to_json(result);
    REQUIRE(json.find("\"ok\": false") != std::string::npos);
    REQUIRE(json.find("unsupported; WAV only") != std::string::npos);
}

TEST_CASE("excerpt find records unreadable WAV inputs without writing dry-run bundles",
          "[audio][tools][excerpt-service][coverage][requested]") {
    TempDir temp;
    auto checkpoint = install_active_clap_model(temp);
    auto corrupt_wav = temp.path / "corrupt.wav";
    write_text(corrupt_wav, "not a wav payload");

    ExcerptFindRequest request;
    request.text = "corrupt input";
    request.input_path = corrupt_wav;
    request.top_k = 2;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 2;
    request.dry_run = true;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.bundle_path.empty());
    REQUIRE(result.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(result.loaded_model_id == "clap_music_audioset_v1");
    REQUIRE(result.backend == "null");
    REQUIRE(result.resolved_checkpoint_path == checkpoint);
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE(result.results.empty());
    REQUIRE(result.skipped_files.size() == 1);
    REQUIRE(result.skipped_files[0].find("corrupt.wav") != std::string::npos);
    REQUIRE(result.skipped_files[0].find("failed to read") != std::string::npos);
}

TEST_CASE("excerpt find reports bundle directory creation failures after ranking",
          "[audio][tools][excerpt-service][coverage]") {
    TempDir temp;
    install_active_clap_model(temp);
    auto input = temp.path / "input.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));
    auto occupied = temp.path / "occupied";
    write_text(occupied, "not a directory");

    ExcerptFindRequest request;
    request.text = "bundle failure";
    request.input_path = input;
    request.bundle_out = occupied;
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("failed to create bundle directories") != std::string::npos);
    REQUIRE(result.bundle_path.string().find("occupied") != std::string::npos);
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE(result.results.size() == 1);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[0].excerpt_file.empty());
}

TEST_CASE("excerpt find JSON serializer preserves failure diagnostics",
          "[audio][tools][excerpt-service][coverage]") {
    ExcerptFindResult result;
    result.query = "missing model";
    result.requested_model_id = "clap_music_audioset_v1";
    result.backend = "null";
    result.scanned_file_count = 4;
    result.error = "model is not installed: clap_music_audioset_v1";
    result.skipped_files.push_back("notes.txt (unsupported; WAV only)");

    auto json = to_json(result);

    REQUIRE(json.find("\"ok\": false") != std::string::npos);
    REQUIRE(json.find("\"query\": \"missing model\"") != std::string::npos);
    REQUIRE(json.find("\"requested_model_id\": \"clap_music_audioset_v1\"")
            != std::string::npos);
    REQUIRE(json.find("\"backend\": \"null\"") != std::string::npos);
    REQUIRE(json.find("\"scanned_file_count\": 4") != std::string::npos);
    REQUIRE(json.find("\"results\": []") != std::string::npos);
    REQUIRE(json.find("notes.txt (unsupported; WAV only)") != std::string::npos);
    REQUIRE(json.find("model is not installed: clap_music_audioset_v1")
            != std::string::npos);
}

TEST_CASE("excerpt find JSON serializer preserves ranked bundle metadata",
          "[audio][tools][excerpt-service][coverage][requested]") {
    ExcerptFindResult result;
    result.ok = true;
    result.bundle_path = "/tmp/pulp-audio-bundle";
    result.query = "tight snare";
    result.requested_model_id = "clap_music_audioset_v1";
    result.loaded_model_id = "clap_music_audioset_v1";
    result.backend = "null";
    result.resolved_checkpoint_path = "/models/clap.pt";
    result.scanned_file_count = 2;

    BundleResult item;
    item.rank = 1;
    item.score = 0.875;
    item.source_file = "/audio/loop.wav";
    item.source_duration_ms = 250.0;
    item.start_ms = 1000.0;
    item.end_ms = 1250.0;
    item.excerpt_file = "excerpts/rank-01-loop-1.000-1.250.wav";
    result.results.push_back(item);
    result.skipped_files.push_back("/audio/readme.txt (unsupported; WAV only)");

    auto json = to_json(result);

    REQUIRE(json.find("\"ok\": true") != std::string::npos);
    REQUIRE(json.find("\"bundle_path\": \"/tmp/pulp-audio-bundle\"") != std::string::npos);
    REQUIRE(json.find("\"requested_model_id\": \"clap_music_audioset_v1\"")
            != std::string::npos);
    REQUIRE(json.find("\"loaded_model_id\": \"clap_music_audioset_v1\"")
            != std::string::npos);
    REQUIRE(json.find("\"resolved_checkpoint_path\": \"/models/clap.pt\"")
            != std::string::npos);
    REQUIRE(json.find("\"scanned_file_count\": 2") != std::string::npos);
    REQUIRE(json.find("\"rank\": 1") != std::string::npos);
    REQUIRE(json.find("\"score\": 0.875") != std::string::npos);
    REQUIRE(json.find("\"source_file\": \"/audio/loop.wav\"") != std::string::npos);
    REQUIRE(json.find("\"source_duration_ms\": 250") != std::string::npos);
    REQUIRE(json.find("\"start_ms\": 1000") != std::string::npos);
    REQUIRE(json.find("\"end_ms\": 1250") != std::string::npos);
    REQUIRE(json.find("\"excerpt_file\": \"excerpts/rank-01-loop-1.000-1.250.wav\"")
            != std::string::npos);
    REQUIRE(json.find("/audio/readme.txt (unsupported; WAV only)") != std::string::npos);
}

TEST_CASE("excerpt find bundle metadata falls back to registered model fields",
          "[audio][tools][excerpt-service][model-registry][coverage][requested]") {
    TempDir temp;
    auto checkpoint = temp.path / "models" / "clap.pt";
    write_text(checkpoint, "stub");
    write_text(temp.path / "audio" / "models" / "clap_music_audioset_v1.json", R"JSON({
  "backend": "clap",
  "resolved_checkpoint_path": ")JSON" + checkpoint.generic_string() + R"JSON("
}
)JSON");
    write_text(temp.path / "audio" / "model-state.json", R"JSON({
  "active_model_id": "clap_music_audioset_v1"
}
)JSON");

    auto input = temp.path / "fallback.wav";
    REQUIRE(pulp::audio::write_wav_file(input.string(), make_audio(48000, 48000)));

    ExcerptFindRequest request;
    request.text = "fallback metadata";
    request.input_path = input;
    request.bundle_out = temp.path / "bundles";
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(result.loaded_model_id == "clap_music_audioset_v1");
    REQUIRE(result.resolved_checkpoint_path == checkpoint);
    REQUIRE(result.results.size() == 1);

    auto bundle = read_excerpt_bundle(result.bundle_path);
    REQUIRE(bundle.ok);
    REQUIRE(bundle.requested_model_id == "clap_music_audioset_v1");
    REQUIRE(bundle.loaded_model_id == "clap_music_audioset_v1");
    REQUIRE(bundle.backend == "null");

    auto model_json = read_text(result.bundle_path / "model.json");
    REQUIRE(model_json.find("\"checkpoint_ref\": \"hf://lukewys/laion_clap/music.pt\"")
            != std::string::npos);
    const auto native_path = "\"resolved_checkpoint_path\": \""
                           + json_escape(checkpoint.string()) + "\"";
    const auto generic_path = "\"resolved_checkpoint_path\": \""
                            + json_escape(checkpoint.generic_string()) + "\"";
    REQUIRE((model_json.find(native_path) != std::string::npos
             || model_json.find(generic_path) != std::string::npos));
}

#if !defined(_WIN32)
TEST_CASE("excerpt find rejects special files before model resolution",
          "[audio][tools][excerpt-service][coverage][requested]") {
    TempDir temp;
    auto pipe_path = temp.path / "input.wav";
    REQUIRE(::mkfifo(pipe_path.c_str(), 0600) == 0);

    ExcerptFindRequest request;
    request.text = "special input";
    request.input_path = pipe_path;
    request.model_id = "not_a_model";

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("input path must be a file or directory") != std::string::npos);
    REQUIRE(result.error.find(pipe_path.string()) != std::string::npos);
    REQUIRE(result.requested_model_id.empty());
    REQUIRE(result.loaded_model_id.empty());
    REQUIRE(result.scanned_file_count == 0);
    REQUIRE(result.results.empty());
    REQUIRE(result.skipped_files.empty());
}
#endif

TEST_CASE("excerpt find writes stereo excerpt audio and manifest sidecars",
          "[audio][tools][excerpt-service][coverage][requested]") {
    TempDir temp;
    auto checkpoint = install_active_clap_model(temp);
    auto input = temp.path / "Stereo Loop!.wav";
    auto source = make_stereo_audio(48000, 48000);
    REQUIRE(pulp::audio::write_wav_file(input.string(), source));

    ExcerptFindRequest request;
    request.text = "stereo texture";
    request.input_path = input;
    request.bundle_out = temp.path / "bundles";
    request.top_k = 1;
    request.window_ms = 1000;
    request.hop_ms = 1000;
    request.max_candidates_per_file = 1;

    auto result = run_excerpt_find(request, temp.path);

    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(result.resolved_checkpoint_path == checkpoint);
    REQUIRE(result.scanned_file_count == 1);
    REQUIRE(result.skipped_files.empty());
    REQUIRE(result.results.size() == 1);
    REQUIRE(result.results[0].rank == 1);
    REQUIRE(result.results[0].source_file == input.string());
    REQUIRE(result.results[0].start_ms == Catch::Approx(0.0));
    REQUIRE(result.results[0].end_ms == Catch::Approx(1000.0));
    REQUIRE(result.results[0].source_duration_ms == Catch::Approx(1000.0));
    REQUIRE(result.results[0].excerpt_file.find("stereo-loop") != std::string::npos);

    auto excerpt_path = result.bundle_path / result.results[0].excerpt_file;
    auto excerpt = pulp::audio::read_audio_file(excerpt_path.string());
    REQUIRE(excerpt.has_value());
    REQUIRE(excerpt->sample_rate == 48000);
    REQUIRE(excerpt->channels.size() == 2);
    REQUIRE(excerpt->num_frames() == 48000);
    REQUIRE(excerpt->channels[0].front() == Catch::Approx(source.channels[0].front()).margin(0.0001f));
    REQUIRE(excerpt->channels[1].front() == Catch::Approx(source.channels[1].front()).margin(0.0001f));
    REQUIRE(excerpt->channels[0].back() == Catch::Approx(source.channels[0].back()).margin(0.0001f));
    REQUIRE(excerpt->channels[1].back() == Catch::Approx(source.channels[1].back()).margin(0.0001f));

    auto manifest = read_excerpt_bundle(result.bundle_path);
    REQUIRE(manifest.ok);
    REQUIRE(manifest.result_count == 1);
    REQUIRE(manifest.results.size() == 1);
    REQUIRE(manifest.results[0].source_file == input.string());
    REQUIRE(manifest.results[0].excerpt_file == result.results[0].excerpt_file);

    auto inputs_json = read_text(result.bundle_path / "inputs.json");
    REQUIRE(inputs_json.find("\"recursive\": false") != std::string::npos);
    REQUIRE(inputs_json.find(json_escape(input.string())) != std::string::npos);
    REQUIRE(inputs_json.find("\"skipped_files\": []") != std::string::npos);

    auto model_json = read_text(result.bundle_path / "model.json");
    REQUIRE(model_json.find("\"backend_package\": \"null_backend\"") != std::string::npos);
    REQUIRE(model_json.find("\"backend_package_version\": \"phase14\"") != std::string::npos);

    auto log = read_text(result.bundle_path / "logs" / "runtime.log");
    REQUIRE(log.find("requested_model_id=clap_music_audioset_v1") != std::string::npos);
    REQUIRE(log.find("loaded_model_id=clap_music_audioset_v1") != std::string::npos);
    REQUIRE(log.find("scanned_files=1") != std::string::npos);
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
