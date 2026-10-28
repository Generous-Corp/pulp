#include <RmlUi/Core.h>
#include <pulp/view/rmlui_backend.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::view::rmlui {

namespace {
[[maybe_unused]] void rmlui_core_compile_smoke() {
    Rml::Vector2i viewport{0, 0};
    (void)viewport;
}

class RenderAdapter final : public Rml::RenderInterface {
  public:
    // Bound per-editor resource admission so repeated document rebuilds cannot
    // grow handle tables or backing payloads without limit.
    static constexpr std::size_t max_geometry_handles = 4096;
    static constexpr std::size_t max_geometry_vertices = 1u << 20;
    static constexpr std::size_t max_geometry_indices = 1u << 20;
    static constexpr std::size_t max_geometry_bytes = 64u * 1024u * 1024u;
    static constexpr std::size_t max_texture_handles = 4096;
    static constexpr std::size_t max_texture_bytes = 64u * 1024u * 1024u;

    RenderAdapter(AssetProvider& assets, pulp::view::rmlui::RenderInterface& sink)
        : assets_(assets), sink_(sink) {}

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        if (vertices.empty() || indices.empty() || vertices.size() > max_geometry_vertices ||
            indices.size() > max_geometry_indices)
            return 0;
        const auto payload_bytes =
            vertices.size() * sizeof(DrawVertex) + indices.size() * sizeof(std::uint32_t);
        if (payload_bytes > max_geometry_bytes ||
            geometry_bytes_ > max_geometry_bytes - payload_bytes)
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
        auto vertex_payload = std::make_shared<std::vector<DrawVertex>>();
        vertex_payload->reserve(vertices.size());
        for (const auto& vertex : vertices) {
            const auto& colour = vertex.colour;
            const auto packed = (static_cast<std::uint32_t>(colour.red) << 24) |
                                (static_cast<std::uint32_t>(colour.green) << 16) |
                                (static_cast<std::uint32_t>(colour.blue) << 8) |
                                static_cast<std::uint32_t>(colour.alpha);
            vertex_payload->push_back(DrawVertex{vertex.position.x, vertex.position.y,
                                                 vertex.tex_coord.x, vertex.tex_coord.y, packed});
        }
        auto index_payload = std::make_shared<std::vector<std::uint32_t>>();
        index_payload->reserve(indices.size());
        for (const auto index : indices)
            index_payload->push_back(static_cast<std::uint32_t>(index));
        geometry.vertices = std::move(vertex_payload);
        geometry.indices = std::move(index_payload);
        geometry.bytes = payload_bytes;
        Rml::CompiledGeometryHandle handle = 0;
        if (!free_geometry_handles_.empty()) {
            handle = free_geometry_handles_.back();
            free_geometry_handles_.pop_back();
            geometries_[static_cast<std::size_t>(handle - 1)] = std::move(geometry);
        } else {
            if (geometries_.size() >= max_geometry_handles)
                return 0;
            geometries_.push_back(std::move(geometry));
            handle = static_cast<Rml::CompiledGeometryHandle>(geometries_.size());
        }
        geometry_bytes_ += payload_bytes;
        return handle;
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                        Rml::TextureHandle texture_handle) override {
        if (handle == 0 || handle > geometries_.size())
            return;
        const auto& geometry = geometries_[static_cast<std::size_t>(handle - 1)];
        DrawCommand command{DrawCommand::Kind::rectangle,
                            geometry.x + translation.x,
                            geometry.y + translation.y,
                            geometry.width,
                            geometry.height,
                            {},
                            {}};
        if (texture_handle != 0 && texture_handle <= textures_.size()) {
            command.kind = DrawCommand::Kind::texture;
            const auto& texture = textures_[static_cast<std::size_t>(texture_handle - 1)];
            command.texture_path = texture.path;
            command.texture_bytes = texture.bytes;
            command.texture_width = texture.width;
            command.texture_height = texture.height;
        }
        command.vertices = geometry.vertices;
        command.indices = geometry.indices;
        command.scissor_enabled = scissor_enabled_;
        command.scissor = scissor_;
        command.transform_enabled = transform_enabled_;
        command.transform = transform_;
        sink_.submit(command);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        if (handle != 0 && handle <= geometries_.size() &&
            geometries_[static_cast<std::size_t>(handle - 1)].vertices) {
            geometry_bytes_ -= geometries_[static_cast<std::size_t>(handle - 1)].bytes;
            geometries_[static_cast<std::size_t>(handle - 1)] = {};
            free_geometry_handles_.push_back(handle);
        }
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        Asset asset;
        if (assets_.load(source, asset) != Status::ok)
            return 0;
        const auto size = image_dimensions(asset.bytes);
        if (!size)
            return 0;
        dimensions = {size->first, size->second};
        return store_texture(Texture{
            std::move(asset.path), std::make_shared<const std::string>(std::move(asset.bytes)),
            static_cast<std::uint32_t>(size->first), static_cast<std::uint32_t>(size->second)});
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> bytes,
                                       Rml::Vector2i dimensions) override {
        if (dimensions.x <= 0 || dimensions.y <= 0 || bytes.size() > max_texture_bytes ||
            texture_bytes_ > max_texture_bytes - bytes.size())
            return 0;
        auto payload = std::make_shared<std::string>(reinterpret_cast<const char*>(bytes.data()),
                                                     bytes.size());
        return store_texture(Texture{"", std::move(payload),
                                     static_cast<std::uint32_t>(dimensions.x),
                                     static_cast<std::uint32_t>(dimensions.y)});
    }
    void ReleaseTexture(Rml::TextureHandle handle) override {
        if (handle != 0 && handle <= textures_.size() &&
            textures_[static_cast<std::size_t>(handle - 1)].bytes) {
            texture_bytes_ -= textures_[static_cast<std::size_t>(handle - 1)].bytes->size();
            textures_[static_cast<std::size_t>(handle - 1)] = {};
            free_texture_handles_.push_back(handle);
        }
    }
    void EnableScissorRegion(bool enable) override {
        scissor_enabled_ = enable;
    }
    void SetScissorRegion(Rml::Rectanglei region) override {
        scissor_ = {region.Left(), region.Top(), region.Width(), region.Height()};
    }
    void SetTransform(const Rml::Matrix4f* transform) override {
        transform_enabled_ = transform != nullptr;
        if (transform)
            std::copy_n(transform->data(), transform_.size(), transform_.begin());
        else
            transform_.fill(0.0f);
    }

  private:
    struct Texture {
        std::string path;
        std::shared_ptr<const std::string> bytes;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    Rml::TextureHandle store_texture(Texture texture) {
        if (!texture.bytes || texture.bytes->size() > max_texture_bytes ||
            texture_bytes_ > max_texture_bytes - texture.bytes->size())
            return 0;
        Rml::TextureHandle handle = 0;
        if (!free_texture_handles_.empty()) {
            handle = free_texture_handles_.back();
            free_texture_handles_.pop_back();
            textures_[static_cast<std::size_t>(handle - 1)] = std::move(texture);
        } else {
            if (textures_.size() >= max_texture_handles)
                return 0;
            textures_.push_back(std::move(texture));
            handle = static_cast<Rml::TextureHandle>(textures_.size());
        }
        texture_bytes_ += textures_[static_cast<std::size_t>(handle - 1)].bytes->size();
        return handle;
    }

    static std::optional<std::pair<int, int>> image_dimensions(const std::string& bytes) {
        // PNG: signature, IHDR length/type, then big-endian width/height.
        if (bytes.size() >= 24 && static_cast<unsigned char>(bytes[0]) == 0x89 && bytes[1] == 'P' &&
            bytes[2] == 'N' && bytes[3] == 'G' && bytes[12] == 'I' && bytes[13] == 'H' &&
            bytes[14] == 'D' && bytes[15] == 'R') {
            const auto u32 = [&](std::size_t offset) {
                return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset]))
                        << 24) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1]))
                        << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2]))
                        << 8) |
                       static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3]));
            };
            const auto width = u32(16);
            const auto height = u32(20);
            if (width > 0 && height > 0 && width <= INT_MAX && height <= INT_MAX)
                return std::pair{static_cast<int>(width), static_cast<int>(height)};
        }
        return std::nullopt;
    }

    AssetProvider& assets_;
    struct Geometry {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        std::shared_ptr<const std::vector<DrawVertex>> vertices;
        std::shared_ptr<const std::vector<std::uint32_t>> indices;
        std::size_t bytes = 0;
    };

    pulp::view::rmlui::RenderInterface& sink_;
    std::vector<Geometry> geometries_;
    std::vector<Rml::CompiledGeometryHandle> free_geometry_handles_;
    std::vector<Texture> textures_;
    std::vector<Rml::TextureHandle> free_texture_handles_;
    std::size_t geometry_bytes_ = 0;
    std::size_t texture_bytes_ = 0;
    bool scissor_enabled_ = false;
    ScissorRect scissor_{};
    bool transform_enabled_ = false;
    std::array<float, 16> transform_{};
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
        : file(assets), render(assets, renderer),
          context_name("pulp-rmlui-" + std::to_string(next_context_id.fetch_add(1))) {}

    FileAdapter file;
    RenderAdapter render;
#if !PULP_RMLUI_FONTS_ENABLED
    Rml::FontEngineInterface font;
#endif
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    bool initialised = false;
    std::string context_name;
    bool active = false;
#if PULP_RMLUI_FONTS_ENABLED
    // RmlUi retains memory-backed font spans until shutdown. Stable storage
    // keeps FreeType from observing a reclaimed temporary Asset buffer.
    std::list<std::string> font_storage;
#endif

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

    std::string bytes;
#if !defined(_WIN32)
    // Keep the confinement check and the read on one opened file description.
    // The descriptor pins the inode after open, while O_NOFOLLOW prevents a
    // final symlink swap from bypassing the canonical-path check. Read at most
    // max_bytes_ + 1 bytes so a growth race can never cause an unbounded
    // allocation before the size/digest check below.
    const int fd = ::open(candidate.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? Status::missing_asset : Status::rejected;
    struct stat opened_stat{};
    const bool stat_ok = ::fstat(fd, &opened_stat) == 0;
    if (!stat_ok || !S_ISREG(opened_stat.st_mode) || opened_stat.st_size < 0 ||
        static_cast<std::uint64_t>(opened_stat.st_size) > max_bytes_) {
        ::close(fd);
        return stat_ok && opened_stat.st_size > 0 &&
                       static_cast<std::uint64_t>(opened_stat.st_size) > max_bytes_
                   ? Status::rejected
                   : Status::missing_asset;
    }
    const auto opened_size = static_cast<std::size_t>(opened_stat.st_size);
    const auto bounded_limit =
        max_bytes_ == std::numeric_limits<std::size_t>::max() ? max_bytes_ : max_bytes_ + 1;
    bytes.reserve(std::min<std::uint64_t>(max_bytes_, opened_size));
    std::array<char, 8192> chunk{};
    while (bytes.size() <= max_bytes_) {
        const auto remaining = bounded_limit - bytes.size();
        if (remaining == 0)
            break;
        const auto want = std::min<std::size_t>(chunk.size(), remaining);
        const auto count = ::read(fd, chunk.data(), want);
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return Status::rejected;
        }
        bytes.append(chunk.data(), static_cast<std::size_t>(count));
    }
    ::close(fd);
#else
    // Windows has no POSIX descriptor API. Keep the same bounded-read
    // contract here; the canonical path check above and the final digest bind
    // the bytes to the admitted manifest entry.
    std::ifstream stream(candidate, std::ios::binary);
    if (!stream)
        return Status::missing_asset;
    bytes.resize(static_cast<std::size_t>(size));
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
        return Status::rejected;
    char extra = 0;
    if (stream.read(&extra, 1) || stream.gcount() != 0)
        bytes.push_back(extra);
#endif
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
#if !PULP_RMLUI_FONTS_ENABLED
    Rml::SetFontEngineInterface(&runtime->font);
#endif
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
    } else if (!runtime_ || !runtime_->context) {
        return Status::rejected;
    } else {
        auto* document = runtime_->context->LoadDocumentFromMemory(asset.bytes, asset.path);
        if (!document)
            return Status::rejected;
        if (runtime_->document)
            runtime_->context->UnloadDocument(runtime_->document);
        runtime_->document = document;
        runtime_->document->Show();
    }
    document_ = std::move(asset);
    return Status::ok;
}

Status Backend::load_font_face(std::string_view path, std::string_view family, bool fallback) {
    return load_font_face(path, family, FontFaceOptions{FontStyle::normal, 0, fallback, 0});
}

Status Backend::load_font_face(std::string_view path, std::string_view family,
                               FontFaceOptions options) {
    if (!open_)
        return Status::not_open;
    if (path.empty() || family.empty() || options.face_index < 0 || options.weight > 1000)
        return Status::rejected;
#if !PULP_RMLUI_FONTS_ENABLED
    (void)path;
    (void)family;
    (void)options;
    return Status::rejected;
#else
    Asset asset;
    const auto status = assets_.load(path, asset);
    if (status != Status::ok)
        return status;
    runtime_->font_storage.emplace_back(std::move(asset.bytes));
    const auto& owned = runtime_->font_storage.back();
    const auto* bytes = reinterpret_cast<const Rml::byte*>(owned.data());
    const auto style = options.style == FontStyle::italic ? Rml::Style::FontStyle::Italic
                                                          : Rml::Style::FontStyle::Normal;
    const auto weight = static_cast<Rml::Style::FontWeight>(options.weight);
    const auto loaded =
        Rml::LoadFontFace(Rml::Span<const Rml::byte>(bytes, owned.size()), Rml::String(family),
                          style, weight, options.fallback, options.face_index);
    if (!loaded)
        runtime_->font_storage.pop_back();
    return loaded ? Status::ok : Status::rejected;
#endif
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

Status Backend::process_mouse_move(int x, int y, int key_modifier_state) {
    if (!open_ || !runtime_ || !runtime_->context)
        return Status::not_open;
    runtime_->context->ProcessMouseMove(x, y, key_modifier_state);
    return Status::ok;
}

Status Backend::process_mouse_button_down(int button_index, int key_modifier_state) {
    if (!open_ || !runtime_ || !runtime_->context)
        return Status::not_open;
    runtime_->context->ProcessMouseButtonDown(button_index, key_modifier_state);
    return Status::ok;
}

Status Backend::process_mouse_button_up(int button_index, int key_modifier_state) {
    if (!open_ || !runtime_ || !runtime_->context)
        return Status::not_open;
    runtime_->context->ProcessMouseButtonUp(button_index, key_modifier_state);
    return Status::ok;
}

Status Backend::process_mouse_wheel(float x, float y, int key_modifier_state) {
    if (!open_ || !runtime_ || !runtime_->context)
        return Status::not_open;
    runtime_->context->ProcessMouseWheel({x, y}, key_modifier_state);
    return Status::ok;
}

Status Backend::process_mouse_leave() {
    if (!open_ || !runtime_ || !runtime_->context)
        return Status::not_open;
    runtime_->context->ProcessMouseLeave();
    return Status::ok;
}

Status Backend::process_key_down(int key_identifier, int key_modifier_state) {
    if (!open_ || !runtime_ || !runtime_->context)
        return Status::not_open;
    runtime_->context->ProcessKeyDown(static_cast<Rml::Input::KeyIdentifier>(key_identifier),
                                      key_modifier_state);
    return Status::ok;
}

Status Backend::process_key_up(int key_identifier, int key_modifier_state) {
    if (!open_ || !runtime_ || !runtime_->context)
        return Status::not_open;
    runtime_->context->ProcessKeyUp(static_cast<Rml::Input::KeyIdentifier>(key_identifier),
                                    key_modifier_state);
    return Status::ok;
}

Status Backend::process_text_input(std::string_view text) {
    if (!open_ || !runtime_ || !runtime_->context)
        return Status::not_open;
    if (text.empty())
        return Status::rejected;
    runtime_->context->ProcessTextInput(Rml::String{text});
    return Status::ok;
}

bool Backend::mouse_interacting() const noexcept {
    return open_ && runtime_ && runtime_->context && runtime_->context->IsMouseInteracting();
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
