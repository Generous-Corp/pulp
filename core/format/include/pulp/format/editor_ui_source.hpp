#pragma once

#include <pulp/format/editor_assets.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::format {

/// Whether a DAW-hosted editor should enable scripted-UI + theme hot reload
/// (item 1.3). OFF by default: a shipping plugin must not watch and reload from
/// disk inside a host, but a developer opts in for the edit→see-it loop by
/// exporting `PULP_DEV_HOT_RELOAD=1` in the DAW's environment. (The standalone
/// app always enables it; it IS the dev tool.) Polling already runs on every
/// editor tick, so this only decides whether the watcher acts. Kept header-inline
/// so the platform view controllers (`au_view_controller_*.mm`,
/// `au_v2_cocoa_view.mm`) share one definition.
inline bool dev_editor_hot_reload_enabled() {
    const char* v = std::getenv("PULP_DEV_HOT_RELOAD");
    if (!v)
        return false;
    switch (v[0]) {
    case '1':
    case 't':
    case 'T':
    case 'y':
    case 'Y':
        return true;
    default:
        return false;
    }
}

/// The on-disk scripted-UI entry file `assets` declares, if any. Only a dev
/// build (`PULP_DEV_BUILD`) records one; see select_editor_ui_source().
inline std::optional<std::filesystem::path> configured_ui_script_path(const EditorAssets& assets) {
    if (assets.script_path == nullptr || assets.script_path[0] == '\0')
        return std::nullopt;
    return std::filesystem::path{assets.script_path};
}

/// The on-disk theme / design-token file `assets` declares, if any.
inline std::optional<std::filesystem::path> configured_ui_theme_path(const EditorAssets& assets) {
    if (assets.theme_path == nullptr || assets.theme_path[0] == '\0')
        return std::nullopt;
    return std::filesystem::path{assets.theme_path};
}

/// The asset directories `assets` declares, split on '|'.
inline std::vector<std::filesystem::path> configured_ui_asset_roots(const EditorAssets& assets) {
    std::vector<std::filesystem::path> roots;
    if (assets.asset_roots == nullptr)
        return roots;
    std::string_view raw{assets.asset_roots};
    while (!raw.empty()) {
        const auto split = raw.find('|');
        const auto item = raw.substr(0, split);
        if (!item.empty())
            roots.emplace_back(std::string(item));
        if (split == std::string_view::npos)
            break;
        raw.remove_prefix(split + 1);
    }
    return roots;
}

/// This binary's on-disk scripted-UI entry file (see register_editor_assets()).
inline std::optional<std::filesystem::path> configured_ui_script_path() {
    return configured_ui_script_path(registered_editor_assets());
}

/// This binary's on-disk theme / design-token file.
inline std::optional<std::filesystem::path> configured_ui_theme_path() {
    return configured_ui_theme_path(registered_editor_assets());
}

/// This binary's asset directories.
inline std::vector<std::filesystem::path> configured_ui_asset_roots() {
    return configured_ui_asset_roots(registered_editor_assets());
}

template <typename EmbeddedAsset>
std::vector<EmbeddedAsset> copy_embedded_editor_files(const EditorAssets& assets) {
    std::vector<EmbeddedAsset> files;
    if (assets.files == nullptr)
        return files;
    files.reserve(assets.file_count);
    for (std::size_t i = 0; i < assets.file_count; ++i) {
        const auto& file = assets.files[i];
        if (file.name == nullptr || file.name[0] == '\0' || file.data == nullptr || file.size == 0)
            continue;
        files.push_back({file.name, file.data, file.size});
    }
    return files;
}

/// Pick the document for a default scripted editor from the record the plug-in
/// registered. The embedded script wins whenever the image carries one; the
/// on-disk path a dev build also records is honored only for a hot-reloading
/// editor (the standalone host, or a DAW launched with `PULP_DEV_HOT_RELOAD=1`),
/// and only while that file exists, so the edit-and-see loop keeps reading the
/// file the build points at while a shipped image never depends on the
/// developer's source tree. Without an embedded script the path is used as
/// before.
template <typename Source>
Source select_editor_ui_source_as(const EditorAssets& assets, bool enable_hot_reload) {
    Source source;
    const auto dev_script_path = configured_ui_script_path(assets);
    const bool dev_file_present = [&] {
        if (!dev_script_path)
            return false;
        std::error_code ec;
        return std::filesystem::is_regular_file(*dev_script_path, ec) && !ec;
    }();
    if (embeds_editor_script(assets) && !(enable_hot_reload && dev_file_present)) {
        source.script_source.assign(reinterpret_cast<const char*>(assets.script),
                                    assets.script_size);
        if (assets.theme != nullptr && assets.theme_size > 0)
            source.theme_source.assign(reinterpret_cast<const char*>(assets.theme),
                                       assets.theme_size);
        source.script_name = assets.script_name != nullptr && assets.script_name[0] != '\0'
                                 ? assets.script_name
                                 : "ui.js";
        using EmbeddedAsset = typename decltype(source.embedded_assets)::value_type;
        source.embedded_assets = copy_embedded_editor_files<EmbeddedAsset>(assets);
        return source;
    }
    if (dev_script_path)
        source.script_path = *dev_script_path;
    return source;
}

} // namespace pulp::format
