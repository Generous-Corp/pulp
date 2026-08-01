#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulp::format {
class StandaloneApp;
class Processor;
class ViewBridge;
} // namespace pulp::format
namespace pulp::view {
class View;
class WindowHost;
} // namespace pulp::view
namespace pulp::inspect {
class InspectorOverlay;
}

namespace pulp::format::detail {

/// Host-owned composition root for one explicitly activated standalone
/// Development Inspector session. Protocol/session code remains platform-free;
/// this owner binds live standalone state and tears transport down before UI.
class StandaloneInspectorRuntime {
  public:
    static std::unique_ptr<StandaloneInspectorRuntime>
    create(StandaloneApp& app, Processor& processor, ViewBridge& bridge, view::View& root,
           view::WindowHost& window, std::string profile,
           std::vector<std::string> custom_capabilities);

    ~StandaloneInspectorRuntime();
    StandaloneInspectorRuntime(const StandaloneInspectorRuntime&) = delete;
    StandaloneInspectorRuntime& operator=(const StandaloneInspectorRuntime&) = delete;

    /// Called from the window idle loop, after its main-thread dispatcher is live.
    void pump();
    void stop();
    void set_overlay_active(bool active);
    bool startup_failed() const { return startup_failed_; }
    std::function<void()> wrap_close(std::function<void()> close_editor);

  private:
    class Impl;
    StandaloneInspectorRuntime(std::unique_ptr<inspect::InspectorOverlay> overlay,
                               std::unique_ptr<Impl> impl,
                               std::vector<std::uint8_t> token,
                               view::View& root, view::WindowHost& window);
    std::unique_ptr<inspect::InspectorOverlay> overlay_;
    std::unique_ptr<Impl> impl_;
    std::vector<std::uint8_t> token_;
    view::View& root_;
    view::WindowHost& window_;
    bool startup_attempted_ = false;
    bool startup_failed_ = false;
    bool stopped_ = false;
};

} // namespace pulp::format::detail
