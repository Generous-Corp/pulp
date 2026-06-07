#include <pulp/runtime/model_store.hpp>

#include <pulp/runtime/model_download.hpp>
#include <pulp/runtime/system.hpp>

#include <choc/text/choc_JSON.h>

#include <fstream>
#include <initializer_list>
#include <sstream>

namespace fs = std::filesystem;

namespace pulp::runtime {

namespace {

std::string read_text_file(const fs::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool parse_json_file(const fs::path& path, choc::value::Value& value, std::string& error) {
    auto text = read_text_file(path);
    if (text.empty()) {
        error = "failed to read " + path.string();
        return false;
    }
    try {
        value = choc::json::parse(text);
        return true;
    } catch (const std::exception& e) {
        error = "failed to parse " + path.string() + ": " + e.what();
        return false;
    } catch (...) {
        error = "failed to parse " + path.string();
        return false;
    }
}

std::string object_string(const choc::value::ValueView& object,
                          std::initializer_list<const char*> keys) {
    if (!object.isObject()) return {};
    for (auto* key : keys) {
        if (!object.hasObjectMember(key)) continue;
        auto value = object[key];
        if (value.isVoid()) continue;
        auto text = std::string(value.toString());
        if (!text.empty()) return text;
    }
    return {};
}

void add_string_member(choc::value::Value& object, const char* key, const std::string& value) {
    object.addMember(key, choc::value::createString(value));
}

void add_path_member(choc::value::Value& object, const char* key, const fs::path& value) {
    add_string_member(object, key, value.string());
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

}  // namespace

fs::path resolve_pulp_home(const fs::path& override_path) {
    if (!override_path.empty()) return override_path;
    if (auto pulp_home = get_env("PULP_HOME")) return fs::path(*pulp_home);
    auto home = get_env("HOME");
#ifdef _WIN32
    if (!home) home = get_env("USERPROFILE");
#endif
    if (!home) return {};
    return fs::path(*home) / ".pulp";
}

fs::path model_state_path(std::string_view subsystem, const fs::path& pulp_home_override) {
    auto pulp_home = resolve_pulp_home(pulp_home_override);
    if (pulp_home.empty()) return {};
    return pulp_home / std::string(subsystem) / "model-state.json";
}

fs::path model_install_path(std::string_view subsystem, std::string_view model_id,
                            const fs::path& pulp_home_override) {
    auto pulp_home = resolve_pulp_home(pulp_home_override);
    if (pulp_home.empty()) return {};
    return pulp_home / std::string(subsystem) / "models" / (std::string(model_id) + ".json");
}

InstalledModelRecord read_installed_model(std::string_view subsystem, std::string_view model_id,
                                          const fs::path& pulp_home_override) {
    InstalledModelRecord record;
    record.metadata_path = model_install_path(subsystem, model_id, pulp_home_override);
    record.model_id = std::string(model_id);

    if (record.metadata_path.empty() || !fs::exists(record.metadata_path)) return record;
    record.metadata_found = true;

    choc::value::Value value;
    std::string error;
    if (!parse_json_file(record.metadata_path, value, error)) return record;

    auto root = value.getView();
    auto parsed_model_id = object_string(root, {"model_id"});
    if (!parsed_model_id.empty()) record.model_id = parsed_model_id;
    record.backend = object_string(root, {"backend"});
    record.checkpoint_ref = object_string(root, {"checkpoint_ref"});

    auto checkpoint = object_string(root, {"resolved_checkpoint_path", "checkpoint_path"});
    if (!checkpoint.empty()) {
        record.resolved_checkpoint_path = fs::path(checkpoint);
        record.checkpoint_exists = fs::exists(record.resolved_checkpoint_path);
    }
    return record;
}

std::string read_active_model_id(std::string_view subsystem, const fs::path& pulp_home_override) {
    auto state_path = model_state_path(subsystem, pulp_home_override);
    if (state_path.empty() || !fs::exists(state_path)) return {};

    choc::value::Value value;
    std::string error;
    if (!parse_json_file(state_path, value, error)) return {};

    return object_string(value.getView(),
                         {"active_model_id", "configured_model_id", "requested_model_id", "model_id"});
}

ModelListResult list_models(const std::vector<ModelEntry>& registry, std::string_view subsystem,
                            const fs::path& pulp_home_override) {
    ModelListResult result;
    result.pulp_home = resolve_pulp_home(pulp_home_override);
    if (result.pulp_home.empty()) {
        result.error = "unable to resolve PULP_HOME";
        return result;
    }

    result.active_model_id = read_active_model_id(subsystem, result.pulp_home);
    for (const auto& model : registry) {
        ListedModel listed;
        listed.model = model;
        listed.active = (model.model_id == result.active_model_id);

        auto installed = read_installed_model(subsystem, model.model_id, result.pulp_home);
        if (installed.metadata_found) {
            listed.status = installed.checkpoint_exists ? "installed" : "missing_checkpoint";
            listed.resolved_checkpoint_path = installed.resolved_checkpoint_path;
        } else {
            // Not installed — surface a resumable partial (an interrupted/cancelled .part).
            const fs::path dir = result.pulp_home / std::string(subsystem) / "models" / model.model_id;
            std::error_code ec;
            if (fs::exists(dir, ec)) {
                for (const auto& entry : fs::directory_iterator(dir, ec)) {
                    if (entry.path().extension() != ".part") continue;
                    listed.status = "partial";
                    if (model.size_bytes > 0) {
                        const auto got = static_cast<float>(fs::file_size(entry.path(), ec));
                        listed.partial_fraction = got / static_cast<float>(model.size_bytes);
                    }
                    break;
                }
            }
        }
        result.models.push_back(std::move(listed));
    }
    return result;
}

ActivateModelResult activate_model(const std::vector<ModelEntry>& registry, std::string_view subsystem,
                                   std::string_view model_id, const fs::path& pulp_home_override) {
    ActivateModelResult result;
    result.state_path = model_state_path(subsystem, pulp_home_override);
    if (result.state_path.empty()) {
        result.error = "unable to resolve PULP_HOME";
        return result;
    }

    auto* registered = find_model(registry, model_id);
    if (!registered) {
        result.error = "unknown model_id: " + std::string(model_id);
        return result;
    }

    auto installed = read_installed_model(subsystem, model_id, pulp_home_override);
    if (!installed.metadata_found) {
        result.error = "model is not installed: " + std::string(model_id);
        return result;
    }
    if (!installed.checkpoint_exists) {
        result.error = "installed model checkpoint does not exist: " +
                       installed.resolved_checkpoint_path.string();
        return result;
    }

    auto object = choc::value::createObject("");
    add_string_member(object, "active_model_id", registered->model_id);
    add_string_member(object, "configured_model_id", registered->model_id);
    add_string_member(object, "backend", installed.backend.empty() ? registered->backend : installed.backend);
    add_string_member(object, "checkpoint_ref",
                      installed.checkpoint_ref.empty() ? registered->checkpoint_ref : installed.checkpoint_ref);
    add_path_member(object, "resolved_checkpoint_path", installed.resolved_checkpoint_path);

    std::string error;
    if (!write_text_file(result.state_path, choc::json::toString(object, true), error)) {
        result.error = error;
        return result;
    }

    result.ok = true;
    result.active_model_id = registered->model_id;
    result.backend = installed.backend.empty() ? registered->backend : installed.backend;
    result.checkpoint_ref = installed.checkpoint_ref.empty() ? registered->checkpoint_ref : installed.checkpoint_ref;
    result.resolved_checkpoint_path = installed.resolved_checkpoint_path;
    return result;
}

std::string to_json(const ModelListResult& result) {
    auto object = choc::value::createObject("");
    add_path_member(object, "pulp_home", result.pulp_home);
    add_string_member(object, "active_model_id", result.active_model_id);

    auto models = choc::value::createEmptyArray();
    for (const auto& item : result.models) {
        auto value = choc::value::createObject("");
        add_string_member(value, "model_id", item.model.model_id);
        add_string_member(value, "display_name", item.model.display_name);
        add_string_member(value, "backend", item.model.backend);
        add_string_member(value, "checkpoint_ref", item.model.checkpoint_ref);
        auto tags = choc::value::createEmptyArray();
        for (const auto& tag : item.model.task_tags)
            tags.addArrayElement(choc::value::createString(tag));
        value.addMember("task_tags", tags);
        value.addMember("size_bytes", choc::value::createInt64(static_cast<int64_t>(item.model.size_bytes)));
        add_string_member(value, "status", item.status);
        value.addMember("active", choc::value::createBool(item.active));
        add_path_member(value, "resolved_checkpoint_path", item.resolved_checkpoint_path);
        models.addArrayElement(value);
    }
    object.addMember("models", models);
    add_string_member(object, "error", result.error);
    return choc::json::toString(object, true);
}

std::string to_json(const ActivateModelResult& result) {
    auto object = choc::value::createObject("");
    object.addMember("ok", choc::value::createBool(result.ok));
    add_path_member(object, "state_path", result.state_path);
    add_string_member(object, "active_model_id", result.active_model_id);
    add_string_member(object, "backend", result.backend);
    add_string_member(object, "checkpoint_ref", result.checkpoint_ref);
    add_path_member(object, "resolved_checkpoint_path", result.resolved_checkpoint_path);
    add_string_member(object, "error", result.error);
    return choc::json::toString(object, true);
}

InstallModelResult install_model(const ModelEntry& model, std::string_view subsystem,
                                 const std::function<bool(const DownloadProgress&)>& on_progress,
                                 const CancellationToken* cancel,
                                 const std::vector<std::pair<std::string, std::string>>& headers,
                                 const fs::path& pulp_home_override) {
    InstallModelResult r;
    const auto home = resolve_pulp_home(pulp_home_override);
    if (home.empty()) {
        r.error = "unable to resolve PULP_HOME";
        return r;
    }
    const fs::path files_dir = home / std::string(subsystem) / "models" / model.model_id;

    // A model is either a single primary checkpoint or a multi-asset bundle (e.g. Magenta's
    // weights + state). Fetch EVERY asset so the model is complete on disk — a half-downloaded
    // bundle (weights only) silently fails to load. Falls back to the primary checkpoint when
    // assets[] is empty.
    struct Item {
        std::string url, role, sha;
    };
    std::vector<Item> items;
    if (!model.assets.empty()) {
        for (const auto& a : model.assets) {
            const std::string u = (a.checkpoint_ref.rfind("http", 0) == 0)
                                      ? a.checkpoint_ref
                                      : resolve_checkpoint_url(a.checkpoint_ref);
            if (u.empty()) {
                r.error = "cannot resolve a download URL for asset '" + a.role + "' of " + model.model_id;
                return r;
            }
            items.push_back({u, a.role.empty() ? "asset" : a.role, ""});
        }
    } else {
        const std::string u =
            model.download_url.empty() ? resolve_checkpoint_url(model.checkpoint_ref) : model.download_url;
        if (u.empty()) {
            r.error = "cannot resolve a download URL for " + model.model_id;
            return r;
        }
        items.push_back({u, "primary", model.sha256});
    }

    auto filename_of = [](const std::string& url, const std::string& fallback) {
        std::string fname;
        if (auto slash = url.find_last_of('/'); slash != std::string::npos) fname = url.substr(slash + 1);
        if (auto q = fname.find('?'); q != std::string::npos) fname = fname.substr(0, q);
        return fname.empty() ? fallback : fname;
    };

    const int n = static_cast<int>(items.size());
    auto assets_obj = choc::value::createEmptyArray();
    fs::path primary_path;  // the first/weights asset — what the engine loads

    for (int i = 0; i < n; ++i) {
        const auto& it = items[static_cast<size_t>(i)];
        const fs::path dest = files_dir / filename_of(it.url, model.model_id + ".bin");

        DownloadRequest req;
        req.url = it.url;
        req.dest = dest;
        req.expected_sha256 = it.sha;
        req.resume = true;
        for (const auto& h : headers) req.headers.push_back(HttpHeader{h.first, h.second});

        // Aggregate progress so the UI sees one 0→100% bar across all assets; asset i
        // contributes 1/n of the whole.
        const int idx = i;
        // download_file() enforces cancellation via `cancel`; this wrapper only reshapes the
        // progress into one aggregate 0→100% bar across all assets.
        auto wrapped = [&on_progress, idx, n](const DownloadProgress& p) -> bool {
            if (!on_progress) return true;
            DownloadProgress agg = p;
            if (p.total > 0) {
                const double frac = (static_cast<double>(idx) +
                                     static_cast<double>(p.downloaded) / static_cast<double>(p.total)) /
                                    static_cast<double>(n);
                agg.downloaded = static_cast<std::uint64_t>(frac * 1'000'000.0);
                agg.total = 1'000'000;
            }
            return on_progress(agg);
        };

        const auto dl = download_file(req, wrapped, cancel);
        if (dl.cancelled) {
            r.cancelled = true;
            r.error = "cancelled";
            return r;
        }
        if (!dl.ok) {
            r.error = dl.error;
            return r;
        }

        if (i == 0) {
            primary_path = dest;
            r.sha256 = dl.sha256;
        }
        auto a = choc::value::createObject("");
        add_string_member(a, "role", it.role);
        add_path_member(a, "path", dest);
        add_string_member(a, "sha256", dl.sha256);
        assets_obj.addArrayElement(a);
    }

    r.checkpoint_path = primary_path;
    r.metadata_path = model_install_path(subsystem, model.model_id, pulp_home_override);

    auto object = choc::value::createObject("");
    add_string_member(object, "model_id", model.model_id);
    add_string_member(object, "backend", model.backend);
    add_string_member(object, "checkpoint_ref", model.checkpoint_ref);
    add_path_member(object, "resolved_checkpoint_path", primary_path);
    add_string_member(object, "sha256", r.sha256);
    object.addMember("assets", assets_obj);

    std::string error;
    if (!write_text_file(r.metadata_path, choc::json::toString(object, true), error)) {
        r.error = error;
        return r;
    }
    r.ok = true;
    return r;
}

bool remove_model(std::string_view subsystem, std::string_view model_id, std::string& error,
                  const fs::path& pulp_home_override) {
    const auto home = resolve_pulp_home(pulp_home_override);
    if (home.empty()) {
        error = "unable to resolve PULP_HOME";
        return false;
    }
    std::error_code ec;
    fs::remove_all(home / std::string(subsystem) / "models" / std::string(model_id), ec);
    fs::remove(model_install_path(subsystem, model_id, pulp_home_override), ec);
    // Clear the active selection if it pointed at the removed model.
    if (read_active_model_id(subsystem, pulp_home_override) == model_id) {
        fs::remove(model_state_path(subsystem, pulp_home_override), ec);
    }
    return true;
}

}  // namespace pulp::runtime
