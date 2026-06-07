#include <pulp/runtime/model_registry.hpp>

namespace pulp::runtime {

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

// Reject percent-encoded path content. hf:// checkpoint paths are plain
// user/repo/file references; a `%` only appears as an encoded byte (e.g.
// `%2e%2e` for `..`), which a normalizing proxy/CDN could decode into a
// traversal segment that `has_dot_dot_segment` cannot see. Fail closed.
bool has_percent_encoding(std::string_view text) {
    return text.find('%') != std::string_view::npos;
}

}  // namespace

std::string resolve_checkpoint_url(const std::string& checkpoint_ref) {
    // hf://user/repo/path/to/file → https://huggingface.co/user/repo/resolve/main/path/to/file
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
        // Reject any `..` path segment in BOTH the user/repo and file portions. HTTP
        // doesn't normalize `..` in URL paths, but proxies/CDNs/redirect handlers
        // sometimes do, which could let the resolved URL escape the intended
        // `resolve/main/<file>` prefix (or escape the user/repo namespace entirely,
        // e.g. `hf://user/../evil/file.pt`).
        if (has_dot_dot_segment(user_repo) || has_dot_dot_segment(file_path)) return {};
        // Reject percent-encoded segments (e.g. `%2e%2e`) that could decode to `..`
        // after the literal check above.
        if (has_percent_encoding(user_repo) || has_percent_encoding(file_path)) return {};
        return "https://huggingface.co/" + user_repo + "/resolve/main/" + file_path;
    }
    if (checkpoint_ref.starts_with("http://") || checkpoint_ref.starts_with("https://"))
        return checkpoint_ref;
    return {};
}

const ModelEntry* find_model(const std::vector<ModelEntry>& registry, std::string_view model_id) {
    for (const auto& model : registry) {
        if (model.model_id == model_id) return &model;
    }
    return nullptr;
}

}  // namespace pulp::runtime
