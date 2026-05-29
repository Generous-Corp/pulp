#include <pulp/tools/audio/model_registry.hpp>

namespace pulp::tools::audio {

namespace {

bool has_control_byte(std::string_view text) {
    for (unsigned char ch : text) {
        if (ch < 0x20) return true;
    }
    return false;
}

bool has_dot_dot_segment(std::string_view path) {
    std::size_t start = 0;
    while (start <= path.size()) {
        auto slash = path.find('/', start);
        auto seg = path.substr(start,
            slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (seg == "..") return true;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return false;
}

} // namespace

const std::vector<RegisteredModel>& registered_models() {
    static const std::vector<RegisteredModel> models = {{
        .model_id = "clap_music_audioset_v1",
        .display_name = "CLAP Music AudioSet",
        .backend = "clap",
        .checkpoint_ref = "hf://lukewys/laion_clap/music.pt",
        .task_tags = {"music", "excerpt_find"},
        .size_bytes = 2523293286ULL,
        .download_url = "https://huggingface.co/lukewys/laion_clap/resolve/main/music.pt",
        .sha256 = "",  // Populated after first verified download
        .auto_downloadable = true,
    }};
    return models;
}

const RegisteredModel* find_registered_model(std::string_view model_id) {
    for (const auto& model : registered_models()) {
        if (model.model_id == model_id) return &model;
    }
    return nullptr;
}

std::string resolve_checkpoint_url(const std::string& checkpoint_ref) {
    // Handle hf:// protocol: hf://user/repo/path/to/file → https://huggingface.co/user/repo/resolve/main/path/to/file
    if (checkpoint_ref.starts_with("hf://")) {
        auto path = checkpoint_ref.substr(5);  // after "hf://"
        auto first_slash = path.find('/');
        if (first_slash == std::string::npos) return {};
        auto second_slash = path.find('/', first_slash + 1);
        if (second_slash == std::string::npos) return {};
        if (first_slash == 0 || second_slash == first_slash + 1) return {};
        auto user_repo = path.substr(0, second_slash);   // "user/repo"
        auto file_path = path.substr(second_slash + 1);  // "path/to/file"
        if (file_path.empty()) return {};
        if (has_control_byte(user_repo)) return {};
        if (file_path.starts_with('/') || has_control_byte(file_path)) return {};
        // Reject any `..` path segment. HTTP technically does not normalize
        // `..` in URL paths, but proxies, CDNs, and redirect handlers
        // sometimes do, and that would let the resolved URL escape the
        // intended `resolve/main/<file>` prefix.
        if (has_dot_dot_segment(file_path)) return {};
        return "https://huggingface.co/" + user_repo + "/resolve/main/" + file_path;
    }
    // Handle direct URLs
    if (checkpoint_ref.starts_with("http://") || checkpoint_ref.starts_with("https://"))
        return checkpoint_ref;
    return {};
}

} // namespace pulp::tools::audio
