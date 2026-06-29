// Phase 1 — the watch→reload dev loop (ReloadController).
//
// Copies the logic-library fixtures to a watched temp path and drives poll(),
// asserting: the first poll establishes a baseline (no reload); a changed file
// triggers a gated reload (compatible → swap; incompatible → rejected, slot
// untouched); an unchanged file is a no-op; and reload_now() forces a reload.
// Reuses the compatible/incompatible MODULE fixtures via injected paths.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_controller.hpp>
#include <pulp/state/store.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>

using namespace pulp;
using namespace pulp::format::reload;
namespace fs = std::filesystem;

#if !defined(RELOAD_LOGIC_COMPATIBLE) || !defined(RELOAD_LOGIC_INCOMPATIBLE) || \
    !defined(RELOAD_WATCH_DIR)
#error "RELOAD_LOGIC_{COMPATIBLE,INCOMPATIBLE} and RELOAD_WATCH_DIR must be defined"
#endif

namespace {

// Minimal initial DSP: unity-times-gain (param id 1), same contract as the
// compatible fixture (which applies 2x).
class InitialGain final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadGain", .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.gain", .version = "0.1.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "", .range = {0.0f, 2.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        const float g = state().get_value(1) * 1.0f;
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c); auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * g;
        }
    }
};

float render_one(ProcessorHotSwapSlot& slot) {
    constexpr int frames = 64;
    audio::Buffer<float> in(2, frames), out(2, frames);
    for (int n = 0; n < frames; ++n) { in.channel(0)[n] = 1.0f; in.channel(1)[n] = 1.0f; }
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, frames);
    auto ov = out.view();
    midi::MidiBuffer a, b;
    slot.process(ov, iv, a, b, format::ProcessContext{});
    return out.channel(0)[0];
}

// Copy `src` over `watched` and stamp a strictly-increasing mtime so the change
// is observable regardless of filesystem timestamp resolution.
void install(const fs::path& src, const fs::path& watched, int tick) {
    fs::copy_file(src, watched, fs::copy_options::overwrite_existing);
    fs::last_write_time(watched,
                        fs::file_time_type{} + std::chrono::seconds(1000 + tick));
}

}  // namespace

TEST_CASE("ReloadController watches a logic file and reloads on change",
          "[reload][controller]") {
    const fs::path watched = fs::path(RELOAD_WATCH_DIR) / "pulp_reload_watched_logic.dylib";
    install(RELOAD_LOGIC_COMPATIBLE, watched, /*tick=*/0);

    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    ReloadSession session(slot, live, current_build_fingerprint(), format::PrepareContext{});
    ReloadController controller(session, watched);

    // First poll: establish baseline, no reload. Output is the initial unity DSP.
    REQUIRE_FALSE(controller.poll().has_value());
    REQUIRE(render_one(slot) == 0.5f);
    REQUIRE(controller.reload_attempts() == 0);

    // Unchanged file: still a no-op.
    REQUIRE_FALSE(controller.poll().has_value());

    // Developer ships an INCOMPATIBLE build: detected, attempted, rejected at the
    // contract gate, slot left on the previous good processor.
    install(RELOAD_LOGIC_INCOMPATIBLE, watched, /*tick=*/1);
    auto bad = controller.poll();
    REQUIRE(bad.has_value());
    REQUIRE(bad->status == ReloadOutcome::Status::RejectedContract);
    REQUIRE(render_one(slot) == 0.5f);  // unchanged
    REQUIRE(controller.reload_attempts() == 1);

    // The same (incompatible) file on the next tick is NOT retried.
    REQUIRE_FALSE(controller.poll().has_value());
    REQUIRE(controller.reload_attempts() == 1);

    // Developer fixes it: a COMPATIBLE build swaps in live (2x × 0.5 == 1.0),
    // parameter value preserved.
    install(RELOAD_LOGIC_COMPATIBLE, watched, /*tick=*/2);
    auto good = controller.poll();
    REQUIRE(good.has_value());
    REQUIRE(good->ok());
    REQUIRE(render_one(slot) == 1.0f);
    REQUIRE(live.get_value(1) == 0.5f);

    std::error_code ec; fs::remove(watched, ec);
}

TEST_CASE("ReloadController.reload_now forces a reload regardless of mtime",
          "[reload][controller]") {
    const fs::path watched = fs::path(RELOAD_WATCH_DIR) / "pulp_reload_watched_force.dylib";
    install(RELOAD_LOGIC_COMPATIBLE, watched, /*tick=*/0);

    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    ReloadSession session(slot, live, current_build_fingerprint(), format::PrepareContext{});
    ReloadController controller(session, watched);

    REQUIRE(render_one(slot) == 0.5f);   // initial unity
    auto r = controller.reload_now();     // no mtime change needed
    REQUIRE(r.ok());
    REQUIRE(render_one(slot) == 1.0f);    // swapped to 2x
    // After a forced reload the baseline is resynced, so a plain poll is a no-op.
    REQUIRE_FALSE(controller.poll().has_value());

    std::error_code ec; fs::remove(watched, ec);
}
