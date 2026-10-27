#include <RmlUi/Core.h>
#include <pulp/view/rmlui_backend.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view::rmlui {

namespace {
[[maybe_unused]] void rmlui_core_compile_smoke() {
    Rml::Vector2i viewport{0, 0};
    (void)viewport;
}

class RenderAdapter final : public Rml::RenderInterface {
  public:
    explicit RenderAdapter(pulp::view::rmlui::RenderInterface& sink) : sink_(sink) {}

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        if (vertices.empty() || indices.empty())
            return 0;
        Geometry geometry;
        float min_x = vertices[0].position.x;
        float min_y = vertices[0].position.y;
        float max_x = min_x;
        float max_y = min_y;
        for (const auto& vertex : vertices) {
            min_x = std::min(min_x, vertex.position.x);
            min_y = std::min(min_y, vertex.position.y);
            max_x = std::max(max_x, vertex.position.x);
            max_y = std::max(max_y, vertex.position.y);
        }
        geometry.x = min_x;
        geometry.y = min_y;
        geometry.width = max_x - min_x;
        geometry.height = max_y - min_y;
        geometries_.push_back(geometry);
        return static_cast<Rml::CompiledGeometryHandle>(geometries_.size());
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                        Rml::TextureHandle) override {
        if (handle == 0 || handle > geometries_.size())
            return;
        const auto& geometry = geometries_[static_cast<std::size_t>(handle - 1)];
        sink_.submit(DrawCommand{DrawCommand::Kind::rectangle,
                                 geometry.x + translation.x,
                                 geometry.y + translation.y,
                                 geometry.width,
                                 geometry.height,
                                 {}});
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        if (handle != 0 && handle <= geometries_.size())
            geometries_[static_cast<std::size_t>(handle - 1)] = {};
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override {
        return 0;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override {
        return 0;
    }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}

  private:
    struct Geometry {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

    pulp::view::rmlui::RenderInterface& sink_;
    std::vector<Geometry> geometries_;
};

class FileAdapter final : public Rml::FileInterface {
  public:
    explicit FileAdapter(AssetProvider& assets) : assets_(assets) {}

    Rml::FileHandle Open(const Rml::String& path) override {
        Asset asset;
        if (assets_.load(path, asset) != Status::ok)
            return 0;
        return reinterpret_cast<Rml::FileHandle>(new Handle{std::move(asset.bytes), 0});
    }

    void Close(Rml::FileHandle file) override {
        delete reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(file));
    }

    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* handle = reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(file));
        if (!handle || !buffer)
            return 0;
        const auto available =
            handle->bytes.size() - std::min(handle->offset, handle->bytes.size());
        const auto count = std::min(size, available);
        std::copy_n(handle->bytes.data() + handle->offset, count, static_cast<char*>(buffer));
        handle->offset += count;
        return count;
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* handle = reinterpret_cast<Handle*>(static_cast<std::uintptr_t>(file));
        if (!handle)
            return false;
        std::int64_t base = 0;
        if (origin == SEEK_CUR)
            base = static_cast<std::int64_t>(handle->offset);
        else if (origin == SEEK_END)
            base = static_cast<std::int64_t>(handle->bytes.size());
        const auto next = base + offset;
        if (next < 0 || static_cast<std::uint64_t>(next) > handle->bytes.size())
            return false;
        handle->offset = static_cast<std::size_t>(next);
        return true;
    }

    size_t Tell(Rml::FileHandle file) override {
        const auto* handle = reinterpret_cast<const Handle*>(static_cast<std::uintptr_t>(file));
        return handle ? handle->offset : 0;
    }

  private:
    struct Handle {
        std::string bytes;
        std::size_t offset;
    };

    AssetProvider& assets_;
};
} // namespace

struct Backend::Runtime {
    Runtime(AssetProvider& assets, RenderInterface& renderer)
        : file(assets), render(renderer),
          context_name("pulp-rmlui-" + std::to_string(next_context_id.fetch_add(1))) {}

    FileAdapter file;
    RenderAdapter render;
    Rml::FontEngineInterface font;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    bool initialised = false;
    std::string context_name;
    bool active = false;

    static std::atomic_uint64_t next_context_id;
    static std::atomic_flag process_active;
};

std::atomic_uint64_t Backend::Runtime::next_context_id{1};
std::atomic_flag Backend::Runtime::process_active = ATOMIC_FLAG_INIT;

Backend::Backend(AssetProvider& assets, RenderInterface& renderer, LuaInterface& lua,
                 ActionSink* actions)
    : assets_(assets), renderer_(renderer), lua_(lua), actions_(actions) {}

Backend::~Backend() {
    close();
}

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
    if (Runtime::process_active.test_and_set(std::memory_order_acquire))
        return Status::rejected;
    auto runtime = std::make_unique<Runtime>(assets_, renderer_);
    runtime->active = true;
    Rml::SetFileInterface(&runtime->file);
    Rml::SetFontEngineInterface(&runtime->font);
    if (!Rml::Initialise()) {
        Runtime::process_active.clear(std::memory_order_release);
        return Status::rejected;
    }
    runtime->initialised = true;
    runtime->context =
        Rml::CreateContext(runtime->context_name,
                           {static_cast<int>(width), static_cast<int>(height)}, &runtime->render);
    if (!runtime->context) {
        Rml::Shutdown();
        Runtime::process_active.clear(std::memory_order_release);
        return Status::rejected;
    }
    width_ = width;
    height_ = height;
    runtime_ = std::move(runtime);
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
    const auto is_lua = asset.path.size() >= 4 && asset.path.ends_with(".lua");
    if (is_lua) {
        status = lua_.load(asset.bytes, asset.path);
        if (status != Status::ok)
            return status;
    } else if (!runtime_ || !runtime_->context ||
               !(runtime_->document =
                     runtime_->context->LoadDocumentFromMemory(asset.bytes, asset.path))) {
        return Status::rejected;
    } else {
        runtime_->document->Show();
    }
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
    if (runtime_ && runtime_->context) {
        const auto updated = runtime_->context->Update();
        const auto rendered = runtime_->context->Render();
        (void)updated;
        (void)rendered;
    }
    renderer_.end_frame();
}

void Backend::close() {
    if (runtime_) {
        if (runtime_->context)
            Rml::RemoveContext(runtime_->context_name);
        if (runtime_->initialised)
            Rml::Shutdown();
        if (runtime_->active)
            Runtime::process_active.clear(std::memory_order_release);
        runtime_.reset();
    }
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
