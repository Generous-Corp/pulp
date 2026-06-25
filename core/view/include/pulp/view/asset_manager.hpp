#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

// ── Asset Types ─────────────────────────────────────────────────────────────

/// Raw pixel data (RGBA 8-bit per channel).
struct ImageData {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;

    bool valid() const { return !pixels.empty() && width > 0 && height > 0; }
    size_t byte_size() const { return pixels.size(); }
};

/// Font data loaded from TTF/OTF.
struct FontData {
    std::vector<uint8_t> data;       // Raw font file bytes
    std::string family_name;
    bool valid() const { return !data.empty(); }
    size_t byte_size() const { return data.size(); }
};

/// Shader source with optional compilation error.
struct ShaderData {
    std::string source;
    std::string error;               // Non-empty if compilation failed
    bool valid() const { return error.empty() && !source.empty(); }
    size_t byte_size() const { return source.size(); }
};

/// Raw binary data for any resource.
struct BlobData {
    std::vector<uint8_t> data;
    bool valid() const { return !data.empty(); }
    size_t byte_size() const { return data.size(); }
};

// ── Asset Manager ───────────────────────────────────────────────────────────
// Centralized resource loading with LRU caching.
//
// Loading sources supported:
//   - File path
//   - Memory buffer
//   - Embedded resource (registered at startup via CMake pulp_add_binary_data)
//   - data: URI (for Three.js bridge compatibility)
//
// All load functions are synchronous. For async loading, use load_async()
// which accepts a completion callback.

class AssetManager {
public:
    AssetManager();
    ~AssetManager();

    /// Get the singleton instance.
    static AssetManager& instance();

    // ── Embedded Resource Registration ──────────────────────────────────────

    /// Register an embedded resource (called by generated code from pulp_add_binary_data).
    void register_embedded(const std::string& name, const uint8_t* data, size_t size);

    /// Check if an embedded resource exists.
    bool has_embedded(const std::string& name) const;

    // ── Image Loading ───────────────────────────────────────────────────────

    /// Load image from file path. Returns cached if available.
    ImageData load_image(const std::string& path);

    /// Load image from memory buffer (PNG/JPEG bytes).
    ImageData load_image_from_memory(const uint8_t* data, size_t size);

    /// Load image from embedded resource by name.
    ImageData load_image_embedded(const std::string& name);

    /// Load image from data: URI (base64-encoded PNG/JPEG).
    ImageData load_image_from_data_uri(const std::string& uri);

    // ── Font Loading ────────────────────────────────────────────────────────

    /// Load font from file path.
    FontData load_font(const std::string& path);

    /// Load font from embedded resource.
    FontData load_font_embedded(const std::string& name);

    /// Register a font in the font fallback chain.
    void register_font_family(const std::string& family, const std::string& path_or_name);

    /// Get font data for a family name (walks fallback chain).
    FontData font_for_family(const std::string& family) const;

    /// Set the font fallback chain.
    void set_font_fallback(std::vector<std::string> families);
    const std::vector<std::string>& font_fallback() const { return font_fallback_; }

    // ── Shader Management ───────────────────────────────────────────────────

    /// Load shader source from file path.
    ShaderData load_shader(const std::string& path);

    /// Load shader from embedded resource.
    ShaderData load_shader_embedded(const std::string& name);

    /// Register a shader string directly.
    void register_shader(const std::string& name, std::string source);

    /// Get a registered/loaded shader by name.
    ShaderData shader(const std::string& name) const;

    // ── Generic Blob Loading ────────────────────────────────────────────────

    /// Load raw bytes from file path.
    BlobData load_blob(const std::string& path);

    /// Load raw bytes from embedded resource.
    BlobData load_blob_embedded(const std::string& name);

    // ── Async Loading ───────────────────────────────────────────────────────

    /// Load an image asynchronously. Callback is called on the calling thread's
    /// next poll() or on a background thread (implementation-defined).
    void load_image_async(const std::string& path, std::function<void(ImageData)> callback);

    // ── Cache Management ────────────────────────────────────────────────────

    /// Set maximum cache size in bytes. Default: 64 MB.
    void set_max_cache_size(size_t bytes);
    size_t max_cache_size() const { return max_cache_bytes_; }

    /// Current cache usage in bytes.
    size_t cache_usage() const { return cache_bytes_; }

    /// Number of cached entries.
    size_t cache_count() const;

    /// Clear all cached assets.
    void clear_cache();

    /// Evict least-recently-used entries until cache fits within max size.
    void evict_lru();

    // ── Resolution ──────────────────────────────────────────────────────────

    /// Set display scale for retina-aware asset resolution.
    /// When loading "icon.png" at scale 2, will try "icon@2x.png" first.
    void set_display_scale(float scale) { display_scale_ = scale; }
    float display_scale() const { return display_scale_; }

private:
    struct EmbeddedEntry {
        const uint8_t* data;
        size_t size;
    };
    std::unordered_map<std::string, EmbeddedEntry> embedded_;

    // LRU cache: key → (data, byte_size, access_order)
    struct CacheEntry {
        enum class Type { image, font, shader, blob };
        Type type;
        ImageData image;
        FontData font;
        ShaderData shader_data;
        BlobData blob;
        size_t byte_size = 0;
        uint64_t access_time = 0;
    };
    mutable std::unordered_map<std::string, CacheEntry> cache_;
    mutable uint64_t access_counter_ = 0;
    size_t cache_bytes_ = 0;
    size_t max_cache_bytes_ = 64 * 1024 * 1024; // 64 MB
    float display_scale_ = 1.0f;

    // Font system
    std::unordered_map<std::string, std::string> font_registry_; // family → path/name
    std::vector<std::string> font_fallback_;

    mutable std::mutex mutex_;

    void touch(CacheEntry& entry) const;
    void add_to_cache(const std::string& key, CacheEntry entry);
    CacheEntry* find_cached(const std::string& key) const;
    std::string resolve_path(const std::string& path) const;

    // PNG decoding (minimal — uses stb_image or platform API)
    static ImageData decode_png(const uint8_t* data, size_t size);
    static BlobData read_file_bytes(const std::string& path);
};

} // namespace pulp::view
