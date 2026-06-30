// Validation tests for the Moonbase activation example.
//
// These run with NO network: a capturing fake http_transport stands in for the
// Moonbase API, so we can prove (a) the moonbase-pulp User-Agent is emitted on
// the wire, (b) audio is gated on the license atomic, (c) the controller's
// screen state machine routes correctly, and (d) the native panel renders in
// the Pulp view tree with no WebView/overlay node and is theme-driven.

#include <catch2/catch_test_macros.hpp>

#include "moonbase_activation_controller.hpp"
#include "moonbase_activation_plugin.hpp"
#include "moonbase_activation_view.hpp"

#include <moonbase/moonbase.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme.hpp>

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace moonbase_pulp;

namespace {

// Records the last request and returns canned responses — no sockets.
class CapturingTransport : public moonbase::http_transport {
public:
    moonbase::http_request last_request;
    int request_count = 0;

    moonbase::http_response send(const moonbase::http_request& request) override
    {
        last_request = request;
        ++request_count;

        moonbase::http_response response;
        if (request.method == "POST") {
            // request_activation → an activation_request JSON.
            response.status_code = 200;
            response.body =
                R"({"id":"act-123",)"
                R"("request":"https://demo.moonbase.sh/api/client/poll/act-123",)"
                R"("browser":"https://demo.moonbase.sh/activate/act-123"})";
        } else {
            // get_requested_activation poll → 204 (not yet fulfilled).
            response.status_code = 204;
        }
        return response;
    }
};

ActivationConfig test_config()
{
    // Reuse the example's throwaway public key so the SDK constructs.
    return demo_config();
}

std::shared_ptr<moonbase::licensing> make_test_licensing(
    const ActivationConfig& config, std::shared_ptr<CapturingTransport> transport)
{
    auto store = std::make_shared<moonbase::memory_license_store>();
    auto fingerprints =
        std::make_shared<moonbase::static_fingerprint_provider>("Test Device", "device-123");
    return std::make_shared<moonbase::licensing>(
        make_options(config), std::move(store), std::move(fingerprints), std::move(transport));
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Recursively assert no node in the tree is a native overlay (WebView etc.).
bool any_native_overlay(const pulp::view::View& view)
{
    if (view.contains_native_overlay()) return true;
    for (std::size_t i = 0; i < view.child_count(); ++i) {
        if (const auto* child = view.child_at(i); child && any_native_overlay(*child)) return true;
    }
    return false;
}

} // namespace

TEST_CASE("moonbase-pulp User-Agent is emitted on the wire", "[moonbase][useragent]")
{
    auto config = test_config();
    auto transport = std::make_shared<CapturingTransport>();
    auto licensing = make_test_licensing(config, transport);
    MoonbaseActivationController controller(config, licensing);

    controller.begin_online_activation();

    REQUIRE(transport->request_count == 1);
    const auto ua_it = transport->last_request.headers.find("User-Agent");
    REQUIRE(ua_it != transport->last_request.headers.end());
    const std::string& ua = ua_it->second;

    // The SDK prefix plus our attribution token, mirroring moonbase-juce.
    CHECK(contains(ua, "moonbase-cpp/"));
    CHECK(contains(ua, "moonbase-pulp/"));
    CHECK(contains(ua, "GenerousCorp Pulp"));

    // The activation flow advanced to the browser-wait screen with the URL the
    // (fake) API returned.
    CHECK(controller.screen() == MoonbaseActivationController::Screen::BrowserWait);
    CHECK(controller.browser_url() == "https://demo.moonbase.sh/activate/act-123");
}

TEST_CASE("PulpMoonbaseHttpTransport round-trips over a real socket", "[moonbase][transport]")
{
    // A loopback cpp-httplib server proves the Pulp transport actually moves
    // bytes (method, path, headers, body) over HTTP, not just that it compiles.
    httplib::Server server;
    server.Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
        res.status = 200;
        res.set_content("ua=" + req.get_header_value("User-Agent") + ";body=" + req.body,
                        "text/plain");
    });
    server.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;  // mirrors a "not yet fulfilled" activation poll
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread listener([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    const std::string origin = "http://127.0.0.1:" + std::to_string(port);
    PulpMoonbaseHttpTransport transport;

    SECTION("POST forwards headers + body and returns the response")
    {
        moonbase::http_request request;
        request.method = "POST";
        request.url = origin + "/echo";
        request.headers = {{"User-Agent", "moonbase-pulp/test"}, {"Content-Type", "application/json"}};
        request.body = R"({"ping":1})";

        const auto response = transport.send(request);
        CHECK(response.status_code == 200);
        CHECK(contains(response.body, "ua=moonbase-pulp/test"));
        CHECK(contains(response.body, R"(body={"ping":1})"));
    }

    SECTION("GET surfaces a 204 (pending activation)")
    {
        moonbase::http_request request;
        request.method = "GET";
        request.url = origin + "/ping";
        const auto response = transport.send(request);
        CHECK(response.status_code == 204);
    }

    SECTION("unreachable host reports status 0, not a throw")
    {
        moonbase::http_request request;
        request.method = "GET";
        request.url = "http://127.0.0.1:1/nope";  // nothing listening on port 1
        const auto response = transport.send(request);
        CHECK(response.status_code == 0);
    }

    server.stop();
    listener.join();
}

TEST_CASE("client_info constant carries the moonbase-pulp token", "[moonbase][useragent]")
{
    const auto info = client_info("9.9.9");
    CHECK(contains(info, "moonbase-pulp/9.9.9"));
    CHECK(contains(info, "GenerousCorp Pulp"));
}

TEST_CASE("audio is gated on the license atomic", "[moonbase][gating]")
{
    MoonbaseActivationPlugin plugin;

    constexpr std::size_t frames = 8;
    std::vector<float> in_l(frames, 0.5f), in_r(frames, -0.5f);
    std::vector<float> out_l(frames, 9.0f), out_r(frames, 9.0f);
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};

    pulp::audio::BufferView<const float> in(in_ptrs, 2, frames);
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx{};

    // Unlicensed (default) → silence regardless of input.
    {
        pulp::audio::BufferView<float> out(out_ptrs, 2, frames);
        REQUIRE_FALSE(plugin.controller().licensed().load());
        plugin.process(out, in, midi_in, midi_out, ctx);
        for (std::size_t s = 0; s < frames; ++s) {
            CHECK(out_l[s] == 0.0f);
            CHECK(out_r[s] == 0.0f);
        }
    }

    // Activate via the preview seam → audio passes through.
    {
        moonbase::license license;
        license.trial = false;
        plugin.controller().set_preview_state(
            MoonbaseActivationController::Screen::Details, license);
        REQUIRE(plugin.controller().licensed().load());

        std::fill(out_l.begin(), out_l.end(), 9.0f);
        std::fill(out_r.begin(), out_r.end(), 9.0f);
        pulp::audio::BufferView<float> out(out_ptrs, 2, frames);
        plugin.process(out, in, midi_in, midi_out, ctx);
        for (std::size_t s = 0; s < frames; ++s) {
            CHECK(out_l[s] == 0.5f);
            CHECK(out_r[s] == -0.5f);
        }
    }
}

TEST_CASE("controller routes the activation state machine", "[moonbase][controller]")
{
    auto config = test_config();
    auto transport = std::make_shared<CapturingTransport>();
    auto licensing = make_test_licensing(config, transport);
    MoonbaseActivationController controller(config, licensing);

    // No stored license → Welcome.
    controller.start();
    CHECK(controller.screen() == MoonbaseActivationController::Screen::Welcome);
    CHECK_FALSE(controller.licensed().load());

    // Begin activation → BrowserWait; a poll that returns 204 stays put.
    controller.begin_online_activation();
    CHECK(controller.screen() == MoonbaseActivationController::Screen::BrowserWait);
    CHECK_FALSE(controller.poll_once());
    CHECK(controller.screen() == MoonbaseActivationController::Screen::BrowserWait);

    // Cancel → back to Welcome.
    controller.cancel_activation();
    CHECK(controller.screen() == MoonbaseActivationController::Screen::Welcome);

    // Preview a trial license → Trial screen + licensed flag set.
    moonbase::license trial;
    trial.trial = true;
    controller.set_preview_state(MoonbaseActivationController::Screen::Trial, trial);
    CHECK(controller.screen() == MoonbaseActivationController::Screen::Trial);
    CHECK(controller.licensed().load());
}

TEST_CASE("license gate fades click-free when prepared", "[moonbase][gating]")
{
    MoonbaseActivationPlugin plugin;
    pulp::format::PrepareContext pctx;
    pctx.sample_rate = 48000.0;       // 12 ms fade ≈ 576 samples
    pctx.max_buffer_size = 2048;
    plugin.prepare(pctx);

    constexpr std::size_t frames = 2048;
    std::vector<float> in_l(frames, 1.0f), in_r(frames, 1.0f);
    std::vector<float> out_l(frames, 0.0f), out_r(frames, 0.0f);
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> in(in_ptrs, 2, frames);
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext ctx{};

    // Activate, then render one block: the gate ramps 0 → 1 (no instantaneous
    // jump), starting near silence and settling at unity pass-through.
    moonbase::license license;
    plugin.controller().set_preview_state(MoonbaseActivationController::Screen::Details, license);
    {
        pulp::audio::BufferView<float> out(out_ptrs, 2, frames);
        plugin.process(out, in, midi_in, midi_out, ctx);
    }
    CHECK(out_l[0] < 0.05f);            // fade-in starts near silence (no click to full)
    CHECK(out_l[frames - 1] == 1.0f);  // settled at unity
    bool monotonic_up = true, no_jump = true;
    for (std::size_t s = 1; s < frames; ++s) {
        if (out_l[s] < out_l[s - 1]) monotonic_up = false;
        if (out_l[s] - out_l[s - 1] > 0.02f) no_jump = false;   // each step is tiny
    }
    CHECK(monotonic_up);
    CHECK(no_jump);

    // Deactivate → the gate fades back DOWN to silence, again without a jump.
    plugin.controller().deactivate();
    std::fill(out_l.begin(), out_l.end(), 0.0f);
    {
        pulp::audio::BufferView<float> out(out_ptrs, 2, frames);
        plugin.process(out, in, midi_in, midi_out, ctx);
    }
    CHECK(out_l[0] > 0.95f);            // fade-out starts from unity
    CHECK(out_l[frames - 1] == 0.0f);  // settled at silence
    bool monotonic_down = true;
    for (std::size_t s = 1; s < frames; ++s)
        if (out_l[s] > out_l[s - 1]) monotonic_down = false;
    CHECK(monotonic_down);
}

TEST_CASE("interactive panel action drives the controller", "[moonbase][view][interactive]")
{
    auto config = test_config();
    auto transport = std::make_shared<CapturingTransport>();
    auto licensing = make_test_licensing(config, transport);
    MoonbaseActivationController controller(config, licensing);
    controller.set_preview_state(MoonbaseActivationController::Screen::Welcome);

    ActivationPanel panel(controller, pulp::view::Theme::dark(), 420.0f, 320.0f);
    REQUIRE(panel.displayed_screen() == MoonbaseActivationController::Screen::Welcome);

    // Click the primary action ("Activate"): the button sits at the bottom-left
    // of the panel (see lay_out_panel). This must call begin_online_activation,
    // hit the (fake) API once, and advance to BrowserWait — then the panel
    // rebuilds itself for the new screen.
    panel.simulate_click({142.0f, 320.0f - 64.0f + 20.0f});
    CHECK(transport->request_count == 1);
    CHECK(controller.screen() == MoonbaseActivationController::Screen::BrowserWait);

    // The action defers its rebuild to the frame tick (rebuilding mid-click would
    // free the button being dispatched). Drive that tick explicitly here.
    CHECK(panel.refresh_if_changed());
    CHECK(panel.displayed_screen() == MoonbaseActivationController::Screen::BrowserWait);
    CHECK_FALSE(panel.refresh_if_changed());   // idempotent once caught up

    // No WebView/overlay node appears even after interaction.
    CHECK_FALSE(any_native_overlay(panel));
}

TEST_CASE("details screen surfaces the full license fields", "[moonbase][view][details]")
{
    auto config = test_config();
    auto transport = std::make_shared<CapturingTransport>();
    auto licensing = make_test_licensing(config, transport);
    MoonbaseActivationController controller(config, licensing);

    moonbase::license lic;
    lic.method = moonbase::activation_method::online;
    lic.issued_to.name = "Ada Lovelace";
    lic.issued_to.email = "ada@example.com";
    lic.licensed_product.name = "Moonbase Activation Demo";
    lic.seat_count = 5;
    lic.seats_used = 2;
    controller.set_preview_state(MoonbaseActivationController::Screen::Details, lic);

    const std::string details = detail::details_for(controller);
    CHECK(contains(details, "Ada Lovelace"));
    CHECK(contains(details, "ada@example.com"));
    CHECK(contains(details, "Moonbase Activation Demo"));
    CHECK(contains(details, "Online"));
    CHECK(contains(details, "2 / 5"));
}

TEST_CASE("start_async applies the local license immediately, then pumps safely",
          "[moonbase][controller][async]")
{
    auto config = test_config();
    auto transport = std::make_shared<CapturingTransport>();
    auto store = std::make_shared<moonbase::memory_license_store>();

    // Seed a previously-validated license so start_async has something local.
    moonbase::license stored;
    stored.token = "stored.jwt.token";
    stored.issued_to.name = "Grace Hopper";
    store->store_local_license(stored);

    auto fingerprints =
        std::make_shared<moonbase::static_fingerprint_provider>("Test Device", "device-123");
    auto licensing = std::make_shared<moonbase::licensing>(
        make_options(config), store, fingerprints, transport);
    MoonbaseActivationController controller(config, licensing);

    // Synchronous local apply: the editor is unlocked the instant start returns,
    // with NO wait on the network.
    controller.start_async();
    CHECK(controller.licensed().load());
    CHECK(controller.screen() == MoonbaseActivationController::Screen::Details);

    // While the worker holds the SDK, SDK-touching UI actions are refused so the
    // worker has exclusive access (no concurrent licensing_/store use). A
    // deactivate during the window is a no-op — the license stays put.
    CHECK(controller.revalidating());
    controller.deactivate();
    CHECK(controller.licensed().load());
    CHECK(controller.screen() == MoonbaseActivationController::Screen::Details);

    // The background online re-validation eventually completes; pump() applies it
    // on the (here, test) thread and re-opens actions. A failed online check
    // (fake key) keeps the local license under grace — licensed must never drop.
    for (int i = 0; i < 500; ++i) {
        controller.pump();
        if (!controller.revalidating()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_FALSE(controller.revalidating());
    CHECK(controller.licensed().load());

    // Actions work again once the worker has joined.
    controller.deactivate();
    CHECK_FALSE(controller.licensed().load());
}

TEST_CASE("native panel renders in the view tree with no overlay", "[moonbase][view]")
{
    auto config = test_config();
    auto transport = std::make_shared<CapturingTransport>();
    auto licensing = make_test_licensing(config, transport);
    MoonbaseActivationController controller(config, licensing);
    controller.set_preview_state(MoonbaseActivationController::Screen::Welcome);

    auto dark_view = build_activation_view(controller, pulp::view::Theme::dark());
    REQUIRE(dark_view != nullptr);

    // Optional visual artifact for manual inspection (set MOONBASE_SNAPSHOT_DIR).
    if (const char* dir = std::getenv("MOONBASE_SNAPSHOT_DIR")) {
        pulp::view::render_to_file(*dark_view, 420, 280,
                                   std::string(dir) + "/moonbase-welcome-dark.png", 2.0f);
    }

    // The default activation UI must stay in the Pulp view tree (capturable);
    // no WebView / OS overlay node anywhere.
    CHECK_FALSE(any_native_overlay(*dark_view));

    // Render proof + skinning proof: dark vs light must differ, which can only
    // happen if colors resolve from Theme tokens (no hard-coded palette).
    auto dark_png = pulp::view::render_to_png(*dark_view, 420, 280, 2.0f);

    auto light_view = build_activation_view(controller, pulp::view::Theme::light());
    auto light_png = pulp::view::render_to_png(*light_view, 420, 280, 2.0f);

    if (!dark_png.empty() && !light_png.empty()) {
        // Compare a bool, not the raw byte vectors — Catch2's expansion
        // formatter can't stringify large binary blobs.
        const bool themes_differ = (dark_png != light_png);
        CHECK(themes_differ);
    } else {
        WARN("No raster screenshot backend available; skipped pixel skinning proof "
             "(view-tree/no-overlay assertions still ran).");
    }
}
