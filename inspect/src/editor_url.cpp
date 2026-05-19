// editor_url.cpp — Inspector source-jump editor URI configuration.
//
// See `pulp/inspect/editor_url.hpp` for the public surface and the
// design context (planning Phase 5.3).

#include <pulp/inspect/editor_url.hpp>
#include <pulp/runtime/system.hpp>

#include <string>

namespace pulp::inspect {

namespace {

// Replace every occurrence of `needle` in `haystack` with `value`.
// Defined locally so we don't pull in a string-utility header for two
// callers. The hot path is short templates (<200 chars), so std::string
// search is fine.
void replace_token(std::string& haystack,
                   std::string_view needle,
                   std::string_view value) {
    if (needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        haystack.replace(pos, needle.size(), value);
        pos += value.size();
    }
}

constexpr std::string_view kEnvVar = "PULP_INSPECTOR_EDITOR_URL";
constexpr std::string_view kDefaultTemplate = "vscode://file/{path}:{line}";

} // namespace

std::string format_editor_url(std::string_view tmpl,
                              std::string_view path,
                              int line,
                              std::optional<int> col) {
    std::string out(tmpl);
    replace_token(out, "{path}", path);
    replace_token(out, "{line}", std::to_string(line));
    // Empty substitution when col is absent keeps single-column templates
    // (e.g. Sublime / IDEA / VS Code) clean while still being correct.
    replace_token(out, "{col}", col.has_value() ? std::to_string(*col) : std::string{});
    return out;
}

bool validate_editor_url_template(std::string_view tmpl,
                                  std::string* error_out) {
    if (tmpl.empty()) {
        if (error_out) *error_out = "editor_url_template must not be empty";
        return false;
    }
    if (tmpl.find("{path}") == std::string_view::npos) {
        if (error_out) *error_out =
            "editor_url_template must contain a {path} token";
        return false;
    }
    return true;
}

std::optional<std::string> editor_url_env_override() {
    auto env = pulp::runtime::get_env(kEnvVar);
    if (!env.has_value() || env->empty()) return std::nullopt;
    return env;
}

EffectiveEditorUrl effective_editor_url(const InspectorConfig& config) {
    if (auto env = editor_url_env_override())
        return { *env, EditorUrlSource::Environment };
    if (config.editor_url_template != std::string(kDefaultTemplate))
        return { config.editor_url_template, EditorUrlSource::Config };
    return { std::string(kDefaultTemplate), EditorUrlSource::Default };
}

std::string_view editor_url_source_name(EditorUrlSource source) {
    switch (source) {
        case EditorUrlSource::Environment: return "environment";
        case EditorUrlSource::Config:      return "config";
        case EditorUrlSource::Default:     return "default";
    }
    return "default";
}

} // namespace pulp::inspect
