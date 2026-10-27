#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view::rmlui {

enum class Status : std::uint8_t {
    ok,
    already_open,
    not_open,
    missing_asset,
    rejected,
};

struct Asset {
    std::string path;
    std::string bytes;
};

class AssetProvider {
  public:
    virtual ~AssetProvider() = default;
    virtual Status load(std::string_view path, Asset& out) = 0;
};

struct AssetManifestEntry {
    std::string path;
    std::string sha256;
};

// Reads only regular files inside a declared asset root. The manifest is a
// content closure: an empty or mismatched digest rejects the read.
class RootedAssetProvider final : public AssetProvider {
  public:
    RootedAssetProvider(std::filesystem::path root, std::vector<AssetManifestEntry> manifest,
                        std::uint64_t max_bytes = 16 * 1024 * 1024);

    Status load(std::string_view path, Asset& out) override;

  private:
    std::filesystem::path root_;
    std::vector<AssetManifestEntry> manifest_;
    std::uint64_t max_bytes_;
};

struct DrawCommand {
    enum class Kind : std::uint8_t { rectangle, texture, text };
    Kind kind = Kind::rectangle;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::string text;
    // For texture commands this is the manifest-relative source path. An
    // empty path identifies a generated texture submitted by RmlUi.
    std::string texture_path{};
    std::shared_ptr<const std::string> texture_bytes{};
    std::uint32_t texture_width = 0;
    std::uint32_t texture_height = 0;
};

class RenderInterface {
  public:
    virtual ~RenderInterface() = default;
    virtual void begin_frame(std::uint32_t width, std::uint32_t height) = 0;
    virtual void submit(const DrawCommand& command) = 0;
    virtual void end_frame() = 0;
};

class LuaInterface {
  public:
    virtual ~LuaInterface() = default;
    virtual Status load(std::string_view source, std::string_view source_name) = 0;
    virtual Status dispatch(std::string_view callback) = 0;
};

class ActionSink {
  public:
    virtual ~ActionSink() = default;
    virtual Status invoke(std::string_view action) = 0;
};

// Pulp-owned seam for the optional RmlUi adapter. It intentionally contains
// no RmlUi or Lua headers; those dependencies remain behind this boundary.
class Backend {
  public:
    Backend(AssetProvider&, RenderInterface&, LuaInterface&, ActionSink* = nullptr);
    ~Backend();

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    Status open(std::uint32_t width, std::uint32_t height);
    Status load_document(std::string_view path);
    Status dispatch_lua(std::string_view callback);
    Status invoke_action(std::string_view action);
    Status process_mouse_move(int x, int y, int key_modifier_state = 0);
    Status process_mouse_button_down(int button_index, int key_modifier_state = 0);
    Status process_mouse_button_up(int button_index, int key_modifier_state = 0);
    Status process_mouse_wheel(float x, float y, int key_modifier_state = 0);
    Status process_mouse_leave();
    Status process_key_down(int key_identifier, int key_modifier_state = 0);
    Status process_key_up(int key_identifier, int key_modifier_state = 0);
    Status process_text_input(std::string_view text);
    [[nodiscard]] bool mouse_interacting() const noexcept;
    void render();
    void close();
    [[nodiscard]] bool is_open() const noexcept {
        return open_;
    }

  private:
    struct Runtime;
    AssetProvider& assets_;
    RenderInterface& renderer_;
    LuaInterface& lua_;
    ActionSink* actions_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool open_ = false;
    Asset document_;
    std::unique_ptr<Runtime> runtime_;
};

// Headless test double used to prove the Pulp-owned contract without a host
// window or external runtime.
class Headless final : public AssetProvider,
                       public RenderInterface,
                       public LuaInterface,
                       public ActionSink {
  public:
    Status load(std::string_view path, Asset& out) override;
    void begin_frame(std::uint32_t width, std::uint32_t height) override;
    void submit(const DrawCommand& command) override;
    void end_frame() override;
    Status load(std::string_view source, std::string_view source_name) override;
    Status dispatch(std::string_view callback) override;
    Status invoke(std::string_view action) override;

    void add_asset(std::string path, std::string bytes);
    void reject_path(std::string path);
    [[nodiscard]] const std::vector<DrawCommand>& commands() const noexcept {
        return commands_;
    }
    [[nodiscard]] const std::vector<std::string>& callbacks() const noexcept {
        return callbacks_;
    }
    [[nodiscard]] const std::vector<std::string>& actions() const noexcept {
        return actions_;
    }
    [[nodiscard]] std::uint32_t frame_width() const noexcept {
        return frame_width_;
    }
    [[nodiscard]] std::uint32_t frame_height() const noexcept {
        return frame_height_;
    }

  private:
    std::vector<Asset> assets_;
    std::vector<std::string> rejected_paths_;
    std::vector<DrawCommand> commands_;
    std::vector<std::string> callbacks_;
    std::vector<std::string> actions_;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
};

} // namespace pulp::view::rmlui
