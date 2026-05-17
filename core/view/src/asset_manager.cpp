#include <pulp/view/asset_manager.hpp>
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/text_font_context.hpp>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace pulp::view {

AssetManager::AssetManager() {
    // pulp emoji-parity — auto-register the best available color-emoji
    // typeface so plugin authors get visible emoji rendering without
    // any explicit setup. On macOS / Windows this picks up the system
    // "Apple Color Emoji" / "Segoe UI Emoji" typeface; on Linux /
    // Android / headless it falls through to the bundled Noto Color
    // Emoji (when `PULP_BUNDLE_NOTO_COLOR_EMOJI=ON`). Idempotent — a
    // subsequent `pulp::canvas::register_emoji_fallback(...)` call from
    // user code replaces the discovery result.
    pulp::canvas::register_best_available_emoji_fallback();
}
AssetManager::~AssetManager() = default;

AssetManager& AssetManager::instance() {
    static AssetManager mgr;
    return mgr;
}

// ── Embedded Resource Registration ──────────────────────────────────────────

void AssetManager::register_embedded(const std::string& name, const uint8_t* data, size_t size) {
    std::lock_guard lock(mutex_);
    embedded_[name] = {data, size};
}

bool AssetManager::has_embedded(const std::string& name) const {
    std::lock_guard lock(mutex_);
    return embedded_.find(name) != embedded_.end();
}

// ── Cache Internals ─────────────────────────────────────────────────────────

void AssetManager::touch(CacheEntry& entry) const {
    entry.access_time = ++access_counter_;
}

void AssetManager::add_to_cache(const std::string& key, CacheEntry entry) {
    cache_bytes_ += entry.byte_size;
    cache_[key] = std::move(entry);

    // Evict if over budget
    if (cache_bytes_ > max_cache_bytes_)
        evict_lru();
}

AssetManager::CacheEntry* AssetManager::find_cached(const std::string& key) const {
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        touch(it->second);
        return &it->second;
    }
    return nullptr;
}

std::string AssetManager::resolve_path(const std::string& path) const {
    if (display_scale_ >= 1.5f) {
        // Try @2x variant first
        auto dot = path.rfind('.');
        if (dot != std::string::npos) {
            std::string retina = path.substr(0, dot) + "@2x" + path.substr(dot);
            if (std::filesystem::exists(retina))
                return retina;
        }
    }
    return path;
}

// ── File I/O ────────────────────────────────────────────────────────────────

BlobData AssetManager::read_file_bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};

    auto size = file.tellg();
    if (size <= 0) return {};

    BlobData blob;
    blob.data.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(blob.data.data()), size);
    return blob;
}

// ── Image Loading ───────────────────────────────────────────────────────────

// Minimal PNG header check + raw pixel extraction
// For production, this would use stb_image or platform decoders.
// For now, we store the raw PNG bytes as the "image" and let the
// rendering backend decode on use.
ImageData AssetManager::decode_png(const uint8_t* data, size_t size) {
    ImageData img;
    if (size < 8) return img;

    // Check PNG signature
    static const uint8_t png_sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    bool is_png = true;
    for (int i = 0; i < 8; ++i) {
        if (data[i] != png_sig[i]) { is_png = false; break; }
    }

    // Store raw bytes — actual decoding happens in the renderer
    img.pixels.assign(data, data + size);
    if (is_png && size >= 24) {
        // Parse IHDR chunk for dimensions
        img.width = (static_cast<uint32_t>(data[16]) << 24) |
                    (static_cast<uint32_t>(data[17]) << 16) |
                    (static_cast<uint32_t>(data[18]) << 8) |
                    static_cast<uint32_t>(data[19]);
        img.height = (static_cast<uint32_t>(data[20]) << 24) |
                     (static_cast<uint32_t>(data[21]) << 16) |
                     (static_cast<uint32_t>(data[22]) << 8) |
                     static_cast<uint32_t>(data[23]);
    }
    return img;
}

ImageData AssetManager::load_image(const std::string& path) {
    std::lock_guard lock(mutex_);
    std::string key = "img:" + path;

    if (auto* cached = find_cached(key))
        return cached->image;

    auto resolved = resolve_path(path);
    auto blob = read_file_bytes(resolved);
    if (!blob.valid()) return {};

    auto img = decode_png(blob.data.data(), blob.data.size());
    if (!img.valid()) return {};

    CacheEntry entry;
    entry.type = CacheEntry::Type::image;
    entry.image = img;
    entry.byte_size = img.byte_size();
    add_to_cache(key, std::move(entry));

    return img;
}

ImageData AssetManager::load_image_from_memory(const uint8_t* data, size_t size) {
    return decode_png(data, size);
}

ImageData AssetManager::load_image_embedded(const std::string& name) {
    std::lock_guard lock(mutex_);
    std::string key = "img_emb:" + name;

    if (auto* cached = find_cached(key))
        return cached->image;

    auto it = embedded_.find(name);
    if (it == embedded_.end()) return {};

    auto img = decode_png(it->second.data, it->second.size);
    if (!img.valid()) return {};

    CacheEntry entry;
    entry.type = CacheEntry::Type::image;
    entry.image = img;
    entry.byte_size = img.byte_size();
    add_to_cache(key, std::move(entry));

    return img;
}

ImageData AssetManager::load_image_from_data_uri(const std::string& uri) {
    // data:image/png;base64,... format
    auto comma = uri.find(',');
    if (comma == std::string::npos) return {};

    std::string encoded = uri.substr(comma + 1);

    // Simple base64 decode
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> decoded;
    decoded.reserve(encoded.size() * 3 / 4);

    uint32_t val = 0;
    int bits = 0;
    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        auto pos = chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | static_cast<uint32_t>(pos);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
        }
    }

    return decode_png(decoded.data(), decoded.size());
}

// ── Font Loading ────────────────────────────────────────────────────────────

FontData AssetManager::load_font(const std::string& path) {
    std::lock_guard lock(mutex_);
    std::string key = "font:" + path;

    if (auto* cached = find_cached(key))
        return cached->font;

    auto blob = read_file_bytes(path);
    if (!blob.valid()) return {};

    FontData font;
    font.data = std::move(blob.data);

    // Extract family name from path using platform separators.
    font.family_name = std::filesystem::path(path).stem().string();

    CacheEntry entry;
    entry.type = CacheEntry::Type::font;
    entry.font = font;
    entry.byte_size = font.byte_size();
    add_to_cache(key, std::move(entry));

    return font;
}

FontData AssetManager::load_font_embedded(const std::string& name) {
    std::lock_guard lock(mutex_);
    std::string key = "font_emb:" + name;

    if (auto* cached = find_cached(key))
        return cached->font;

    auto it = embedded_.find(name);
    if (it == embedded_.end()) return {};

    FontData font;
    font.data.assign(it->second.data, it->second.data + it->second.size);
    font.family_name = name;

    CacheEntry entry;
    entry.type = CacheEntry::Type::font;
    entry.font = font;
    entry.byte_size = font.byte_size();
    add_to_cache(key, std::move(entry));

    return font;
}

void AssetManager::register_font_family(const std::string& family, const std::string& path_or_name) {
    // pulp #1150 — historically this was dead plumbing: the family→path
    // mapping was stored but never consulted by SkFontMgr, so plugin code
    // calling `register_font_family("MyFamily", "/path/to.ttf")` got a
    // silent no-op from the renderer's perspective.
    //
    // Now we forward to the canonical canvas-side registry, which
    // materialises the font via the platform SkFontMgr and makes it
    // resolvable through `canvas.set_font()` / `setFontFamily()`. The
    // legacy family→key map is still updated so existing
    // `font_for_family()` callers (mostly internal asset_manager tests)
    // keep working.
    {
        std::lock_guard lock(mutex_);
        font_registry_[family] = path_or_name;
    }

    // Try the bridge: an embedded asset name first, then a file path.
    // `register_font` returns false on non-Skia builds, on file-not-found,
    // or when Skia rejects the bytes — that's fine, the legacy map above
    // is still populated as a fallback for non-canvas consumers.
    if (has_embedded(path_or_name)) {
        auto blob = load_blob_embedded(path_or_name);
        if (blob.valid()) {
            pulp::canvas::register_font(blob.data.data(), blob.data.size(),
                                        family);
            return;
        }
    }
    pulp::canvas::register_font_file(path_or_name, family);
}

FontData AssetManager::font_for_family(const std::string& family) const {
    std::lock_guard lock(mutex_);
    auto it = font_registry_.find(family);
    if (it != font_registry_.end()) {
        // Check if it's an embedded resource or file path
        auto emb = embedded_.find(it->second);
        if (emb != embedded_.end()) {
            FontData font;
            font.data.assign(emb->second.data, emb->second.data + emb->second.size);
            font.family_name = family;
            return font;
        }
        // Try as file path (need to bypass lock — const method)
        // For now, return empty if not embedded
    }

    // Walk fallback chain
    for (auto& fb : font_fallback_) {
        auto fb_it = font_registry_.find(fb);
        if (fb_it != font_registry_.end()) {
            auto emb = embedded_.find(fb_it->second);
            if (emb != embedded_.end()) {
                FontData font;
                font.data.assign(emb->second.data, emb->second.data + emb->second.size);
                font.family_name = fb;
                return font;
            }
        }
    }

    return {};
}

void AssetManager::set_font_fallback(std::vector<std::string> families) {
    std::lock_guard lock(mutex_);
    font_fallback_ = std::move(families);
}

// ── Shader Management ───────────────────────────────────────────────────────

ShaderData AssetManager::load_shader(const std::string& path) {
    std::lock_guard lock(mutex_);
    std::string key = "shader:" + path;

    if (auto* cached = find_cached(key))
        return cached->shader_data;

    auto blob = read_file_bytes(path);
    if (!blob.valid()) return {"", "Failed to read shader file: " + path};

    ShaderData shader;
    shader.source.assign(blob.data.begin(), blob.data.end());

    CacheEntry entry;
    entry.type = CacheEntry::Type::shader;
    entry.shader_data = shader;
    entry.byte_size = shader.byte_size();
    add_to_cache(key, std::move(entry));

    return shader;
}

ShaderData AssetManager::load_shader_embedded(const std::string& name) {
    std::lock_guard lock(mutex_);
    std::string key = "shader_emb:" + name;

    if (auto* cached = find_cached(key))
        return cached->shader_data;

    auto it = embedded_.find(name);
    if (it == embedded_.end()) return {"", "Embedded shader not found: " + name};

    ShaderData shader;
    shader.source.assign(it->second.data, it->second.data + it->second.size);

    CacheEntry entry;
    entry.type = CacheEntry::Type::shader;
    entry.shader_data = shader;
    entry.byte_size = shader.byte_size();
    add_to_cache(key, std::move(entry));

    return shader;
}

void AssetManager::register_shader(const std::string& name, std::string source) {
    std::lock_guard lock(mutex_);
    CacheEntry entry;
    entry.type = CacheEntry::Type::shader;
    entry.shader_data = {std::move(source), ""};
    entry.byte_size = entry.shader_data.byte_size();
    add_to_cache("shader_reg:" + name, std::move(entry));
}

ShaderData AssetManager::shader(const std::string& name) const {
    std::lock_guard lock(mutex_);
    if (auto* cached = find_cached("shader_reg:" + name))
        return cached->shader_data;
    if (auto* cached = find_cached("shader_emb:" + name))
        return cached->shader_data;
    if (auto* cached = find_cached("shader:" + name))
        return cached->shader_data;
    return {};
}

// ── Generic Blob Loading ────────────────────────────────────────────────────

BlobData AssetManager::load_blob(const std::string& path) {
    std::lock_guard lock(mutex_);
    std::string key = "blob:" + path;

    if (auto* cached = find_cached(key))
        return cached->blob;

    auto blob = read_file_bytes(path);
    if (!blob.valid()) return {};

    CacheEntry entry;
    entry.type = CacheEntry::Type::blob;
    entry.blob = blob;
    entry.byte_size = blob.byte_size();
    add_to_cache(key, std::move(entry));

    return blob;
}

BlobData AssetManager::load_blob_embedded(const std::string& name) {
    std::lock_guard lock(mutex_);
    std::string key = "blob_emb:" + name;

    if (auto* cached = find_cached(key))
        return cached->blob;

    auto it = embedded_.find(name);
    if (it == embedded_.end()) return {};

    BlobData blob;
    blob.data.assign(it->second.data, it->second.data + it->second.size);

    CacheEntry entry;
    entry.type = CacheEntry::Type::blob;
    entry.blob = blob;
    entry.byte_size = blob.byte_size();
    add_to_cache(key, std::move(entry));

    return blob;
}

// ── Async Loading ───────────────────────────────────────────────────────────

void AssetManager::load_image_async(const std::string& path, std::function<void(ImageData)> callback) {
    // Simple synchronous fallback — a real implementation would use a thread pool
    auto img = load_image(path);
    if (callback) callback(std::move(img));
}

// ── Cache Management ────────────────────────────────────────────────────────

void AssetManager::set_max_cache_size(size_t bytes) {
    std::lock_guard lock(mutex_);
    max_cache_bytes_ = bytes;
    if (cache_bytes_ > max_cache_bytes_)
        evict_lru();
}

size_t AssetManager::cache_count() const {
    std::lock_guard lock(mutex_);
    return cache_.size();
}

void AssetManager::clear_cache() {
    std::lock_guard lock(mutex_);
    cache_.clear();
    cache_bytes_ = 0;
}

void AssetManager::evict_lru() {
    while (cache_bytes_ > max_cache_bytes_ && !cache_.empty()) {
        // Find entry with oldest access time
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.access_time < oldest->second.access_time)
                oldest = it;
        }
        cache_bytes_ -= oldest->second.byte_size;
        cache_.erase(oldest);
    }
}

} // namespace pulp::view
