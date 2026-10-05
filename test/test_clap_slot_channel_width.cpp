// CLAP slot channel-width negotiation (core/host/src/plugin_slot_clap.cpp).
//
// A CLAP plugin declares its audio ports statically, so a host cannot
// renegotiate the width the way it can for an AU. Handing a plugin a buffer
// shape its ports do not accept makes a conforming plugin return without
// writing its output, and the caller sees a successful render of pure silence —
// a wrong answer presented as data. prepare() refuses the request instead.
//
// Driven against a fake clap_plugin_t so the contract is pinned as C-ABI
// behaviour with no dlopen and no bundle on disk.

#include <catch2/catch_test_macros.hpp>

#include "../core/host/src/plugin_slot_clap_internal.hpp"
#include <pulp/host/plugin_slot.hpp>

#include <clap/clap.h>

#include <memory>

using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::PluginSlot;

namespace {

// A plugin that declares exactly `in` x `out` channels on one audio port each,
// and otherwise does nothing. `ports_declared` false omits the audio-ports
// extension entirely, which is the "nothing is known" case prepare() must not
// police.
struct FakePortPlugin {
    uint32_t in_channels = 1;
    uint32_t out_channels = 1;
    bool ports_declared = true;

    clap_plugin_t plugin{};
    clap_plugin_audio_ports_t audio_ports{};
};

FakePortPlugin* g_fake = nullptr;

uint32_t CLAP_ABI fake_ports_count(const clap_plugin_t*, bool /*is_input*/) { return 1; }

bool CLAP_ABI fake_ports_get(const clap_plugin_t*, uint32_t index, bool is_input,
                             clap_audio_port_info_t* info) {
    if (index != 0 || info == nullptr || g_fake == nullptr) return false;
    *info = clap_audio_port_info_t{};
    info->id = is_input ? 0 : 1;
    info->channel_count = is_input ? g_fake->in_channels : g_fake->out_channels;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    return true;
}

bool CLAP_ABI fake_init(const clap_plugin_t*) { return true; }
void CLAP_ABI fake_destroy(const clap_plugin_t*) {}
bool CLAP_ABI fake_activate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void CLAP_ABI fake_deactivate(const clap_plugin_t*) {}
bool CLAP_ABI fake_start_processing(const clap_plugin_t*) { return true; }
void CLAP_ABI fake_stop_processing(const clap_plugin_t*) {}
void CLAP_ABI fake_reset(const clap_plugin_t*) {}
clap_process_status CLAP_ABI fake_process(const clap_plugin_t*, const clap_process_t*) {
    return CLAP_PROCESS_CONTINUE;
}
void CLAP_ABI fake_on_main_thread(const clap_plugin_t*) {}

const void* CLAP_ABI fake_get_extension(const clap_plugin_t*, const char* id) {
    if (g_fake == nullptr || !g_fake->ports_declared) return nullptr;
    if (std::string_view(id) == CLAP_EXT_AUDIO_PORTS) return &g_fake->audio_ports;
    return nullptr;
}

std::unique_ptr<PluginSlot> slot_for(FakePortPlugin& fake) {
    g_fake = &fake;
    fake.audio_ports.count = fake_ports_count;
    fake.audio_ports.get = fake_ports_get;
    fake.plugin.init = fake_init;
    fake.plugin.destroy = fake_destroy;
    fake.plugin.activate = fake_activate;
    fake.plugin.deactivate = fake_deactivate;
    fake.plugin.start_processing = fake_start_processing;
    fake.plugin.stop_processing = fake_stop_processing;
    fake.plugin.reset = fake_reset;
    fake.plugin.process = fake_process;
    fake.plugin.on_main_thread = fake_on_main_thread;
    fake.plugin.get_extension = fake_get_extension;

    PluginInfo info;
    info.name = "Fake Mono Ports";
    info.format = PluginFormat::CLAP;
    return pulp::host::make_clap_slot(info, [&fake](const clap_host_t*) {
        return &fake.plugin;
    });
}

}  // namespace

TEST_CASE("CLAP prepare refuses a width the plugin's ports cannot serve",
          "[host][clap][channels]") {
    // The control first: the declared width prepares, so a refusal below is the
    // width mismatch and not a broken fake.
    SECTION("the declared width prepares") {
        FakePortPlugin fake;  // 1 x 1
        auto slot = slot_for(fake);
        REQUIRE(slot != nullptr);
        slot->set_preferred_channel_layout(1, 1);
        CHECK(slot->prepare(48000.0, 512));
    }

    SECTION("a wider request is refused rather than rendering silence") {
        FakePortPlugin fake;  // declares 1 x 1
        auto slot = slot_for(fake);
        REQUIRE(slot != nullptr);
        slot->set_preferred_channel_layout(2, 2);
        CHECK_FALSE(slot->prepare(48000.0, 512));
    }

    SECTION("a mismatch on either side alone is enough") {
        FakePortPlugin fake;
        auto slot = slot_for(fake);
        slot->set_preferred_channel_layout(1, 2);
        CHECK_FALSE(slot->prepare(48000.0, 512));

        FakePortPlugin fake2;
        auto slot2 = slot_for(fake2);
        slot2->set_preferred_channel_layout(2, 1);
        CHECK_FALSE(slot2->prepare(48000.0, 512));
    }

    SECTION("a stereo plugin serves a stereo request") {
        FakePortPlugin fake;
        fake.in_channels = 2;
        fake.out_channels = 2;
        auto slot = slot_for(fake);
        slot->set_preferred_channel_layout(2, 2);
        CHECK(slot->prepare(48000.0, 512));
    }

    SECTION("no request means nothing is policed") {
        // The graph and other callers never call set_preferred_channel_layout,
        // and must keep their existing behaviour.
        FakePortPlugin fake;
        auto slot = slot_for(fake);
        CHECK(slot->prepare(48000.0, 512));
    }

    SECTION("a plugin declaring no audio ports is not policed either") {
        FakePortPlugin fake;
        fake.ports_declared = false;
        auto slot = slot_for(fake);
        slot->set_preferred_channel_layout(2, 2);
        CHECK(slot->prepare(48000.0, 512));
    }
}
