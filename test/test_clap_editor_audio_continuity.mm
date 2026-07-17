// Editor lifecycle vs. the audio stream.
//
// Opening, resizing, and closing a hosted plugin's editor all happen on the UI
// thread while process() keeps running on the audio thread. Nothing in that
// lifecycle may reach the audio thread: no dropout, no allocation, no NaN. The
// negotiation tests in test_clap_hosted_editor.mm prove the protocol is
// correct; this proves it is inaudible.
//
// The fake plugin emits a continuous full-scale tone, so ANY interruption shows
// up as a silence run in the rendered audio — a dropout cannot hide.
//
// Apple-only for the same reason the feature is: the editor path needs a real
// parent view.

#include <catch2/catch_test_macros.hpp>

#include "../core/host/src/plugin_slot_clap_internal.hpp"
#include "harness/rt_allocation_probe.hpp"
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/host/plugin_slot.hpp>

#import <AppKit/AppKit.h>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace pulp::host;
namespace analysis = pulp::test::audio;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr int kChannels = 2;

/// A CLAP plugin that emits a steady sine and serves a gui. The tone is what
/// makes a dropout visible: silence in the output means the stream broke.
struct TonePlugin {
    clap_plugin_t plugin{};
    clap_plugin_gui_t gui{};
    const clap_host_t* host = nullptr;
    double phase = 0.0;
    std::atomic<int> blocks_rendered{0};
};

TonePlugin* g_tone = nullptr;

// ── gui: enough to open, resize, and close an editor ──────────────────────────

bool CLAP_ABI tone_gui_is_api_supported(const clap_plugin_t*, const char*, bool floating) {
    return !floating;
}
bool CLAP_ABI tone_gui_create(const clap_plugin_t*, const char*, bool) { return true; }
void CLAP_ABI tone_gui_destroy(const clap_plugin_t*) {}
bool CLAP_ABI tone_gui_set_scale(const clap_plugin_t*, double) { return true; }
bool CLAP_ABI tone_gui_get_size(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    *w = 400;
    *h = 300;
    return true;
}
bool CLAP_ABI tone_gui_can_resize(const clap_plugin_t*) { return true; }
bool CLAP_ABI tone_gui_adjust_size(const clap_plugin_t*, uint32_t*, uint32_t*) { return true; }
bool CLAP_ABI tone_gui_set_size(const clap_plugin_t*, uint32_t, uint32_t) { return true; }
bool CLAP_ABI tone_gui_set_parent(const clap_plugin_t*, const clap_window_t*) { return true; }
bool CLAP_ABI tone_gui_show(const clap_plugin_t*) { return true; }
bool CLAP_ABI tone_gui_hide(const clap_plugin_t*) { return true; }

// ── plugin ───────────────────────────────────────────────────────────────────

bool CLAP_ABI tone_init(const clap_plugin_t*) { return true; }
void CLAP_ABI tone_destroy(const clap_plugin_t*) {}
bool CLAP_ABI tone_activate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void CLAP_ABI tone_deactivate(const clap_plugin_t*) {}
bool CLAP_ABI tone_start_processing(const clap_plugin_t*) { return true; }
void CLAP_ABI tone_stop_processing(const clap_plugin_t*) {}
void CLAP_ABI tone_reset(const clap_plugin_t*) {}

clap_process_status CLAP_ABI tone_process(const clap_plugin_t* p, const clap_process_t* proc) {
    auto* t = static_cast<TonePlugin*>(p->plugin_data);
    if (!proc || proc->audio_outputs_count == 0) return CLAP_PROCESS_CONTINUE;

    const double step = 2.0 * M_PI * 440.0 / kSampleRate;
    auto& out = proc->audio_outputs[0];
    for (uint32_t f = 0; f < proc->frames_count; ++f) {
        const auto s = static_cast<float>(std::sin(t->phase) * 0.5);
        t->phase += step;
        if (t->phase > 2.0 * M_PI) t->phase -= 2.0 * M_PI;
        for (uint32_t c = 0; c < out.channel_count; ++c) {
            if (out.data32 && out.data32[c]) out.data32[c][f] = s;
        }
    }
    t->blocks_rendered.fetch_add(1, std::memory_order_relaxed);
    return CLAP_PROCESS_CONTINUE;
}

void CLAP_ABI tone_on_main_thread(const clap_plugin_t*) {}

const void* CLAP_ABI tone_get_extension(const clap_plugin_t* p, const char* id) {
    auto* t = static_cast<TonePlugin*>(p->plugin_data);
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &t->gui;
    return nullptr;
}

clap_plugin_descriptor_t g_tone_desc{};

std::unique_ptr<PluginSlot> make_tone_slot(TonePlugin& tone) {
    g_tone = &tone;
    g_tone_desc.clap_version = CLAP_VERSION_INIT;
    g_tone_desc.id = "pulp.test.tone";
    g_tone_desc.name = "Tone";

    tone.gui.is_api_supported = &tone_gui_is_api_supported;
    tone.gui.create = &tone_gui_create;
    tone.gui.destroy = &tone_gui_destroy;
    tone.gui.set_scale = &tone_gui_set_scale;
    tone.gui.get_size = &tone_gui_get_size;
    tone.gui.can_resize = &tone_gui_can_resize;
    tone.gui.adjust_size = &tone_gui_adjust_size;
    tone.gui.set_size = &tone_gui_set_size;
    tone.gui.set_parent = &tone_gui_set_parent;
    tone.gui.show = &tone_gui_show;
    tone.gui.hide = &tone_gui_hide;

    tone.plugin.desc = &g_tone_desc;
    tone.plugin.plugin_data = &tone;
    tone.plugin.init = &tone_init;
    tone.plugin.destroy = &tone_destroy;
    tone.plugin.activate = &tone_activate;
    tone.plugin.deactivate = &tone_deactivate;
    tone.plugin.start_processing = &tone_start_processing;
    tone.plugin.stop_processing = &tone_stop_processing;
    tone.plugin.reset = &tone_reset;
    tone.plugin.process = &tone_process;
    tone.plugin.get_extension = &tone_get_extension;
    tone.plugin.on_main_thread = &tone_on_main_thread;

    PluginInfo info;
    info.name = "Tone";
    info.format = PluginFormat::CLAP;
    info.num_inputs = kChannels;
    info.num_outputs = kChannels;

    return make_clap_slot(info, [&tone](const clap_host_t* host) -> const clap_plugin_t* {
        tone.host = host;
        return &tone.plugin;
    });
}

class ParentView {
public:
    ParentView() : view_([[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1024, 768)]) {}
    ~ParentView() { [view_ release]; }
    void* handle() const { return (__bridge void*) view_; }

private:
    NSView* view_ = nil;
};

/// Render `blocks` blocks and append them into one contiguous per-channel buffer
/// so the whole stream can be analyzed as a single signal.
struct Recorder {
    std::vector<std::vector<float>> channels{kChannels};

    void render(PluginSlot& slot, int blocks) {
        std::vector<std::vector<float>> out(kChannels, std::vector<float>(kBlockSize, 0.0f));
        std::vector<std::vector<float>> in(kChannels, std::vector<float>(kBlockSize, 0.0f));
        std::vector<float*> out_ptrs, in_ptrs;
        for (auto& c : out) out_ptrs.push_back(c.data());
        for (auto& c : in) in_ptrs.push_back(c.data());

        pulp::midi::MidiBuffer midi_in, midi_out;
        ParameterEventQueue params;

        for (int b = 0; b < blocks; ++b) {
            for (auto& c : out) std::fill(c.begin(), c.end(), 0.0f);
            pulp::audio::BufferView<float> ov(out_ptrs.data(), kChannels, kBlockSize);
            pulp::audio::BufferView<const float> iv(
                const_cast<const float**>(in_ptrs.data()), kChannels, kBlockSize);
            slot.process(ov, iv, midi_in, midi_out, params, kBlockSize);
            for (int c = 0; c < kChannels; ++c) {
                channels[c].insert(channels[c].end(), out[c].begin(), out[c].end());
            }
        }
    }

    analysis::BufferMetrics analyze() {
        std::vector<const float*> ptrs;
        for (auto& c : channels) ptrs.push_back(c.data());
        pulp::audio::BufferView<const float> view(
            ptrs.data(), kChannels, static_cast<int>(channels[0].size()));
        return analysis::analyze(view, kSampleRate);
    }
};

} // namespace

TEST_CASE("Opening and closing an editor does not interrupt the audio stream",
          "[clap][editor][audio]") {
    TonePlugin tone;
    auto slot = make_tone_slot(tone);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->prepare(kSampleRate, kBlockSize));

    ParentView parent;
    Recorder rec;

    // Audio before, during, and after the whole editor lifecycle. The editor
    // work is interleaved between blocks exactly as a UI thread would do it.
    rec.render(*slot, 8);

    auto ed = slot->create_hosted_editor(parent.handle());
    REQUIRE(ed != nullptr);
    rec.render(*slot, 8);

    uint32_t w = 800;
    uint32_t h = 600;
    REQUIRE(slot->set_hosted_editor_size(w, h));
    rec.render(*slot, 8);

    slot->destroy_hosted_editor(std::move(ed));
    rec.render(*slot, 8);

    const auto m = rec.analyze();

    // A tone that never stops: no NaN, no dropout, no clipping.
    REQUIRE_FALSE(m.has_nan_or_inf());
    REQUIRE(m.max_peak() > 0.4);
    REQUIRE(m.total_clipped_samples() == 0);

    // The load-bearing assertion. The plugin emits a 440 Hz sine, whose own
    // zero crossings are the only legitimate silence — at 48 kHz that is a
    // handful of samples. A dropout from an editor call would be a run of
    // block-size length or more.
    for (const auto& ch : m.channels) {
        REQUIRE(ch.longest_silence_run < static_cast<std::uint64_t>(kBlockSize));
    }

    REQUIRE(tone.blocks_rendered.load() == 32);
}

TEST_CASE("Editor calls do not allocate on the audio thread", "[clap][editor][audio][rt-safety]") {
    TonePlugin tone;
    auto slot = make_tone_slot(tone);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->prepare(kSampleRate, kBlockSize));

    ParentView parent;
    auto ed = slot->create_hosted_editor(parent.handle());
    REQUIRE(ed != nullptr);

    std::vector<std::vector<float>> out(kChannels, std::vector<float>(kBlockSize, 0.0f));
    std::vector<std::vector<float>> in(kChannels, std::vector<float>(kBlockSize, 0.0f));
    std::vector<float*> out_ptrs, in_ptrs;
    for (auto& c : out) out_ptrs.push_back(c.data());
    for (auto& c : in) in_ptrs.push_back(c.data());
    pulp::midi::MidiBuffer midi_in, midi_out;
    ParameterEventQueue params;

    pulp::audio::BufferView<float> ov(out_ptrs.data(), kChannels, kBlockSize);
    pulp::audio::BufferView<const float> iv(
        const_cast<const float**>(in_ptrs.data()), kChannels, kBlockSize);

    // Warm any lazy state before the probe so the measurement is about
    // process() itself, not first-call setup.
    slot->process(ov, iv, midi_in, midi_out, params, kBlockSize);

    {
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 16; ++b) {
            slot->process(ov, iv, midi_in, midi_out, params, kBlockSize);
        }
        // An open editor must not add per-block work to the audio thread.
        REQUIRE_FALSE(probe.saw_allocation());
    }

    slot->destroy_hosted_editor(std::move(ed));
}

TEST_CASE("Audio keeps flowing while the editor opens on another thread",
          "[clap][editor][audio]") {
    // The real shape of the risk: process() on the audio thread, editor calls
    // on the UI thread, concurrently rather than interleaved.
    TonePlugin tone;
    auto slot = make_tone_slot(tone);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->prepare(kSampleRate, kBlockSize));

    ParentView parent;
    std::atomic<bool> stop{false};
    std::atomic<int> audio_blocks{0};

    std::thread audio([&] {
        std::vector<std::vector<float>> out(kChannels, std::vector<float>(kBlockSize, 0.0f));
        std::vector<std::vector<float>> in(kChannels, std::vector<float>(kBlockSize, 0.0f));
        std::vector<float*> out_ptrs, in_ptrs;
        for (auto& c : out) out_ptrs.push_back(c.data());
        for (auto& c : in) in_ptrs.push_back(c.data());
        pulp::midi::MidiBuffer midi_in, midi_out;
        ParameterEventQueue params;
        while (!stop.load(std::memory_order_relaxed)) {
            pulp::audio::BufferView<float> ov(out_ptrs.data(), kChannels, kBlockSize);
            pulp::audio::BufferView<const float> iv(
                const_cast<const float**>(in_ptrs.data()), kChannels, kBlockSize);
            slot->process(ov, iv, midi_in, midi_out, params, kBlockSize);
            audio_blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Churn the editor while audio runs.
    for (int i = 0; i < 8; ++i) {
        auto ed = slot->create_hosted_editor(parent.handle());
        REQUIRE(ed != nullptr);
        uint32_t w = 400u + static_cast<uint32_t>(i) * 10u;
        uint32_t h = 300u + static_cast<uint32_t>(i) * 10u;
        slot->set_hosted_editor_size(w, h);
        slot->destroy_hosted_editor(std::move(ed));
    }

    stop.store(true);
    audio.join();

    // Audio kept running throughout — the editor never blocked or wedged it.
    REQUIRE(audio_blocks.load() > 0);
    REQUIRE(tone.blocks_rendered.load() == audio_blocks.load());
}
