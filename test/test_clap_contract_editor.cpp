// test_clap_contract_editor.cpp — the CLAP adapter opening a processor's
// editor through the editor-host contract, in a view-free build.
//
// Built without PULP_CLAP_GUI and linked against pulp-format-clap-core, the way
// a `pulp_add_plugin(... UI <package>)` plug-in is, so the only GUI extension
// the plug-in can offer is the contract one. The editor records every contract
// call, and a fake host records the param events, resize requests, dirty marks
// and timers the plug-in sends it.

#include <catch2/catch_test_macros.hpp>

#include "support/recording_contract_editor.hpp"

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/timer-support.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using pulp::test_support::contract_config;
using pulp::test_support::contract_log;
using pulp::test_support::EditorEnvironment;
using pulp::test_support::MainLoop;
using pulp::test_support::RecordingContractFactory;
using pulp::test_support::reset_contract_recording;

namespace {

constexpr pulp::state::ParamID kGain = 1;
bool g_processor_has_factory = true;

class ContractProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Contract CLAP";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.clap-contract-editor";
        d.version = "1.0.0";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo info;
        info.id = kGain;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(info);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        out.clear();
    }
    pulp::format::EditorFactory* editor_factory() override {
        return has_factory_ ? &factory_ : nullptr;
    }

  private:
    bool has_factory_ = g_processor_has_factory;
    RecordingContractFactory factory_;
};

std::unique_ptr<pulp::format::Processor> create_contract_processor() {
    return std::make_unique<ContractProcessor>();
}

} // namespace

#include <pulp/format/clap_entry.hpp>
PULP_CLAP_PLUGIN(create_contract_processor)

namespace {

// Holds one copy of each CLAP event it is given.
struct CapturedEvents {
    struct alignas(std::max_align_t) Bytes {
        std::array<std::byte, sizeof(clap_event_param_value_t)> data{};
    };
    std::vector<Bytes> events;
    clap_output_events_t api{};

    CapturedEvents() {
        api.ctx = this;
        api.try_push = [](const clap_output_events_t* out, const clap_event_header_t* event) {
            auto* self = static_cast<CapturedEvents*>(out->ctx);
            if (event->size > sizeof(Bytes::data))
                return false;
            self->events.emplace_back();
            std::memcpy(self->events.back().data.data(), event, event->size);
            return true;
        };
    }
    std::string stream() const {
        std::string s;
        for (const auto& e : events) {
            const auto* h = reinterpret_cast<const clap_event_header_t*>(e.data.data());
            if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN)
                s += "B ";
            else if (h->type == CLAP_EVENT_PARAM_GESTURE_END)
                s += "E ";
            else if (h->type == CLAP_EVENT_PARAM_VALUE)
                s += "V ";
        }
        return s;
    }
};

// A CLAP host with params, gui, state and (optionally) timer-support.
struct FakeHost {
    clap_host_t host{};
    clap_host_params_t params{};
    clap_host_gui_t gui{};
    clap_host_state_t state{};
    clap_host_timer_support_t timers{};
    bool offers_timers = false;
    bool refuse_timers = false;
    bool accept_resize = true;
    std::vector<std::string> resize_requests;
    int dirty_marks = 0;
    std::vector<clap_id> live_timers;
    clap_id next_timer = 7;
    std::uint32_t last_period = 0;

    FakeHost() {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "Pulp contract editor test host";
        host.vendor = "Pulp";
        host.url = "https://pulp.audio";
        host.version = "1";
        host.get_extension = [](const clap_host_t* h, const char* id) -> const void* {
            auto* self = static_cast<FakeHost*>(h->host_data);
            if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
                return &self->params;
            if (std::strcmp(id, CLAP_EXT_GUI) == 0)
                return &self->gui;
            if (std::strcmp(id, CLAP_EXT_STATE) == 0)
                return &self->state;
            if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
                return self->offers_timers ? &self->timers : nullptr;
            return nullptr;
        };
        host.request_restart = [](const clap_host_t*) {};
        host.request_process = [](const clap_host_t*) {};
        host.request_callback = [](const clap_host_t*) {};
        params.rescan = [](const clap_host_t*, clap_param_rescan_flags) {};
        params.clear = [](const clap_host_t*, clap_id, clap_param_clear_flags) {};
        params.request_flush = [](const clap_host_t*) {};
        gui.resize_hints_changed = [](const clap_host_t*) {};
        gui.request_resize = [](const clap_host_t* h, uint32_t w, uint32_t hh) {
            auto* self = static_cast<FakeHost*>(h->host_data);
            self->resize_requests.push_back(std::to_string(w) + "x" + std::to_string(hh));
            return self->accept_resize;
        };
        gui.request_show = [](const clap_host_t*) { return false; };
        gui.request_hide = [](const clap_host_t*) { return false; };
        gui.closed = [](const clap_host_t*, bool) {};
        state.mark_dirty = [](const clap_host_t* h) {
            ++static_cast<FakeHost*>(h->host_data)->dirty_marks;
        };
        timers.register_timer = [](const clap_host_t* h, uint32_t period_ms, clap_id* id) {
            auto* self = static_cast<FakeHost*>(h->host_data);
            self->last_period = period_ms;
            if (self->refuse_timers)
                return false;
            *id = self->next_timer++;
            self->live_timers.push_back(*id);
            return true;
        };
        timers.unregister_timer = [](const clap_host_t* h, clap_id id) {
            auto* self = static_cast<FakeHost*>(h->host_data);
            for (auto it = self->live_timers.begin(); it != self->live_timers.end(); ++it) {
                if (*it == id) {
                    self->live_timers.erase(it);
                    return true;
                }
            }
            return false;
        };
    }
};

struct OpenPlugin {
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    const clap_plugin_params_t* params = nullptr;

    explicit OpenPlugin(FakeHost& host) {
        REQUIRE(clap_entry.init("test"));
        auto* factory = static_cast<const clap_plugin_factory_t*>(
            clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        auto* desc = factory->get_plugin_descriptor(factory, 0);
        plugin = factory->create_plugin(factory, &host.host, desc->id);
        REQUIRE(plugin != nullptr);
        REQUIRE(plugin->init(plugin));
        gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        params = static_cast<const clap_plugin_params_t*>(
            plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    }
    ~OpenPlugin() {
        if (plugin != nullptr)
            plugin->destroy(plugin);
    }
    void destroy() {
        plugin->destroy(plugin);
        plugin = nullptr;
    }
    pulp::state::StateStore& store() const {
        return static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data)->store;
    }
};

int g_fake_view = 0;
clap_window_t fake_window() {
    clap_window_t window{};
#if defined(__APPLE__)
    window.api = CLAP_WINDOW_API_COCOA;
    window.cocoa = &g_fake_view;
#elif defined(_WIN32)
    window.api = CLAP_WINDOW_API_WIN32;
    window.win32 = &g_fake_view;
#else
    window.api = CLAP_WINDOW_API_X11;
    window.x11 = 42;
#endif
    return window;
}

const char* platform_api() {
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

} // namespace

TEST_CASE("CLAP opens, sizes and closes a contract editor, twice",
          "[clap][editor-host][clap-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_processor_has_factory = true;
    FakeHost host;
    OpenPlugin p(host);
    REQUIRE(p.gui != nullptr);

    REQUIRE(p.gui->is_api_supported(p.plugin, platform_api(), false));
    REQUIRE_FALSE(p.gui->is_api_supported(p.plugin, platform_api(), true));
    REQUIRE_FALSE(p.gui->is_api_supported(p.plugin, "not-a-window-api", false));
    const char* api = nullptr;
    bool floating = true;
    REQUIRE(p.gui->get_preferred_api(p.plugin, &api, &floating));
    REQUIRE(std::strcmp(api, platform_api()) == 0);
    REQUIRE_FALSE(floating);

    REQUIRE_FALSE(p.gui->create(p.plugin, platform_api(), true));
    REQUIRE(p.gui->create(p.plugin, platform_api(), false));
    REQUIRE_FALSE(p.gui->create(p.plugin, platform_api(), false)); // already open

    uint32_t w = 0, h = 0;
    REQUIRE(p.gui->get_size(p.plugin, &w, &h));
    REQUIRE(w == 400);
    REQUIRE(h == 300);

    const auto window = fake_window();
    REQUIRE(p.gui->set_parent(p.plugin, &window));
#if defined(__APPLE__)
    REQUIRE(contract_log().saw("attach:cocoa_view"));
    REQUIRE(contract_log().parent_handle == &g_fake_view);
#endif
    REQUIRE(p.gui->show(p.plugin));
    REQUIRE(contract_log().saw("visible:on"));

    // Size negotiation: 4:3, clamped to the editor's bounds.
    REQUIRE(p.gui->can_resize(p.plugin));
    clap_gui_resize_hints_t hints{};
    REQUIRE(p.gui->get_resize_hints(p.plugin, &hints));
    REQUIRE(hints.can_resize_horizontally);
    REQUIRE(hints.can_resize_vertically);
    REQUIRE(hints.preserve_aspect_ratio);
    REQUIRE(hints.aspect_ratio_width == 400);
    REQUIRE(hints.aspect_ratio_height == 300);
    w = 1000;
    h = 1000;
    REQUIRE(p.gui->adjust_size(p.plugin, &w, &h));
    REQUIRE(w == 800);
    REQUIRE(h == 600);
    REQUIRE(p.gui->set_size(p.plugin, 640, 480));
    REQUIRE(contract_log().saw("set_size:640x480"));
    REQUIRE_FALSE(p.gui->set_size(p.plugin, 100, 100));
    REQUIRE(p.gui->get_size(p.plugin, &w, &h));
    REQUIRE(w == 640);
    REQUIRE(h == 480);

    // The host's scale reaches the editor's size and survives a resize.
    REQUIRE(p.gui->set_scale(p.plugin, 2.0));
    REQUIRE(contract_log().last_scale == 2.0);
    REQUIRE(p.gui->set_size(p.plugin, 720, 540));
    REQUIRE(contract_log().last_scale == 2.0);
    REQUIRE_FALSE(p.gui->set_scale(p.plugin, 0.0));

    // A drag through the contract's port reaches the host as CLAP param
    // events, and closing the editor mid-drag still ends the gesture.
    auto& port = contract_log().context->params();
    port.begin(kGain);
    port.set(kGain, 0.75f);
    REQUIRE(p.gui->hide(p.plugin));
    REQUIRE(contract_log().saw("visible:off"));
    p.gui->destroy(p.plugin);
    REQUIRE(contract_log().saw("detach"));
    REQUIRE_FALSE(contract_log().saw("invalidate"));
    REQUIRE(contract_log().live == 0);
    REQUIRE(p.store().open_gesture_count() == 0);
    CapturedEvents out;
    p.params->flush(p.plugin, nullptr, &out.api);
    REQUIRE(out.stream() == "B V E ");

    // Reopen: a fresh editor reads the value the first one set.
    contract_log().calls.clear();
    REQUIRE(p.gui->create(p.plugin, platform_api(), false));
    REQUIRE(p.gui->set_parent(p.plugin, &window));
    REQUIRE(contract_log().context->params().get(kGain) == 0.75f);
    p.gui->destroy(p.plugin);
    REQUIRE(contract_log().live == 0);
    REQUIRE(p.store().open_gesture_count() == 0);

    // Calls with no editor open are refused.
    REQUIRE_FALSE(p.gui->get_size(p.plugin, &w, &h));
    REQUIRE_FALSE(p.gui->set_parent(p.plugin, &window));
    REQUIRE_FALSE(p.gui->show(p.plugin));
}

TEST_CASE("a CLAP contract editor reaches its host through the context",
          "[clap][editor-host][clap-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_processor_has_factory = true;
    FakeHost host;
    OpenPlugin p(host);
    REQUIRE(p.gui->create(p.plugin, platform_api(), false));
    auto& context = *contract_log().context;

    REQUIRE(context.request_resize({500, 375, 1.0}));
    REQUIRE(host.resize_requests == std::vector<std::string>{"500x375"});
    host.accept_resize = false;
    REQUIRE_FALSE(context.request_resize({600, 450, 1.0}));

    pulp::format::HostActionRequest dirty;
    dirty.action = pulp::format::HostAction::mark_state_dirty;
    REQUIRE(context.host_action(dirty));
    REQUIRE(host.dirty_marks == 1);
    pulp::format::HostActionRequest close;
    close.action = pulp::format::HostAction::close_editor;
    REQUIRE_FALSE(context.host_action(close));
    REQUIRE(context.host_kind() == pulp::format::EditorHostKind::plugin);

    p.gui->destroy(p.plugin);
}

TEST_CASE("a CLAP contract editor ticks from the host's timer while attached",
          "[clap][editor-host][clap-contract][idle]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_processor_has_factory = true;
    FakeHost host;
    host.offers_timers = true;
    OpenPlugin p(host);
    auto* timer = static_cast<const clap_plugin_timer_support_t*>(
        p.plugin->get_extension(p.plugin, CLAP_EXT_TIMER_SUPPORT));
    REQUIRE(timer != nullptr);

    REQUIRE(p.gui->create(p.plugin, platform_api(), false));
    REQUIRE(host.live_timers.empty());
    const auto window = fake_window();
    REQUIRE(p.gui->set_parent(p.plugin, &window));
    REQUIRE(host.live_timers.size() == 1);
    REQUIRE(host.last_period == pulp::format::ContractEditorSession::kIdleIntervalMs);
    const clap_id id = host.live_timers.front();

    timer->on_timer(p.plugin, id);
    timer->on_timer(p.plugin, id);
    timer->on_timer(p.plugin, id + 100); // someone else's timer
    REQUIRE(contract_log().idles == 2);

    p.gui->destroy(p.plugin);
    REQUIRE(host.live_timers.empty());
    timer->on_timer(p.plugin, id);
    REQUIRE(contract_log().idles == 2);
}

TEST_CASE("a CLAP contract editor without host timers ticks only while attached",
          "[clap][editor-host][clap-contract][idle]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_processor_has_factory = true;
    FakeHost host;
    OpenPlugin p(host);
    MainLoop loop;

    REQUIRE(p.gui->create(p.plugin, platform_api(), false));
    loop.run_for(0.1);
    REQUIRE(contract_log().idles == 0);

    const auto window = fake_window();
    REQUIRE(p.gui->set_parent(p.plugin, &window));
    REQUIRE(loop.run_until([] { return contract_log().idles >= 3; }));

    p.gui->destroy(p.plugin);
    const int at_destroy = contract_log().idles;
    loop.run_for(0.1);
    REQUIRE(contract_log().idles == at_destroy);
}

TEST_CASE("a CLAP contract editor ticks itself when the host refuses its timer",
          "[clap][editor-host][clap-contract][idle]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_processor_has_factory = true;
    FakeHost host;
    host.offers_timers = true;
    host.refuse_timers = true;
    OpenPlugin p(host);
    MainLoop loop;

    REQUIRE(p.gui->create(p.plugin, platform_api(), false));
    const auto window = fake_window();
    REQUIRE(p.gui->set_parent(p.plugin, &window));
    REQUIRE(host.live_timers.empty());
    REQUIRE(loop.run_until([] { return contract_log().idles >= 2; }));

    p.gui->destroy(p.plugin);
    const int at_destroy = contract_log().idles;
    loop.run_for(0.1);
    REQUIRE(contract_log().idles == at_destroy);
}

TEST_CASE("destroying a CLAP plug-in with its contract editor open invalidates it first",
          "[clap][editor-host][clap-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_processor_has_factory = true;
    FakeHost host;
    host.offers_timers = true;
    OpenPlugin p(host);
    REQUIRE(p.gui->create(p.plugin, platform_api(), false));
    const auto window = fake_window();
    REQUIRE(p.gui->set_parent(p.plugin, &window));
    contract_log().context->params().begin(kGain);
    REQUIRE(host.live_timers.size() == 1);

    // The host never called gui destroy(): the editor is invalidated (its
    // window is going away) and destroyed before the processor, and the timer
    // is given back.
    p.destroy();
    REQUIRE(contract_log().saw("invalidate"));
    REQUIRE_FALSE(contract_log().saw("detach"));
    REQUIRE(contract_log().live == 0);
    REQUIRE(host.live_timers.empty());
}

TEST_CASE("a CLAP contract editor refuses what it cannot do",
          "[clap][editor-host][clap-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    g_processor_has_factory = true;
    FakeHost host;
    OpenPlugin p(host);

    contract_config().factory_builds_editor = false;
    REQUIRE_FALSE(p.gui->create(p.plugin, platform_api(), false));
    contract_config() = {};
    contract_config().factory_supports_editor = false;
    REQUIRE_FALSE(p.gui->create(p.plugin, platform_api(), false));
    contract_config() = {};

    contract_config().fixed_size = true;
    REQUIRE(p.gui->create(p.plugin, platform_api(), false));
    REQUIRE_FALSE(p.gui->can_resize(p.plugin));
    clap_gui_resize_hints_t hints{};
    REQUIRE(p.gui->get_resize_hints(p.plugin, &hints));
    REQUIRE_FALSE(hints.can_resize_horizontally);
    REQUIRE_FALSE(hints.preserve_aspect_ratio);
    REQUIRE_FALSE(p.gui->set_transient(p.plugin, nullptr));
    p.gui->suggest_title(p.plugin, "ignored");

    clap_window_t wrong{};
    wrong.api = "not-a-window-api";
    REQUIRE_FALSE(p.gui->set_parent(p.plugin, &wrong));
    REQUIRE_FALSE(p.gui->set_parent(p.plugin, nullptr));
    contract_config().attach_succeeds = false;
    const auto window = fake_window();
    REQUIRE_FALSE(p.gui->set_parent(p.plugin, &window));
    contract_config().attach_succeeds = true;
    REQUIRE(p.gui->set_parent(p.plugin, &window));
    REQUIRE_FALSE(p.gui->set_parent(p.plugin, &window)); // already attached
    p.gui->destroy(p.plugin);
}

TEST_CASE("CLAP offers the contract GUI only to a processor with an editor factory",
          "[clap][editor-host][clap-contract]") {
    reset_contract_recording();
    FakeHost host;
    {
        EditorEnvironment env;
        g_processor_has_factory = false;
        OpenPlugin p(host);
        // This build has no create_view() GUI, so there is none at all.
        REQUIRE(p.gui == nullptr);
        REQUIRE(p.plugin->get_extension(p.plugin, CLAP_EXT_TIMER_SUPPORT) == nullptr);
    }
    {
        g_processor_has_factory = true;
        EditorEnvironment env;
        pulp::test_support::ScopedEnvUnset clear_ci("CI");
#if defined(_WIN32)
        _putenv_s("CI", "1");
#else
        ::setenv("CI", "1", 1);
#endif
        OpenPlugin p(host);
        // Editors are refused in CI, as the create_view() GUI is.
        REQUIRE(p.gui == nullptr);
    }
#if PULP_TEST_EDITOR_NEEDS_DISPLAY
    {
        g_processor_has_factory = true;
        EditorEnvironment env;
        pulp::test_support::ScopedEnvUnset no_display("DISPLAY");
        OpenPlugin p(host);
        // No display server to open a window on.
        REQUIRE(p.gui == nullptr);
    }
#endif
    {
        // The control: the same plug-in with editors allowed offers the GUI.
        g_processor_has_factory = true;
        EditorEnvironment env;
        OpenPlugin p(host);
        REQUIRE(p.gui != nullptr);
    }
    g_processor_has_factory = true;
}
