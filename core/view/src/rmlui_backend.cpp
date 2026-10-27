#include <RmlUi/Core.h>
#include <pulp/view/rmlui_backend.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace pulp::view::rmlui {

namespace {
[[maybe_unused]] void rmlui_core_compile_smoke() {
    Rml::Vector2i viewport{0, 0};
    (void)viewport;
}
} // namespace

Backend::Backend(AssetProvider& assets, RenderInterface& renderer, LuaInterface& lua,
                 ActionSink* actions)
    : assets_(assets), renderer_(renderer), lua_(lua), actions_(actions) {}

RootedAssetProvider::RootedAssetProvider(std::filesystem::path root,
                                         std::vector<AssetManifestEntry> manifest,
                                         std::uint64_t max_bytes)
    : root_(std::move(root)), manifest_(std::move(manifest)), max_bytes_(max_bytes) {}

Status RootedAssetProvider::load(std::string_view path, Asset& out) {
    if (path.empty())
        return Status::rejected;

    const std::filesystem::path relative(path);
    if (relative.is_absolute())
        return Status::rejected;
    for (const auto& component : relative) {
        if (component == ".." || component == ".")
            return Status::rejected;
    }

    const auto manifest =
        std::find_if(manifest_.begin(), manifest_.end(),
                     [&](const AssetManifestEntry& entry) { return entry.path == path; });
    if (manifest == manifest_.end() || manifest->sha256.size() != 64)
        return Status::rejected;

    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(root_, ec);
    if (ec)
        return Status::missing_asset;
    const auto candidate = std::filesystem::weakly_canonical(root / relative, ec);
    if (ec)
        return Status::rejected;
    const auto confined = candidate.lexically_relative(root);
    const auto first = confined.begin();
    if (confined.empty() || first == confined.end() || *first == "..")
        return Status::rejected;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec)
        return Status::missing_asset;
    const auto size = std::filesystem::file_size(candidate, ec);
    if (ec)
        return Status::missing_asset;
    if (size > max_bytes_)
        return Status::rejected;

    std::ifstream stream(candidate, std::ios::binary);
    if (!stream)
        return Status::missing_asset;
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.size() != size || pulp::runtime::sha256_hex(bytes) != manifest->sha256)
        return Status::rejected;
    out = Asset{std::string(path), std::move(bytes)};
    return Status::ok;
}

Status Backend::open(std::uint32_t width, std::uint32_t height) {
    if (open_)
        return Status::already_open;
    width_ = width;
    height_ = height;
    open_ = true;
    return Status::ok;
}

Status Backend::load_document(std::string_view path) {
    if (!open_)
        return Status::not_open;
    Asset asset;
    auto status = assets_.load(path, asset);
    if (status != Status::ok)
        return status;
    status = lua_.load(asset.bytes, asset.path);
    if (status != Status::ok)
        return status;
    document_ = std::move(asset);
    return Status::ok;
}

Status Backend::dispatch_lua(std::string_view callback) {
    if (!open_)
        return Status::not_open;
    return lua_.dispatch(callback);
}

Status Backend::invoke_action(std::string_view action) {
    if (!open_)
        return Status::not_open;
    return actions_ ? actions_->invoke(action) : Status::rejected;
}

void Backend::render() {
    if (!open_)
        return;
    renderer_.begin_frame(width_, height_);
    renderer_.end_frame();
}

void Backend::close() {
    document_ = {};
    open_ = false;
    width_ = height_ = 0;
}

Status Headless::load(std::string_view path, Asset& out) {
    if (std::find(rejected_paths_.begin(), rejected_paths_.end(), path) != rejected_paths_.end())
        return Status::rejected;
    const auto it = std::find_if(assets_.begin(), assets_.end(),
                                 [&](const Asset& asset) { return asset.path == path; });
    if (it == assets_.end())
        return Status::missing_asset;
    out = *it;
    return Status::ok;
}

void Headless::begin_frame(std::uint32_t width, std::uint32_t height) {
    frame_width_ = width;
    frame_height_ = height;
    commands_.clear();
}

void Headless::submit(const DrawCommand& command) {
    commands_.push_back(command);
}
void Headless::end_frame() {}

Status Headless::load(std::string_view source, std::string_view source_name) {
    if (source.empty() || source_name.empty())
        return Status::rejected;
    callbacks_.push_back(std::string(source_name));
    return Status::ok;
}

Status Headless::dispatch(std::string_view callback) {
    if (callback.empty())
        return Status::rejected;
    callbacks_.push_back(std::string(callback));
    return Status::ok;
}

Status Headless::invoke(std::string_view action) {
    if (action.empty())
        return Status::rejected;
    actions_.push_back(std::string(action));
    return Status::ok;
}

void Headless::add_asset(std::string path, std::string bytes) {
    assets_.push_back(Asset{std::move(path), std::move(bytes)});
}

void Headless::reject_path(std::string path) {
    rejected_paths_.push_back(std::move(path));
}

} // namespace pulp::view::rmlui
