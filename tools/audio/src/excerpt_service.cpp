#include <pulp/tools/audio/excerpt_service.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/excerpt_types.hpp>
#include <pulp/audio/window_enumerator.hpp>
#include <pulp/tools/audio/model_store.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace pulp::tools::audio {

namespace {

struct ResolvedModel {
    bool ok = false;
    std::string requested_model_id;
    std::string loaded_model_id;
    std::string backend;
    std::string checkpoint_ref;
    fs::path resolved_checkpoint_path;
    std::string error;
};

struct Candidate {
    pulp::audio::ExcerptWindow window;
    double score = 0.0;
};

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_supported_audio_path(const fs::path& path) {
    return to_lower(path.extension().string()) == ".wav";
}

std::string slugify(std::string text) {
    for (char& c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            c = '-';
        }
    }

    std::string collapsed;
    bool last_dash = false;
    for (char c : text) {
        if (c == '-') {
            if (last_dash) continue;
            last_dash = true;
        } else {
            last_dash = false;
        }
        collapsed += c;
    }

    while (!collapsed.empty() && collapsed.front() == '-') collapsed.erase(collapsed.begin());
    while (!collapsed.empty() && collapsed.back() == '-') collapsed.pop_back();
    return collapsed.empty() ? "query" : collapsed;
}

std::string timestamp_slug() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
    return buffer;
}

std::string iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
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

bool write_text_file(const fs::path& path, const std::string& content, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "failed to create " + path.parent_path().string() + ": " + ec.message();
        return false;
    }

    std::ofstream output(path);
    if (!output.is_open()) {
        error = "failed to open " + path.string() + " for writing";
        return false;
    }

    output << content;
    if (!output.good()) {
        error = "failed to write " + path.string();
        return false;
    }

    return true;
}

void add_string_member(choc::value::Value& object, const char* key, const std::string& value) {
    object.addMember(key, choc::value::createString(value));
}

void add_path_member(choc::value::Value& object, const char* key, const fs::path& value) {
    add_string_member(object, key, value.string());
}

fs::path default_bundle_root() {
    return fs::current_path() / "audio-runs";
}

fs::path make_bundle_path(const ExcerptFindRequest& request) {
    auto base = request.bundle_out.empty() ? default_bundle_root() : request.bundle_out;
    auto stem = timestamp_slug() + "-excerpt-find-" + slugify(request.text);
    auto bundle = base / stem;
    for (int suffix = 1; fs::exists(bundle); ++suffix)
        bundle = base / (stem + "-" + std::to_string(suffix));
    return bundle;
}

ResolvedModel resolve_model(const ExcerptFindRequest& request, const fs::path& pulp_home_override) {
    ResolvedModel model;
    model.requested_model_id = request.model_id.empty()
                             ? read_active_model_id(pulp_home_override)
                             : request.model_id;
    if (model.requested_model_id.empty()) {
        model.error = "no active audio model; run `pulp audio model activate <model-id>` or pass --model";
        return model;
    }

    auto* registered = find_registered_model(model.requested_model_id);
    if (!registered) {
        model.error = "unknown model_id: " + model.requested_model_id;
        return model;
    }

    auto installed = read_installed_model(model.requested_model_id, pulp_home_override);
    if (!installed.metadata_found) {
        model.error = "model is not installed: " + model.requested_model_id;
        return model;
    }
    if (!installed.checkpoint_exists) {
        model.error = "installed model checkpoint does not exist: " + installed.resolved_checkpoint_path.string();
        return model;
    }

    model.ok = true;
    model.loaded_model_id = installed.model_id.empty() ? registered->model_id : installed.model_id;
    model.backend = "null";
    model.checkpoint_ref = installed.checkpoint_ref.empty() ? registered->checkpoint_ref : installed.checkpoint_ref;
    model.resolved_checkpoint_path = installed.resolved_checkpoint_path;
    return model;
}

std::vector<fs::path> collect_input_files(const fs::path& input_path,
                                          bool recursive,
                                          std::vector<std::string>& skipped) {
    std::vector<fs::path> files;

    if (fs::is_regular_file(input_path)) {
        if (is_supported_audio_path(input_path)) files.push_back(input_path);
        else skipped.push_back(input_path.string() + " (unsupported; WAV only)");
        return files;
    }

    auto handle_entry = [&](const fs::directory_entry& entry) {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec)) {
            if (entry_ec)
                skipped.push_back(entry.path().string() + " (" + entry_ec.message() + ")");
            return;
        }
        if (is_supported_audio_path(entry.path())) files.push_back(entry.path());
        else skipped.push_back(entry.path().string() + " (unsupported; WAV only)");
    };

    if (recursive) {
        std::error_code iter_ec;
        auto options = fs::directory_options::skip_permission_denied;
        for (fs::recursive_directory_iterator it(input_path, options, iter_ec), end;
             it != end;
             it.increment(iter_ec)) {
            if (iter_ec) {
                skipped.push_back(input_path.string() + " (" + iter_ec.message() + ")");
                iter_ec.clear();
                continue;
            }
            handle_entry(*it);
        }
    } else {
        std::error_code iter_ec;
        for (fs::directory_iterator it(input_path, iter_ec), end; it != end; it.increment(iter_ec)) {
            if (iter_ec) {
                skipped.push_back(input_path.string() + " (" + iter_ec.message() + ")");
                iter_ec.clear();
                continue;
            }
            handle_entry(*it);
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

double deterministic_score(const std::string& query_text, const pulp::audio::ExcerptWindow& window) {
    auto key = query_text + "|" + window.source_path + "|" + std::to_string(window.start_frame)
             + "|" + std::to_string(window.frame_count);
    auto hash = stable_hash64(key);
    return static_cast<double>(hash % 1000000ULL) / 1000000.0;
}

pulp::audio::AudioFileData slice_excerpt(const pulp::audio::AudioFileData& source,
                                         uint64_t start_frame,
                                         uint64_t frame_count) {
    pulp::audio::AudioFileData excerpt;
    excerpt.sample_rate = source.sample_rate;
    excerpt.channels.resize(source.channels.size());

    auto start = static_cast<std::size_t>(start_frame);
    auto count = static_cast<std::size_t>(frame_count);
    for (std::size_t channel = 0; channel < source.channels.size(); ++channel) {
        auto& dst = excerpt.channels[channel];
        const auto& src = source.channels[channel];
        dst.assign(src.begin() + static_cast<std::ptrdiff_t>(start),
                   src.begin() + static_cast<std::ptrdiff_t>(start + count));
    }
    return excerpt;
}

std::string excerpt_filename(const BundleResult& item) {
    std::ostringstream name;
    name << "rank-";
    name.fill('0');
    name.width(2);
    name << item.rank << "-";
    name << slugify(fs::path(item.source_file).stem().string()) << "-";
    name << std::fixed << std::setprecision(3) << (item.start_ms / 1000.0) << "-";
    name << std::fixed << std::setprecision(3) << (item.end_ms / 1000.0) << ".wav";
    return name.str();
}

bool write_bundle_files(const ExcerptFindRequest& request,
                        const ResolvedModel& model,
                        const std::vector<fs::path>& scanned_files,
                        const std::vector<std::string>& skipped_files,
                        ExcerptFindResult& result) {
    std::string error;
    std::error_code ec;
    fs::create_directories(result.bundle_path / "logs", ec);
    fs::create_directories(result.bundle_path / "excerpts", ec);
    if (ec) {
        result.error = "failed to create bundle directories: " + ec.message();
        return false;
    }

    auto manifest = choc::value::createObject("");
    add_string_member(manifest, "tool", "pulp audio excerpt-find");
    manifest.addMember("bundle_version", choc::value::createInt64(1));
    add_string_member(manifest, "created_at", iso_timestamp());
    add_string_member(manifest, "query_file", "query.json");
    add_string_member(manifest, "model_file", "model.json");
    add_string_member(manifest, "inputs_file", "inputs.json");
    add_string_member(manifest, "ranked_results_file", "ranked_results.json");
    add_string_member(manifest, "log_file", "logs/runtime.log");
    add_string_member(manifest, "excerpt_dir", "excerpts");
    add_string_member(manifest, "requested_model_id", result.requested_model_id);
    add_string_member(manifest, "loaded_model_id", result.loaded_model_id);
    add_string_member(manifest, "backend", result.backend);
    manifest.addMember("result_count", choc::value::createInt64(static_cast<int64_t>(result.results.size())));

    auto query = choc::value::createObject("");
    add_string_member(query, "text", request.text);
    query.addMember("window_ms", choc::value::createInt64(static_cast<int64_t>(request.window_ms)));
    query.addMember("hop_ms", choc::value::createInt64(static_cast<int64_t>(request.hop_ms)));
    query.addMember("top", choc::value::createInt64(static_cast<int64_t>(request.top_k)));
    query.addMember("min_score", choc::value::createFloat64(request.min_score));
    query.addMember("max_candidates_per_file",
                    choc::value::createInt64(static_cast<int64_t>(request.max_candidates_per_file)));

    auto model_json = choc::value::createObject("");
    add_string_member(model_json, "requested_model_id", result.requested_model_id);
    add_string_member(model_json, "loaded_model_id", result.loaded_model_id);
    add_string_member(model_json, "backend", result.backend);
    add_string_member(model_json, "checkpoint_ref", model.checkpoint_ref);
    add_path_member(model_json, "resolved_checkpoint_path", result.resolved_checkpoint_path);
    auto runtime_version = choc::value::createObject("");
    add_string_member(runtime_version, "backend_package", "null_backend");
    add_string_member(runtime_version, "backend_package_version", "phase14");
    model_json.addMember("runtime_version", runtime_version);

    auto inputs_json = choc::value::createObject("");
    add_path_member(inputs_json, "input_path", request.input_path);
    inputs_json.addMember("recursive", choc::value::createBool(request.recursive));
    auto scanned = choc::value::createEmptyArray();
    for (const auto& file : scanned_files)
        scanned.addArrayElement(choc::value::createString(file.string()));
    inputs_json.addMember("scanned_files", scanned);
    auto skipped = choc::value::createEmptyArray();
    for (const auto& item : skipped_files)
        skipped.addArrayElement(choc::value::createString(item));
    inputs_json.addMember("skipped_files", skipped);

    auto ranked = choc::value::createObject("");
    add_string_member(ranked, "query", result.query);
    auto ranked_results = choc::value::createEmptyArray();
    for (const auto& item : result.results) {
        auto value = choc::value::createObject("");
        value.addMember("rank", choc::value::createInt64(item.rank));
        value.addMember("score", choc::value::createFloat64(item.score));
        add_string_member(value, "source_file", item.source_file);
        value.addMember("source_duration_ms", choc::value::createFloat64(item.source_duration_ms));
        value.addMember("start_ms", choc::value::createFloat64(item.start_ms));
        value.addMember("end_ms", choc::value::createFloat64(item.end_ms));
        add_string_member(value, "excerpt_file", item.excerpt_file);
        ranked_results.addArrayElement(value);
    }
    ranked.addMember("results", ranked_results);

    auto log = std::string("backend=null\n")
             + "mode=deterministic-stub\n"
             + "wav_first=true\n"
             + "requested_model_id=" + result.requested_model_id + "\n"
             + "loaded_model_id=" + result.loaded_model_id + "\n"
             + "scanned_files=" + std::to_string(scanned_files.size()) + "\n";

    if (!write_text_file(result.bundle_path / "manifest.json", choc::json::toString(manifest, true), error)
        || !write_text_file(result.bundle_path / "query.json", choc::json::toString(query, true), error)
        || !write_text_file(result.bundle_path / "model.json", choc::json::toString(model_json, true), error)
        || !write_text_file(result.bundle_path / "inputs.json", choc::json::toString(inputs_json, true), error)
        || !write_text_file(result.bundle_path / "ranked_results.json", choc::json::toString(ranked, true), error)
        || !write_text_file(result.bundle_path / "logs" / "runtime.log", log, error)) {
        result.error = error;
        return false;
    }

    return true;
}

} // namespace

ExcerptFindResult run_excerpt_find(const ExcerptFindRequest& request,
                                   const fs::path& pulp_home_override) {
    ExcerptFindResult result;
    result.query = request.text;

    if (request.text.empty()) {
        result.error = "text query is required";
        return result;
    }
    if (request.input_path.empty()) {
        result.error = "input path is required";
        return result;
    }
    if (!fs::exists(request.input_path)) {
        result.error = "input path does not exist: " + request.input_path.string();
        return result;
    }
    if (!fs::is_regular_file(request.input_path) && !fs::is_directory(request.input_path)) {
        result.error = "input path must be a file or directory: " + request.input_path.string();
        return result;
    }
    if (request.top_k == 0 || request.max_candidates_per_file == 0) {
        result.error = "top and max_candidates_per_file must be >= 1";
        return result;
    }
    if (request.window_ms == 0 || request.hop_ms == 0) {
        result.error = "window_ms and hop_ms must be >= 1";
        return result;
    }

    auto model = resolve_model(request, pulp_home_override);
    if (!model.ok) {
        result.error = model.error;
        return result;
    }
    result.requested_model_id = model.requested_model_id;
    result.loaded_model_id = model.loaded_model_id;
    result.backend = model.backend;
    result.resolved_checkpoint_path = model.resolved_checkpoint_path;

    auto files = collect_input_files(request.input_path, request.recursive, result.skipped_files);
    result.scanned_file_count = files.size();
    if (files.empty()) {
        result.error = "no supported WAV inputs found";
        return result;
    }

    std::vector<Candidate> ranked;
    for (const auto& file : files) {
        auto audio = pulp::audio::read_audio_file(file.string());
        if (!audio) {
            result.skipped_files.push_back(file.string() + " (failed to read)");
            continue;
        }

        pulp::audio::ExcerptQuery query;
        query.text = request.text;
        query.top_k = request.top_k;
        query.min_score = static_cast<float>(request.min_score);
        query.window_frames = std::max<uint64_t>(1, static_cast<uint64_t>(
            std::llround((static_cast<double>(audio->sample_rate) * static_cast<double>(request.window_ms)) / 1000.0)));
        query.hop_frames = std::max<uint64_t>(1, static_cast<uint64_t>(
            std::llround((static_cast<double>(audio->sample_rate) * static_cast<double>(request.hop_ms)) / 1000.0)));

        auto windows = pulp::audio::enumerate_excerpt_windows(file.string(), *audio, query);
        if (windows.status != pulp::audio::WindowEnumerationStatus::ok) {
            result.skipped_files.push_back(file.string() + " (no excerpt windows)");
            continue;
        }

        std::vector<Candidate> file_candidates;
        file_candidates.reserve(windows.windows.size());
        for (const auto& window : windows.windows) {
            auto score = deterministic_score(request.text, window);
            if (score < request.min_score) continue;
            file_candidates.push_back({window, score});
        }

        std::sort(file_candidates.begin(), file_candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.window.source_path != b.window.source_path) return a.window.source_path < b.window.source_path;
            return a.window.start_frame < b.window.start_frame;
        });

        if (file_candidates.size() > request.max_candidates_per_file)
            file_candidates.resize(request.max_candidates_per_file);

        ranked.insert(ranked.end(), file_candidates.begin(), file_candidates.end());
    }

    std::sort(ranked.begin(), ranked.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.window.source_path != b.window.source_path) return a.window.source_path < b.window.source_path;
        return a.window.start_frame < b.window.start_frame;
    });

    if (ranked.size() > request.top_k)
        ranked.resize(request.top_k);

    for (std::size_t i = 0; i < ranked.size(); ++i) {
        const auto& candidate = ranked[i];
        BundleResult item;
        item.rank = static_cast<int>(i + 1);
        item.score = candidate.score;
        item.source_file = candidate.window.source_path;
        item.source_duration_ms = candidate.window.duration_seconds() * 1000.0;
        item.start_ms = candidate.window.start_seconds() * 1000.0;
        item.end_ms = static_cast<double>(candidate.window.end_frame()) * 1000.0
                    / static_cast<double>(candidate.window.sample_rate);
        result.results.push_back(std::move(item));
    }

    if (!request.dry_run) {
        result.bundle_path = make_bundle_path(request);
        std::error_code ec;
        fs::create_directories(result.bundle_path / "excerpts", ec);
        fs::create_directories(result.bundle_path / "logs", ec);
        if (ec) {
            result.error = "failed to create bundle directories: " + ec.message();
            return result;
        }

        for (auto& item : result.results) {
            item.excerpt_file = (fs::path("excerpts") / excerpt_filename(item)).string();
            auto audio = pulp::audio::read_audio_file(item.source_file);
            if (!audio) {
                result.error = "failed to reload source audio for excerpt export: " + item.source_file;
                return result;
            }
            auto start_frame = static_cast<uint64_t>(std::llround(item.start_ms * audio->sample_rate / 1000.0));
            auto end_frame = static_cast<uint64_t>(std::llround(item.end_ms * audio->sample_rate / 1000.0));
            // The window was found on an earlier read of this path; the file
            // may have shrunk (or emptied) since, so bound the slice by the
            // frames actually decoded now rather than indexing past them.
            if (audio->empty() || end_frame < start_frame || end_frame > audio->num_frames()) {
                result.error =
                    "source audio no longer covers the excerpt window: " + item.source_file;
                return result;
            }
            auto excerpt = slice_excerpt(*audio, start_frame, end_frame - start_frame);
            if (!pulp::audio::write_wav_file((result.bundle_path / item.excerpt_file).string(), excerpt)) {
                result.error = "failed to write excerpt file: " + (result.bundle_path / item.excerpt_file).string();
                return result;
            }
        }

        if (!write_bundle_files(request, model, files, result.skipped_files, result))
            return result;
    }

    result.ok = true;
    return result;
}

std::string to_json(const ExcerptFindResult& result) {
    auto object = choc::value::createObject("");
    object.addMember("ok", choc::value::createBool(result.ok));
    add_path_member(object, "bundle_path", result.bundle_path);
    add_string_member(object, "query", result.query);
    add_string_member(object, "requested_model_id", result.requested_model_id);
    add_string_member(object, "loaded_model_id", result.loaded_model_id);
    add_string_member(object, "backend", result.backend);
    add_path_member(object, "resolved_checkpoint_path", result.resolved_checkpoint_path);
    object.addMember("scanned_file_count", choc::value::createInt64(static_cast<int64_t>(result.scanned_file_count)));

    auto results = choc::value::createEmptyArray();
    for (const auto& item : result.results) {
        auto value = choc::value::createObject("");
        value.addMember("rank", choc::value::createInt64(item.rank));
        value.addMember("score", choc::value::createFloat64(item.score));
        add_string_member(value, "source_file", item.source_file);
        value.addMember("source_duration_ms", choc::value::createFloat64(item.source_duration_ms));
        value.addMember("start_ms", choc::value::createFloat64(item.start_ms));
        value.addMember("end_ms", choc::value::createFloat64(item.end_ms));
        add_string_member(value, "excerpt_file", item.excerpt_file);
        results.addArrayElement(value);
    }
    object.addMember("results", results);

    auto skipped = choc::value::createEmptyArray();
    for (const auto& item : result.skipped_files)
        skipped.addArrayElement(choc::value::createString(item));
    object.addMember("skipped_files", skipped);
    add_string_member(object, "error", result.error);
    return choc::json::toString(object, true);
}

} // namespace pulp::tools::audio
