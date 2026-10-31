#pragma once

#include <pulp/view/window_host.hpp>

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#include <pulp/view/sdl_window_host.hpp>
#include <utility>
#endif

namespace pulp::format::detail {

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
using view::SdlWindowHost;
using view::View;
using view::WindowHost;
using view::WindowOptions;

class SdlWindowHostAdapter final : public WindowHost {
  public:
    SdlWindowHostAdapter(View& root, const WindowOptions& options)
        : size_{static_cast<uint32_t>(options.width), static_cast<uint32_t>(options.height)} {
        SdlWindowHost::Options sdl_options;
        sdl_options.title = options.title;
        sdl_options.width = size_.width;
        sdl_options.height = size_.height;
        sdl_options.resizable = options.resizable;
        host_ = SdlWindowHost::create(root, std::move(sdl_options));
    }

    bool valid() const {
        return host_ != nullptr;
    }
    void show() override {
        if (host_) {
            host_->show();
            visible_ = true;
        }
    }
    void hide() override {
        if (host_) {
            host_->hide();
            visible_ = false;
        }
    }
    bool is_visible() const override {
        return visible_;
    }
    void repaint() override {
        if (host_)
            host_->repaint();
    }
    ContentSize get_content_size() const override {
        return size_;
    }
    void set_close_callback(std::function<void()> cb) override {
        if (host_)
            host_->set_close_callback(std::move(cb));
    }
    void run_event_loop() override {
        if (host_)
            host_->run_event_loop();
    }

  private:
    std::unique_ptr<SdlWindowHost> host_;
    ContentSize size_;
    bool visible_ = false;
};

#endif

// Called on the standalone startup thread before creating its editor window.
// Embedding hosts retain their explicitly registered factory; generic
// WindowHost::create() never silently installs a platform implementation.
inline void ensure_standalone_window_factory() {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    if (!view::WindowHost::has_factory()) {
        view::WindowHost::set_factory(
            [](view::View& root,
               const view::WindowOptions& options) -> std::unique_ptr<view::WindowHost> {
                auto host = std::make_unique<SdlWindowHostAdapter>(root, options);
                if (!host->valid())
                    return nullptr;
                return host;
            });
    }
#endif
}

} // namespace pulp::format::detail
