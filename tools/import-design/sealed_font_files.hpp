#pragma once

// The font files a generated panel registers with registerEmbeddedFont, read
// from the directory the panel's script sits in. A shipped plug-in embeds the
// files beside its script and hands them to its WidgetBridge; a tool that runs
// the same generated script headlessly seals the same files here, so it
// registers the same faces through the same capability-free call rather than
// through a filesystem lookup the shipped panel cannot make.

#include <pulp/runtime/safe_relative_path.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/font_container.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace pulp::import_design {

/// Owns the bytes the embedded assets point at; keep it alive for as long as
/// the bridge that was given `assets`.
struct SealedFontFiles {
    std::vector<std::unique_ptr<std::vector<unsigned char>>> storage;
    std::vector<pulp::view::WidgetBridge::EmbeddedAsset> assets;
};

/// Read every face `sealed_font_asset_name` names from `script_dir`. A file
/// that is missing, empty or larger than `kMaxShippedFontBytes` is skipped;
/// the bridge then refuses the name exactly as a plug-in without it would.
inline SealedFontFiles read_sealed_font_files(const pulp::view::DesignIR& ir,
                                              const std::filesystem::path& script_dir) {
    SealedFontFiles out;
    if (script_dir.empty())
        return out;
    std::set<std::string> seen;
    for (const auto& face : ir.font_family_assets) {
        const auto name = pulp::view::sealed_font_asset_name(ir, face);
        if (name.empty() || !seen.insert(name).second)
            continue;
        std::error_code ec;
        const auto root = std::filesystem::weakly_canonical(script_dir, ec);
        if (ec)
            continue;
        // Resolved through any symlink, the file must still lie in the
        // script's directory.
        const auto path = std::filesystem::weakly_canonical(root / std::filesystem::path(name), ec);
        if (ec || !pulp::runtime::is_within_directory(root, path))
            continue;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0 || size > pulp::view::kMaxShippedFontBytes)
            continue;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            continue;
        auto bytes = std::make_unique<std::vector<unsigned char>>(
            std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (bytes->empty())
            continue;
        out.assets.push_back({name, bytes->data(), bytes->size()});
        out.storage.push_back(std::move(bytes));
    }
    return out;
}

} // namespace pulp::import_design
