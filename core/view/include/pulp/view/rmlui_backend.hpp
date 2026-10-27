#pragma once

#include <cstdint>
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

struct DrawCommand {
    enum class Kind : std::uint8_t { rectangle, text };
    Kind kind = Kind::rectangle;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::string text;
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

    Status open(std::uint32_t width, std::uint32_t height);
    Status load_document(std::string_view path);
    Status dispatch_lua(std::string_view callback);
    Status invoke_action(std::string_view action);
    void render();
    void close();
    [[nodiscard]] bool is_open() const noexcept {
        return open_;
    }

  private:
    AssetProvider& assets_;
    RenderInterface& renderer_;
    LuaInterface& lua_;
    ActionSink* actions_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool open_ = false;
    Asset document_;
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
