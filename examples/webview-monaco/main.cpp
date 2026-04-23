// Optional Monaco WebView host example.
// This serves a prebundled browser payload from disk through Pulp's native
// WebView bridge without pretending Pulp vendors Monaco itself.

#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/web_view.hpp>
#include <pulp/view/window_host.hpp>

#include <filesystem>
#include <iostream>
#include <memory>

using namespace pulp::view;

namespace {

constexpr int kWindowWidth = 1320;
constexpr int kWindowHeight = 860;

} // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path bundle_dir = PULP_MONACO_BUNDLE_DIR;
  if (!fs::exists(bundle_dir / "index.html")) {
    std::cerr << "Monaco bundle directory is missing index.html: " << bundle_dir << "\n";
    return 1;
  }

  View root;
  root.set_theme(Theme::dark());

  WindowOptions window_options;
  window_options.title = "Pulp WebView Monaco";
  window_options.width = kWindowWidth;
  window_options.height = kWindowHeight;

  auto window = WindowHost::create(root, window_options);
  if (!window || !window->native_content_view_handle()) {
    std::cerr << "Failed to create Monaco host window\n";
    return 1;
  }

  WebViewOptions options;
  options.enable_debug = true;
  options.enable_debug_inspector = true;
  options.fetch_resource = make_webview_directory_resource_fetcher(bundle_dir);
  options.custom_scheme_uri = "pulp://monaco";

  auto panel = WebViewPanel::create(options);
  if (!panel || !panel->native_handle()) {
    std::cerr << "Failed to create Monaco WebView\n";
    return 1;
  }

  auto apply_native_bounds = [window = window.get(), panel = panel.get()] {
    const auto size = window->get_content_size();
    if (size.width == 0 || size.height == 0) {
      return;
    }
    window->set_native_child_view_bounds(
        panel->native_handle(), 0, 0, static_cast<float>(size.width), static_cast<float>(size.height));
  };

  panel->set_message_handler([](const WebViewMessage& message) -> std::string {
    std::cout << "[webview-monaco] " << message.type << " " << message.payload_json << "\n";

    if (message.type == "editor.ready") {
      return R"({"message":"native acknowledged Monaco boot"})";
    }

    if (message.type == "editor.changed") {
      return R"({"message":"native saw document change"})";
    }

    return R"({"message":"ok"})";
  });

  const auto initial_size = window->get_content_size();
  const float initial_width = initial_size.width > 0 ? static_cast<float>(initial_size.width)
                                                     : static_cast<float>(kWindowWidth);
  const float initial_height = initial_size.height > 0 ? static_cast<float>(initial_size.height)
                                                       : static_cast<float>(kWindowHeight);
  if (!window->attach_native_child_view(
          panel->native_handle(), 0, 0, initial_width, initial_height)) {
    std::cerr << "Failed to attach Monaco WebView\n";
    return 1;
  }

  window->set_resize_callback([apply_native_bounds](uint32_t, uint32_t) {
    apply_native_bounds();
  });

  panel->set_ready_handler([panel = panel.get(), apply_native_bounds] {
    apply_native_bounds();
    panel->navigate("pulp://monaco");
  });

  window->set_close_callback([window = window.get(), panel = panel.get()] {
    window->detach_native_child_view(panel->native_handle());
  });

  window->show();
  window->run_event_loop();
  return 0;
}
