// Headless screenshot generator for the Moonbase Activation panel. Renders each
// activation screen through the Pulp view tree with the Skia backend (faithful
// text + rects, no window, no audio) into an output directory (argv[1], default
// "moonbase-screenshots"). Used to refresh the README/docs imagery.
#include "moonbase_activation_controller.hpp"
#include "moonbase_activation_plugin.hpp"
#include "moonbase_activation_view.hpp"

#include <moonbase/moonbase.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace moonbase_pulp;
using Screen = MoonbaseActivationController::Screen;

namespace {

moonbase::license sample_full_license()
{
    moonbase::license lic;
    lic.trial = false;
    lic.method = moonbase::activation_method::online;
    lic.issued_to.name = "Ada Lovelace";
    lic.issued_to.email = "ada@example.com";
    lic.licensed_product.name = "Moonbase Activation Demo";
    lic.expires_at = std::chrono::system_clock::now() + std::chrono::hours(24 * 365);
    lic.seat_count = 5;
    lic.seats_used = 2;
    return lic;
}

moonbase::license sample_trial_license()
{
    moonbase::license lic = sample_full_license();
    lic.trial = true;
    lic.seat_count.reset();
    lic.seats_used.reset();
    lic.expires_at = std::chrono::system_clock::now() + std::chrono::hours(24 * 14);
    return lic;
}

bool shoot(const std::string& dir, const std::string& name,
           Screen screen, const pulp::view::Theme& theme,
           const moonbase::license* license)
{
    MoonbaseActivationController controller(demo_config());
    if (license) controller.set_preview_state(screen, *license);
    else controller.set_preview_state(screen);

    auto view = build_activation_view(controller, theme, 420.0f, 320.0f);
    const std::string path = dir + "/" + name + ".png";
    const bool ok = pulp::view::render_to_file(*view, 420, 320, path, 2.0f,
                                               pulp::view::ScreenshotBackend::skia);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "moonbase-screenshots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto dark = pulp::view::Theme::dark();
    const auto light = pulp::view::Theme::light();
    const auto full = sample_full_license();
    const auto trial = sample_trial_license();

    bool ok = true;
    ok &= shoot(dir, "welcome-dark", Screen::Welcome, dark, nullptr);
    ok &= shoot(dir, "browser-wait-dark", Screen::BrowserWait, dark, nullptr);
    ok &= shoot(dir, "details-dark", Screen::Details, dark, &full);
    ok &= shoot(dir, "details-light", Screen::Details, light, &full);
    ok &= shoot(dir, "trial-dark", Screen::Trial, dark, &trial);
    ok &= shoot(dir, "expired-dark", Screen::Expired, dark, nullptr);
    ok &= shoot(dir, "error-dark", Screen::Error, dark, nullptr);
    return ok ? 0 : 1;
}
