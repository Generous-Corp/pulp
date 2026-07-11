#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/native_view_host.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/web_view.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulp::examples {

namespace {

constexpr const char* kWebViewEditorHtml = R"HTML(
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      :root {
        color-scheme: dark;
        font-family: "Inter", "Helvetica Neue", sans-serif;
      }
      body {
        margin: 0;
        min-height: 100vh;
        display: grid;
        place-items: center;
        background:
          radial-gradient(circle at top, rgba(89, 196, 255, 0.28), transparent 45%),
          linear-gradient(160deg, #111827, #0f172a 48%, #0b1120);
        color: #e2e8f0;
      }
      .card {
        width: min(420px, calc(100vw - 32px));
        padding: 24px;
        border-radius: 20px;
        border: 1px solid rgba(148, 163, 184, 0.18);
        background: rgba(15, 23, 42, 0.78);
        box-shadow: 0 24px 80px rgba(15, 23, 42, 0.45);
        backdrop-filter: blur(12px);
      }
      .eyebrow {
        font-size: 12px;
        letter-spacing: 0.18em;
        text-transform: uppercase;
        color: #7dd3fc;
      }
      h1 {
        margin: 10px 0 8px;
        font-size: 28px;
      }
      p {
        margin: 0;
        color: #cbd5e1;
        line-height: 1.5;
      }
      #status {
        margin-top: 16px;
        color: #93c5fd;
        font-size: 14px;
      }
    </style>
  </head>
  <body>
    <main class="card">
      <div class="eyebrow">Pulp WebView Plugin</div>
      <h1>Plugin-hosted WebView</h1>
      <p>This editor is attached through <code>PluginViewHost</code>, not a standalone <code>WindowHost</code>.</p>
      <div id="status">Waiting for native host...</div>
    </main>
    <script>
      const status = document.getElementById("status");
      window.addEventListener("DOMContentLoaded", async () => {
        if (!window.pulp) {
          status.textContent = "bridge unavailable";
          return;
        }

        try {
          const reply = await window.pulp.postMessage("editor.ready", { source: "webview-plugin" }, "ready-1");
          status.textContent = reply?.message || "native attached";
        } catch (error) {
          status.textContent = String(error);
        }
      });
    </script>
  </body>
</html>
)HTML";

constexpr const char* kWebViewLoadingHtml = R"HTML(
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      :root {
        color-scheme: dark;
        font-family: "Inter", "Helvetica Neue", sans-serif;
      }
      body {
        margin: 0;
        min-height: 100vh;
        display: grid;
        place-items: center;
        background:
          radial-gradient(circle at top, rgba(89, 196, 255, 0.28), transparent 45%),
          linear-gradient(160deg, #111827, #0f172a 48%, #0b1120);
        color: rgba(226, 232, 240, 0.88);
      }
      .loading {
        padding: 14px 18px;
        border-radius: 999px;
        border: 1px solid rgba(148, 163, 184, 0.18);
        background: rgba(15, 23, 42, 0.72);
        letter-spacing: 0.08em;
        text-transform: uppercase;
        font-size: 12px;
      }
    </style>
  </head>
  <body>
    <div class="loading">Loading editor...</div>
  </body>
</html>
)HTML";

// The editor tree is a single view::NativeViewHost that fills the window via
// flex and embeds the processor-owned WKWebView (see the processor below). The
// NativeViewHost widget drives attach / detach / bounds / clip itself from its
// host back-reference and Yoga layout, so this example works under BOTH a
// PluginViewHost (VST3 / AUv2 / CLAP editor) AND a WindowHost (standalone): the
// widget falls back to window_host() when no plugin host is present
// (core/view/src/native_view_host.cpp — try_attach()). The previous hand-rolled
// plugin_view_host() attach path silently no-oped in standalone, where the root
// is hosted by WindowHost (core/format/src/standalone.cpp), so the WebView never
// appeared. NativeViewHost also routes headless snapshot capture (via the
// snapshot callback) and honors the active design-viewport transform for free.
//
// WKWebView SCALING CAVEAT: scaling the NativeViewHost frame REFLOWS the web
// content — CSS px track the frame — it does NOT zoom the page. Pixel-parity
// with a letterbox-scaled Pulp sibling would require WKWebView.pageZoom = scale;
// that parity hook is deliberately DEFERRED and is not a native-editor blocker.

} // namespace

class PulpWebViewPluginProcessor final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpWebViewPlugin",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.webview-plugin",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore&) override {}

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto out = output.channel(ch);
            if (ch < input.num_channels()) {
                auto in = input.channel(ch);
                for (std::size_t i = 0; i < output.num_samples(); ++i) {
                    out[i] = in[i];
                }
            } else {
                for (std::size_t i = 0; i < output.num_samples(); ++i) {
                    out[i] = 0.0f;
                }
            }
        }
    }

    format::ViewSize view_size() const override {
        return {720, 440, 480, 320, 1280, 800};
    }

    std::unique_ptr<view::View> create_view() override {
        // Create the WKWebView once and keep it on the processor so its JS heap
        // and page state survive editor close/reopen (RETENTION RULE above).
        ensure_panel();

        auto root = std::make_unique<view::View>();
        root->set_theme(view::Theme::dark());

        auto host = std::make_unique<view::NativeViewHost>();
        // Fill the window: grow to consume the main axis, stretch on the cross
        // axis (the root's default align_items). No manual set_bounds — Yoga
        // sizes the child, and the widget re-drives the native frame each paint.
        host->flex().flex_grow = 1.0f;

        if (panel_ && panel_->native_handle()) {
            // The pane BORROWS the native handle; ownership stays on the
            // processor. The snapshot callback keeps the OS-composited WebView
            // headlessly capturable (WKWebView takeSnapshot) so the smart-capture
            // path composites it instead of rastering a blank overlay region.
            view::WebViewPanel* panel = panel_.get();
            host->set_native_child(
                panel->native_handle(),
                [panel](uint32_t /*width*/, uint32_t /*height*/) {
                    return panel->snapshot_png();
                });
        }

        root->add_child(std::move(host));
        return root;
    }

    // Test-only accessor: proves the WKWebView is processor-owned and reused
    // across editor close/reopen (pointer identity). Not part of the public SDK.
    view::WebViewPanel* webview_panel_for_test() const { return panel_.get(); }

private:
    void ensure_panel() {
        if (panel_) return;

        view::WebViewOptions options;
        options.transparent_background = true;
        options.initial_html = kWebViewLoadingHtml;
        panel_ = view::WebViewPanel::create(options);
        if (!panel_) {
            runtime::log_warn(
                "PulpWebViewPlugin: native WebView backend unavailable; "
                "editor will use the fallback native background");
            return;
        }

        panel_->set_message_handler(
            [](const view::WebViewMessage& message) -> std::string {
                if (message.type == "editor.ready") {
                    return R"({"message":"native child view attached"})";
                }
                return R"({"message":"ok"})";
            });
        panel_->set_ready_handler([this] {
            if (panel_) {
                panel_->set_html(kWebViewEditorHtml);
            }
        });
    }

    // Owned by the PROCESSOR, not the view tree — survives editor close/reopen.
    std::unique_ptr<view::WebViewPanel> panel_;
};

inline std::unique_ptr<format::Processor> create_pulp_webview_plugin() {
    return std::make_unique<PulpWebViewPluginProcessor>();
}

} // namespace pulp::examples
