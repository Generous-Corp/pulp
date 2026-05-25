#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/web_view.hpp>

#include <memory>
#include <string>

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

class WebViewEditorPane final : public view::View {
public:
    WebViewEditorPane() {
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

        panel_->set_message_handler([](const view::WebViewMessage& message) -> std::string {
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

    ~WebViewEditorPane() override {
        detach_if_needed();
    }

    void attach_if_needed() {
        auto* host = plugin_view_host();
        if (attached_ || !host || !panel_ || !panel_->native_handle()) {
            return;
        }

        const auto size = host->get_size();
        attached_ = host->attach_native_child_view(
            panel_->native_handle(),
            0.0f,
            0.0f,
            static_cast<float>(size.width),
            static_cast<float>(size.height));
        if (attached_) {
            sync_to_host();
        } else if (!warned_attach_failure_) {
            warned_attach_failure_ = true;
            runtime::log_warn(
                "PulpWebViewPlugin: PluginViewHost rejected native child "
                "embedding; this platform host must provide attach/bounds/"
                "detach support for embedded WebViews");
        }
    }

    void sync_to_host() {
        auto* host = plugin_view_host();
        if (!attached_ || !host || !panel_ || !panel_->native_handle()) {
            return;
        }

        const auto size = host->get_size();
        host->set_native_child_view_bounds(
            panel_->native_handle(),
            0.0f,
            0.0f,
            static_cast<float>(size.width),
            static_cast<float>(size.height));
    }

    void detach_if_needed() {
        auto* host = plugin_view_host();
        if (!attached_ || !host || !panel_ || !panel_->native_handle()) {
            attached_ = false;
            return;
        }

        host->detach_native_child_view(panel_->native_handle());
        attached_ = false;
    }

private:
    std::unique_ptr<view::WebViewPanel> panel_;
    bool attached_ = false;
    bool warned_attach_failure_ = false;
};

class WebViewEditorRoot final : public view::View {
public:
    WebViewEditorRoot() {
        set_theme(view::Theme::dark());
        auto pane = std::make_unique<WebViewEditorPane>();
        pane_ = pane.get();
        add_child(std::move(pane));
    }

    WebViewEditorPane& pane() { return *pane_; }

    void on_resized() override {
        if (pane_) {
            pane_->set_bounds({0, 0, bounds().width, bounds().height});
        }
    }

private:
    WebViewEditorPane* pane_ = nullptr;
};

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
        return std::make_unique<WebViewEditorRoot>();
    }

    void on_view_opened(view::View& root) override {
        static_cast<WebViewEditorRoot&>(root).pane().attach_if_needed();
    }

    void on_view_resized(view::View& root, uint32_t, uint32_t) override {
        static_cast<WebViewEditorRoot&>(root).pane().sync_to_host();
    }

    void on_view_closed(view::View& root) override {
        static_cast<WebViewEditorRoot&>(root).pane().detach_if_needed();
    }
};

inline std::unique_ptr<format::Processor> create_pulp_webview_plugin() {
    return std::make_unique<PulpWebViewPluginProcessor>();
}

} // namespace pulp::examples
