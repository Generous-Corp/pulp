// ReloadableShell — the DAW-integration shell, end to end (Phase 1b).
//
// Drives the shell through the SAME Processor interface a format adapter
// (VST3 / AU / CLAP) uses — descriptor()/define_parameters()/prepare()/process()
// — and proves a real dylib hot-swap is observable in the audio output without
// re-instantiating the shell or stopping the (simulated) audio stream:
//   - the shell mirrors the initial logic's parameter contract;
//   - a forced reload to a contract-compatible 2x build swaps the DSP live
//     (output goes 1x -> 2x), preserving the parameter value;
//   - a contract-INCOMPATIBLE build is rejected, leaving the live DSP untouched;
//   - the background file-watcher performs the same swap on an mtime change.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/reloadable_shell.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace pulp;
using namespace pulp::format::reload;
namespace fs = std::filesystem;

#if !defined(RELOAD_LOGIC_UNITY) || !defined(RELOAD_LOGIC_COMPATIBLE) || \
    !defined(RELOAD_LOGIC_INCOMPATIBLE) || !defined(RELOAD_WATCH_DIR)
#error "RELOAD_LOGIC_{UNITY,COMPATIBLE,INCOMPATIBLE} and RELOAD_WATCH_DIR must be defined"
#endif

namespace {

// Render one block of all-ones input through the shell and return out[0][0].
float render_one(format::Processor& shell, float /*unused*/ = 0) {
    constexpr int frames = 64;
    audio::Buffer<float> in(2, frames), out(2, frames);
    for (int n = 0; n < frames; ++n) { in.channel(0)[n] = 1.0f; in.channel(1)[n] = 1.0f; }
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, frames);
    auto ov = out.view();
    midi::MidiBuffer a, b;
    shell.process(ov, iv, a, b, format::ProcessContext{});
    return out.channel(0)[0];
}

// Copy `src` over `watched` with a strictly-increasing mtime so the change is
// observable regardless of filesystem timestamp resolution.
void install(const fs::path& src, const fs::path& watched, int tick) {
    fs::copy_file(src, watched, fs::copy_options::overwrite_existing);
    fs::last_write_time(watched, fs::file_time_type{} + std::chrono::seconds(2000 + tick));
}

}  // namespace

TEST_CASE("ReloadableShell hot-swaps DSP through the Processor path", "[reload][shell]") {
    const fs::path watched = fs::path(RELOAD_WATCH_DIR) / "pulp_shell_watched_logic.dylib";
    install(RELOAD_LOGIC_UNITY, watched, /*tick=*/0);

    // Construct + wire exactly as a format adapter would.
    ReloadableShell shell(watched.string());
    state::StateStore store;
    shell.define_parameters(store);
    REQUIRE(store.all_params().size() == 1);   // mirrored the logic's contract
    shell.set_state_store(&store);
    store.set_value(1, 0.5f);

    format::PrepareContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.max_buffer_size = 64;
    shell.prepare(ctx);                         // installs unity DSP + starts watcher

    // Initial DSP: unity gain * 0.5.
    REQUIRE(render_one(shell) == 0.5f);
    REQUIRE(shell.has_active_dsp());

    // Force a reload to the compatible 2x build (contract matches): the DSP
    // swaps live and the parameter value is preserved (2x * 0.5 == 1.0).
    install(RELOAD_LOGIC_COMPATIBLE, watched, /*tick=*/1);
    auto good = shell.reload_now();
    REQUIRE(good.ok());
    REQUIRE(render_one(shell) == 1.0f);
    REQUIRE(store.get_value(1) == 0.5f);
    // >= 1, not == 1: the background watcher may also have detected the same
    // mtime change and swapped before reload_now() forced its swap. Either way
    // the DSP is the 2x build; the count just isn't deterministic here.
    REQUIRE(shell.successful_reloads() >= 1);

    // A contract-incompatible build (extra param) is rejected; the live DSP is
    // left on the previous good processor — audio keeps playing the 2x build.
    install(RELOAD_LOGIC_INCOMPATIBLE, watched, /*tick=*/2);
    auto bad = shell.reload_now();
    REQUIRE_FALSE(bad.ok());
    REQUIRE(bad.status == ReloadOutcome::Status::RejectedContract);
    REQUIRE(render_one(shell) == 1.0f);         // unchanged

    shell.release();                            // joins the watcher
    std::error_code ec; fs::remove(watched, ec);
}

TEST_CASE("ReloadableShell background watcher swaps on an mtime change", "[reload][shell]") {
    const fs::path watched = fs::path(RELOAD_WATCH_DIR) / "pulp_shell_watched_watch.dylib";
    install(RELOAD_LOGIC_UNITY, watched, /*tick=*/0);

    ReloadableShell shell(watched.string());
    state::StateStore store;
    shell.define_parameters(store);
    shell.set_state_store(&store);
    store.set_value(1, 0.5f);
    format::PrepareContext ctx; ctx.sample_rate = 48000.0; ctx.max_buffer_size = 64;
    shell.prepare(ctx);
    REQUIRE(render_one(shell) == 0.5f);         // unity

    // Developer recompiles: drop a 2x build at the watched path. The background
    // watcher (poll ~150ms) must pick it up and swap with no further calls.
    install(RELOAD_LOGIC_COMPATIBLE, watched, /*tick=*/1);

    bool swapped = false;
    for (int i = 0; i < 100 && !swapped; ++i) {            // up to ~5s, generous
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (shell.successful_reloads() >= 1) swapped = true;
    }
    REQUIRE(swapped);
    REQUIRE(render_one(shell) == 1.0f);          // 2x * 0.5
    REQUIRE(shell.last_status() == ReloadOutcome::Status::Swapped);

    shell.release();
    std::error_code ec; fs::remove(watched, ec);
}

TEST_CASE("ReloadableShell with no logic path runs as passthrough", "[reload][shell]") {
    ReloadableShell shell("");                  // no path, no env
    state::StateStore store;
    shell.define_parameters(store);
    REQUIRE(store.all_params().empty());        // nothing to mirror
    shell.set_state_store(&store);
    format::PrepareContext ctx; ctx.sample_rate = 48000.0; ctx.max_buffer_size = 64;
    shell.prepare(ctx);
    // No active DSP -> the slot passes input straight through.
    REQUIRE(render_one(shell) == 1.0f);
    REQUIRE(shell.contention_blocks() >= 1);
    shell.release();
}
