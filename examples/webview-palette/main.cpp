// WebView palette example — proves a floating child palette window can host a
// simple HTML/JS page through Pulp's WindowHost + WebViewPanel seams.

#include "pulp_webview_palette_assets_data.hpp"

#include <pulp/view/asset_manager.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/web_view.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/window_manager.hpp>

#include <iostream>
#include <memory>

using namespace pulp::view;

namespace {

constexpr int kPaletteWidth = 420;
constexpr int kPaletteHeight = 260;

void register_palette_assets() {
  auto& assets = AssetManager::instance();
  assets.register_embedded("phase7_palette_html",
                           pulp_webview_palette_assets::index_html,
                           pulp_webview_palette_assets::index_html_size);
  assets.register_embedded("phase7_palette_css",
                           pulp_webview_palette_assets::palette_css,
                           pulp_webview_palette_assets::palette_css_size);
  assets.register_embedded("phase7_palette_js",
                           pulp_webview_palette_assets::palette_js,
                           pulp_webview_palette_assets::palette_js_size);
}

} // namespace

int main() {
  register_palette_assets();

  View main_root;
  main_root.set_theme(Theme::dark());

  WindowOptions main_opts;
  main_opts.title = "Pulp WebView Palette Host";
  main_opts.width = 520;
  main_opts.height = 320;
  auto main_window = WindowHost::create(main_root, main_opts);
  if (!main_window) {
    if (!WindowHost::has_factory()) {
      std::cerr << "Failed to create main host window: WindowHost::create "
                   "returned nullptr and no WindowHost::Factory is registered\n";
    } else {
      std::cerr << "Failed to create main host window: registered "
                   "WindowHost::Factory returned nullptr\n";
    }
    return 1;
  }
  if (!main_window->native_window_handle()) {
    std::cerr << "Failed to create main host window: WindowHost does not expose "
                 "a native window handle for parented palette creation\n";
    return 1;
  }

  View palette_root;
  palette_root.set_theme(Theme::dark());

  WindowType palette_type = WindowType::palette;
  WindowOptions palette_opts;
  palette_opts.title = "WebView Palette";
  palette_opts.width = kPaletteWidth;
  palette_opts.height = kPaletteHeight;
  palette_opts.resizable = false;
  palette_opts.window_type = &palette_type;
  palette_opts.parent_native_handle = main_window->native_window_handle();
  auto palette_window = WindowHost::create(palette_root, palette_opts);
  if (!palette_window) {
    if (!WindowHost::has_factory()) {
      std::cerr << "Failed to create palette host window: WindowHost::create "
                   "returned nullptr and no WindowHost::Factory is registered\n";
    } else {
      std::cerr << "Failed to create palette host window: registered "
                   "WindowHost::Factory returned nullptr\n";
    }
    return 1;
  }
  if (!palette_window->native_content_view_handle()) {
    std::cerr << "Failed to create palette host window: WindowHost does not expose "
                 "a native content-view handle for child embedding\n";
    return 1;
  }

  WebViewOptions webview_options;
  webview_options.enable_debug = true;
  webview_options.fetch_resource = make_webview_embedded_resource_fetcher(
      "phase7_palette_html",
      {
          { "palette.css", "phase7_palette_css", "text/css" },
          { "palette.js", "phase7_palette_js", "text/javascript" },
      });
  webview_options.custom_scheme_uri = "pulp://palette";

  auto palette_panel = WebViewPanel::create(webview_options);
  if (!palette_panel || !palette_panel->native_handle()) {
    std::cerr << "Failed to create embedded WebView\n";
    return 1;
  }

  palette_panel->set_message_handler([](const WebViewMessage& message) -> std::string {
    std::cout << "[webview-palette] " << message.type << " " << message.payload_json << "\n";

    if (message.type == "palette.loaded") {
      return R"({"message":"native acknowledged palette load"})";
    }

    if (message.type == "palette.ping") {
      return R"({"message":"pong from native host"})";
    }

    return R"({"message":"ok"})";
  });

  if (!palette_window->attach_native_child_view(
          palette_panel->native_handle(), 0, 0, palette_opts.width, palette_opts.height)) {
    std::cerr << "Failed to attach WebView into palette host: WindowHost rejected "
                 "native child embedding; the concrete host must override "
                 "attach/bounds/detach for this example\n";
    return 1;
  }

  palette_panel->set_ready_handler([panel = palette_panel.get(), palette_window = palette_window.get()] {
    palette_window->set_native_child_view_bounds(panel->native_handle(), 0, 0, kPaletteWidth, kPaletteHeight);
    panel->navigate("pulp://palette");
  });

  main_window->set_close_callback([palette_window = palette_window.get(),
                                   panel = palette_panel.get()] {
    palette_window->detach_native_child_view(panel->native_handle());
  });

  main_window->show();
  palette_window->show();
  main_window->run_event_loop();
  return 0;
}
