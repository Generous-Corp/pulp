// test_native_surface_clap.mm - the metal-probe editor (a native Metal
// surface) opened through the CLAP contract GUI, in an offscreen window.
//
// The probe plug-in is built into this test the way a UI-package plug-in is
// (view-free pulp-format-clap-core, no PULP_CLAP_GUI) and loaded through its
// clap_entry. The test plays the host's part: it creates the GUI, gives it an
// NSView, resizes that NSView and calls set_size, and destroys the GUI. What
// the host sees is asserted by the checks shared with the other formats
// (support/native_surface_format_checks.hpp). Skips, saying so, without a
// Metal device. Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include "support/native_surface_format_checks.hpp"
#include "support/recording_contract_editor.hpp"

#include "metal_probe_processor.hpp"

#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/clap_contract_gui.hpp>
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/editor_package.hpp>
#include <pulp/state/store.hpp>

#include <clap/clap.h>
#include <clap/ext/gui.h>

#include <cstring>

#include <pulp/format/clap_entry.hpp>
PULP_CLAP_PLUGIN(pulp_native_metal_probe::create_metal_probe)

using namespace pulp::test_support;

namespace {

struct InstallHook {
    InstallHook() {
        pulp::format::install_plugin_editor_factory_hook(&pulp_native_metal_probe::editor_factory);
    }
} g_install_hook;

const void* host_get_extension(const clap_host_t*, const char*) {
    return nullptr;
}
void host_noop(const clap_host_t*) {}

} // namespace

TEST_CASE("CLAP gives a native-surface editor its size, first frame, resize and reopen",
          "[native-surface][clap][editor-host][format-host]") {
    if (!metal_device_available())
        SKIP("no Metal device on this host; the CLAP native surface was not exercised");
    EditorEnvironment env;
    MainLoop loop;

    clap_host_t host{CLAP_VERSION,
                     nullptr,
                     "pulp-native-surface-test-host",
                     "Pulp",
                     "",
                     "1.0",
                     host_get_extension,
                     host_noop,
                     host_noop,
                     host_noop};
    REQUIRE(clap_entry.init("test"));
    auto* factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t* plugin = factory->create_plugin(factory, &host, desc->id);
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->init(plugin));
    auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    REQUIRE(gui != nullptr);
    auto& store =
        static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data)->store;

    NSView* host_view = nil; // the NSView a CLAP host owns and hands the plug-in
    bool open = false;

    SurfaceFormatDriver driver;
    driver.open = [&](NSWindow* window, float gain) -> NSView* {
        store.set_value(pulp_native_metal_probe::kGain, gain);
        if (!gui->create(plugin, CLAP_WINDOW_API_COCOA, false))
            return nil;
        open = true;
        uint32_t width = 0, height = 0;
        REQUIRE(gui->get_size(plugin, &width, &height));
        host_view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
        [window.contentView addSubview:host_view];
        clap_window_t parent{};
        parent.api = CLAP_WINDOW_API_COCOA;
        parent.cocoa = (__bridge void*)host_view;
        if (!gui->set_parent(plugin, &parent))
            return nil;
        REQUIRE(gui->show(plugin));
        return host_view;
    };
    driver.editor = [&]() -> pulp::format::Editor* {
        if (!open)
            return nullptr;
        auto* session = pulp::format::clap_adapter::contract_gui_session(plugin);
        return session ? session->editor() : nullptr;
    };
    driver.host_resize = [&](std::uint32_t width, std::uint32_t height) {
        // The host sizes its view, then tells the plug-in.
        [host_view setFrameSize:NSMakeSize(width, height)];
        return gui->set_size(plugin, width, height);
    };
    driver.reported_size = [&] {
        uint32_t width = 0, height = 0;
        gui->get_size(plugin, &width, &height);
        return pulp::format::EditorSize{width, height, 1.0};
    };
    driver.close = [&] {
        gui->hide(plugin);
        gui->destroy(plugin);
        open = false;
        [host_view removeFromSuperview];
        host_view = nil;
    };
    driver.run_loop = [&](double seconds) { loop.run_for(seconds); };

    check_native_surface_lifecycle(driver);

    plugin->destroy(plugin);
    clap_entry.deinit();
}
