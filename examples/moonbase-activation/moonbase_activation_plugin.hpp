// MoonbaseActivationPlugin — a Pulp effect gated on a Moonbase license. When
// unlicensed it fades to silence; once activated it fades back up and passes
// audio through. The editor is the interactive Pulp-native ActivationPanel (no
// WebView): its button drives the controller, online activation opens the system
// browser, and the editor's frame tick polls + applies background revalidation.
//
// Like Moonbase's own JUCE reference plugins (DRIFT/HALO), the "Drive"/"Mix"
// parameters are real, host-automatable parameters that deliberately do NOT
// process audio — the point of this example is the activation workflow wrapped
// around a real plugin surface, not the DSP. Audio is shaped only by the
// click-free license gate.
//
// This is a reference/validation example: replace the demo endpoint, product id,
// and public key with your Moonbase product's values. The public key below is a
// throwaway key that only lets the SDK construct — it cannot validate real
// Moonbase tokens.

#pragma once

#include "moonbase_activation_controller.hpp"
#include "moonbase_activation_view.hpp"
#include "platform_open_url.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/state/content_registry.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace moonbase_pulp {

// Parameter ids for the (deliberately non-processing) automatable surface.
enum : uint32_t { kDrive = 1, kMix = 2 };

// Replace these three with your Moonbase product's real values.
inline const char* demo_endpoint() { return "https://demo.moonbase.sh"; }
inline const char* demo_product_id() { return "pulp-moonbase-demo"; }
inline const char* demo_public_key()
{
    // A well-formed (throwaway) RS256 public key so the SDK constructs. Swap in
    // your product's real public key for live activation.
    return
        "-----BEGIN PUBLIC KEY-----\n"
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA08gy3e+K/RqcVWMWQVzh\n"
        "MYBSYn6OninSbQ4Zx5qFGfyxfRqMIWp+Gq3k0cCH94GjjGAajcwuQmAtTba3/C9m\n"
        "0NIqDUWN0eLldY5U7qN8+ZP+BnULvFIB592dfwJlqXJlWCKMEsZHDJn+KwjTL69R\n"
        "GFTPnC2ym7eq2g91LEB52zfzDzyd6LuIn4I+yPse/glUNBdDUWD4XVU1oZzE1Ay3\n"
        "7z5jBPUhBZSjMAsHCr+vADNhQzlemjTDF5HIH8gusmoD1XUWUNNCfB0nMsurbSt/\n"
        "+8gK4Bb1KCJrgD8j5VTDv04U/A/xne7mZhnh+msA5D6IXxx4rrFBFq0/ramPpewF\n"
        "ZwIDAQAB\n"
        "-----END PUBLIC KEY-----\n";
}

// Persist the license under the platform per-user data directory, e.g.
// ~/Library/Application Support/Pulp/GenerousCorp/MoonbaseActivation/license.mb
// on macOS, %APPDATA%\Pulp\... on Windows, ~/.local/share/pulp/... on Linux.
inline std::string demo_license_path()
{
    namespace fs = std::filesystem;
    const fs::path dir = pulp::state::ContentRegistry::platform_data_root() /
                         "GenerousCorp" / "MoonbaseActivation";
    std::error_code ec;
    fs::create_directories(dir, ec);  // best-effort; store still works if cwd-relative
    return (dir / "license.mb").string();
}

inline ActivationConfig demo_config()
{
    ActivationConfig config;
    config.endpoint = demo_endpoint();
    config.product_id = demo_product_id();
    config.public_key = demo_public_key();
    config.license_path = demo_license_path();
    return config;
}

class MoonbaseActivationPlugin : public pulp::format::Processor {
public:
    MoonbaseActivationPlugin() : controller_(demo_config()) {}

    explicit MoonbaseActivationPlugin(ActivationConfig config) : controller_(std::move(config)) {}

    pulp::format::PluginDescriptor descriptor() const override
    {
        return {"Moonbase Activation", "GenerousCorp", "com.generouscorp.moonbase-activation",
                "1.0.0", pulp::format::PluginCategory::Effect};
    }

    void define_parameters(pulp::state::StateStore& store) override
    {
        // Real, host-automatable parameters that deliberately do not process
        // audio (see file header) — they give the plugin a genuine parameter
        // surface for the activation demo without claiming DSP it doesn't do.
        store.add_parameter({.id = kDrive, .name = "Drive", .unit = "%",
                             .range = {0.0f, 100.0f, 50.0f, 0.0f}});
        store.add_parameter({.id = kMix, .name = "Mix", .unit = "%",
                             .range = {0.0f, 100.0f, 100.0f, 0.0f}});
    }

    void prepare(const pulp::format::PrepareContext& ctx) override
    {
        const double sr = ctx.sample_rate > 0 ? ctx.sample_rate : 48000.0;
        // 12 ms click-free fade on the license gate.
        gate_.set_ramp_time(0.012f, static_cast<float>(sr));
    }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override
    {
        // The only audio-thread interaction with Moonbase: one relaxed atomic
        // read. The gate then ramps to that target so a license flip never
        // clicks. (Before prepare() the ramp is a single sample, i.e. an instant
        // switch — see SmoothedValue.)
        const bool licensed = controller_.licensed().load(std::memory_order_relaxed);
        gate_.set_target(licensed ? 1.0f : 0.0f);

        const std::size_t channels = out.num_channels();
        const std::size_t frames = out.num_samples();
        const std::size_t in_channels = in.num_channels();
        for (std::size_t s = 0; s < frames; ++s) {
            const float g = gate_.next();   // one ramp step per frame, shared across channels
            for (std::size_t c = 0; c < channels; ++c) {
                const float x = (c < in_channels && s < in.channel(c).size())
                                    ? in.channel(c)[s]
                                    : 0.0f;
                out.channel(c)[s] = x * g;
            }
        }
    }

    pulp::format::ViewSize view_size() const override { return {420, 320, 360, 260, 640, 460}; }

    std::unique_ptr<pulp::view::View> create_view() override
    {
        return std::make_unique<ActivationPanel>(controller_, pulp::view::Theme::dark(),
                                                 420.0f, 320.0f);
    }

    void on_view_opened(pulp::view::View& view) override
    {
        controller_.on_open_url = [](const std::string& url) { open_url_in_browser(url); };
        view_open_ = true;

        auto* panel = dynamic_cast<ActivationPanel*>(&view);
        clock_ = view.frame_clock();
        if (clock_) {
            // With a frame tick we can drive the controller non-blocking: apply
            // background revalidation (pump), poll ~1 s while waiting on the
            // browser, and rebuild the panel whenever the screen changes.
            controller_.start_async();
            poll_accum_ = 0.0f;
            tick_sub_id_ = clock_->subscribe([this, panel](float dt) {
                if (!view_open_) return false;   // belt-and-suspenders auto-unsubscribe
                controller_.pump();
                poll_accum_ += dt;
                if (controller_.screen() == MoonbaseActivationController::Screen::BrowserWait
                    && poll_accum_ >= 1.0f) {
                    poll_accum_ = 0.0f;
                    controller_.poll_once();
                }
                if (panel) panel->refresh_if_changed();
                return true;
            });
        } else {
            // No frame tick to drive pump()/poll()/refresh — fall back to the
            // synchronous startup so the background result is never stranded and
            // the panel reflects the resolved screen at open. (GPU plugin hosts
            // and the standalone provide a frame clock; interactive transitions
            // need one.)
            controller_.start();
        }
    }

    void on_view_closed(pulp::view::View&) override
    {
        view_open_ = false;
        if (clock_ && tick_sub_id_ >= 0) clock_->unsubscribe(tick_sub_id_);
        tick_sub_id_ = -1;
        clock_ = nullptr;
    }

    MoonbaseActivationController& controller() noexcept { return controller_; }
    const MoonbaseActivationController& controller() const noexcept { return controller_; }

private:
    MoonbaseActivationController controller_;
    pulp::signal::SmoothedValue<float> gate_{0.0f};  // starts gated (silent) until licensed
    bool view_open_ = false;
    float poll_accum_ = 0.0f;
    pulp::view::FrameClock* clock_ = nullptr;        // editor frame clock, while open
    int tick_sub_id_ = -1;                           // our subscription id (for unsubscribe)
};

// Factory for the format entry points (VST3 / AU / CLAP / Standalone).
inline std::unique_ptr<pulp::format::Processor> create_moonbase_activation_plugin()
{
    return std::make_unique<MoonbaseActivationPlugin>();
}

} // namespace moonbase_pulp
