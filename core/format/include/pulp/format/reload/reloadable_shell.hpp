#pragma once

/// @file reloadable_shell.hpp
/// A ready-made hot-reloadable plugin shell (v2 plan §4.4 / Phase 1b — DAW
/// integration). This is the piece that makes DSP hot-reload work inside a real
/// host (REAPER, Logic, a DAW): a `Processor` the format adapters (VST3 / AU /
/// CLAP / Standalone) wrap exactly like any other, except its DSP lives in a
/// separately-compiled "logic" shared library that can be recompiled and swapped
/// while the host keeps the plugin instance — and the audio stream — alive.
///
/// Split of responsibilities:
///   - The SHELL (this class) owns the audio entry point, the StateStore the host
///     gave it, the RT-safe ProcessorHotSwapSlot, and a control-thread watcher.
///   - The LOGIC library exports the reload ABI (PULP_RELOAD_LOGIC) and contains
///     the actual DSP. The shell dlopens it, gates it (reload-ABI version, build
///     fingerprint, parameter contract), and swaps the new Processor into the
///     slot on success.
///
/// Threading:
///   - `process()` runs on the audio thread and only ever calls
///     `slot_.process()` (a non-blocking try-lock; passthrough on swap
///     contention). It never loads, allocates, or destroys.
///   - A single background WATCHER thread (started in `prepare()`, joined in
///     `release()`/dtor) polls the logic file's mtime and performs the dlopen +
///     gated swap. The slot's writer lock proves no audio reader is inside the
///     old DSP before it is destroyed on this control thread. dlopen / file I/O /
///     ~Processor() therefore never touch the audio thread.
///
/// The parameter contract is FIXED at load time: the shell mirrors the initial
/// logic library's parameters into the host's store, and a reload whose contract
/// differs is rejected (the sound keeps playing on the previous DSP). Changing
/// the automatable parameter set still requires a full plugin reload — only the
/// DSP behind a stable contract hot-swaps. This is by design (the host has
/// already cached the parameter list).
///
/// Logic path resolution (in order): the explicit constructor argument, else the
/// `PULP_RELOAD_LOGIC_PATH` environment variable, else the empty path (the shell
/// runs as passthrough with no parameters and logs a clear diagnostic — e.g. the
/// host scanned the plugin before the developer built the logic library).

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/build_fingerprint.hpp>
#include <pulp/format/reload/processor_hotswap_slot.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/format/reload/reload_controller.hpp>
#include <pulp/format/reload/reload_library.hpp>
#include <pulp/format/reload/reload_transaction.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/view.hpp>  // complete View for create_view() forwarding

#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>   // _getpid
#else
#include <unistd.h>    // getpid
#endif

// Ship-safety gate (live-swap item 1.12 / D5). The dev filesystem WATCHER — the
// background thread that polls a source path and dlopen()s whatever compiled
// artifact appears — is a development affordance, not something a shipped plugin
// should carry. Define PULP_RELOAD_DEV_WATCHER=0 in a release build and the
// watcher thread, its poll loop, and the raw-path ReloadController it drives are
// compiled OUT entirely (no filesystem-watch / auto-dlopen entry point in the
// binary — asserted by the symbol-absence test). The signed SwapTransaction
// surface (`session_` / the hot-swap slot) is deliberately NOT gated: that is
// the path a shipping app-store-style signed content swap would use.
#if !defined(PULP_RELOAD_DEV_WATCHER)
#define PULP_RELOAD_DEV_WATCHER 1
#endif

namespace pulp::format::reload {

class ReloadableShell : public Processor {
public:
    /// Poll interval for the watcher thread. Small enough to feel instant in a
    /// dev loop, large enough not to spin.
    static constexpr std::chrono::milliseconds kPollInterval{150};

    /// Crossfade length applied on each hot-swap so the DSP change is click-free.
    static constexpr double kCrossfadeMs = 12.0;

    /// @param logic_path path to the logic shared library. Empty → resolve from
    /// the `PULP_RELOAD_LOGIC_PATH` environment variable.
    explicit ReloadableShell(std::string logic_path = {})
        : logic_path_(resolve_logic_path(std::move(logic_path))) {
        load_initial();
    }

    ~ReloadableShell() override { stop_watcher(); }

    // ── Processor interface ──────────────────────────────────────────────

    PluginDescriptor descriptor() const override { return descriptor_; }

    // Forward the editor to the ACTIVE logic, so a hot-reload swaps the UI as well
    // as the DSP. A host that keeps the editor across reloads should rebuild it on
    // each swap (see set_on_reloaded) — or the logic should return a self-contained
    // view; see ProcessorHotSwapSlot::create_active_view() for the lifetime note.
    std::unique_ptr<view::View> create_view() override {
        return slot_.with_active([](Processor& p) { return p.create_view(); });
    }

    // Live-swap 1.9: this shell's editor rebuilds IN PLACE on each hot-swap. The
    // ViewBridge hosts create_view() under a stable root container and rebuilds
    // its content whenever editor_reload_generation() changes — polled on the
    // editor idle tick, so the reload notification never crosses threads unsafely
    // (the counter is bumped on the control/watcher thread, read on the UI thread).
    bool supports_editor_reload() const override { return true; }
    std::uint64_t editor_reload_generation() const override {
        return reload_generation_.load(std::memory_order_acquire);
    }

    /// Register a callback fired (on the CONTROL/watcher thread) after each
    /// successful hot-swap — e.g. a standalone host rebuilds its window's editor
    /// from create_view(). The callback must marshal any UI work to the UI thread.
    void set_on_reloaded(std::function<void()> cb) {
        std::lock_guard<std::mutex> g(controller_mutex_);
        on_reloaded_ = std::move(cb);
    }

    // The host caches latency once (for plugin-delay compensation), so it is part
    // of the frozen contract: report the initial logic's latency, and a reload
    // that changes latency is rejected (see prepare()/the reload note below).
    int latency_samples() const override { return initial_latency_; }

    void define_parameters(state::StateStore& store) override {
        // Mirror the initial logic's parameter contract so the host sees the
        // automatable surface the DSP expects. Reloads must match this contract.
        if (initial_) initial_->define_parameters(store);
    }

    void prepare(const PrepareContext& context) override {
        // A host may call prepare() again (e.g. a sample-rate change). Join any
        // running watcher BEFORE touching session_/controller_: the watcher holds
        // a reference into them via poll(), and controller_ holds a ReloadSession&
        // — re-emplacing under a live watcher would be a data race / dangling ref.
        stop_watcher();
        prepare_ctx_ = context;
        if (initial_) {
            initial_->set_state_store(&state());
            initial_->prepare(context);
            (void)slot_.swap(std::move(initial_));   // install the starting DSP (first install never fades)
            initial_.reset();
        } else {
            // Re-prepare (e.g. a sample-rate change): the DSP already in the slot
            // must see the new context, or it keeps running at the stale rate
            // (tempo-synced LFOs, delay-line sizes). Audio is stopped during
            // prepare(), and reprepare_active() takes the slot's writer lock, so
            // this can't race the audio thread.
            slot_.reprepare_active(context);
        }
        // Enable a short click-free crossfade on hot-swap: allocate the
        // parallel-render scratch here (off the audio thread), sized to the
        // worst-case block, and set the fade length from the sample rate.
        const double sr = prepare_ctx_.sample_rate > 0 ? prepare_ctx_.sample_rate : 48000.0;
        const std::size_t max_ch = static_cast<std::size_t>(
            std::max({context.input_channels, context.output_channels, 2}));
        const std::size_t max_frames =
            static_cast<std::size_t>(std::max(context.max_buffer_size, 1));
        slot_.prepare_crossfade(max_frames, max_ch);
        slot_.set_crossfade_samples(static_cast<std::size_t>(kCrossfadeMs * 0.001 * sr));

        // (Re)build the session + controller against the live store. Constructed
        // here (not in the ctor) because they need the host-provided store,
        // prepare context, and the running slot.
        {
            std::lock_guard<std::mutex> g(controller_mutex_);
            // session_ is the signed SwapTransaction surface — always present so
            // a shipping build can still adopt a signed content swap.
            session_.emplace(slot_, state(), current_build_fingerprint(), prepare_ctx_);
#if PULP_RELOAD_DEV_WATCHER
            // The raw-path controller (dev filesystem reload) is compiled out of
            // a shipping build; without it reload_now() / the watcher have no
            // path to dlopen an arbitrary on-disk artifact.
            controller_.emplace(*session_, logic_path_);
#endif
        }
        start_watcher();  // no-op (compiled out) when the watcher is gated off
    }

    void release() override { stop_watcher(); }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext& context) override {
        // Audio thread: forward to the live DSP. On swap contention the slot
        // passes input through for one block — never blocks/allocates/destroys.
        slot_.process(output, input, midi_in, midi_out, context);
    }

    // ── Diagnostics (control-thread / test use) ──────────────────────────

    /// Force a reload now (e.g. a "reload" UI command), bypassing the mtime
    /// gate. Runs on the CALLING thread — must not be the audio thread.
    ReloadOutcome reload_now() {
        ReloadOutcome outcome;
        std::function<void()> on_reloaded;
        {
            std::lock_guard<std::mutex> g(controller_mutex_);
            if (!controller_) return {ReloadOutcome::Status::RejectedLoadFailed, "not prepared"};
            outcome = controller_->reload_now();
            record(outcome);
            if (outcome.ok()) {
                reload_generation_.fetch_add(1, std::memory_order_release);  // editor rebuild (1.9)
                on_reloaded = on_reloaded_snapshot();
            }
        }
        if (on_reloaded) on_reloaded();   // fire AFTER releasing the lock (re-entrant-safe)
        return outcome;
    }

    const std::string& logic_path() const { return logic_path_; }
    std::uint64_t reload_attempts() const { return reload_attempts_.load(std::memory_order_relaxed); }
    std::uint64_t successful_reloads() const { return successful_reloads_.load(std::memory_order_relaxed); }
    ReloadOutcome::Status last_status() const { return last_status_.load(std::memory_order_relaxed); }
    /// Total wall-clock of the last successful DSP reload, ms (item 1.2 diagnostic).
    double last_reload_ms() const { return last_reload_ms_.load(std::memory_order_relaxed); }
    std::uint64_t contention_blocks() const { return slot_.contention_blocks(); }
    bool has_active_dsp() const { return slot_.has_active(); }

private:
    static std::string resolve_logic_path(std::string explicit_path) {
        if (!explicit_path.empty()) return explicit_path;
        if (const char* env = std::getenv("PULP_RELOAD_LOGIC_PATH"); env && *env)
            return std::string(env);
        return {};
    }

    // Copy the watched logic to a private, uniquely-named sibling and return that
    // path; the initial image is loaded from it so the live mapping never sits on
    // the developer-overwritten watched path (see load_initial). Unique per
    // process (pid) and per instance (a monotonic counter) so two plugin
    // instances — even in separate host processes — never stage onto each other's
    // live mapping. Staged beside the original (dlopen-safe: a world-writable temp
    // dir trips macOS dyld's unsigned-dylib kill). Best-effort: on any copy
    // failure, fall back to the original path (the corruption only bites when the
    // watched file is overwritten while the initial image is still live).
    static std::string stage_initial(const std::string& logic_path) {
        namespace fs = std::filesystem;
        static std::atomic<std::uint64_t> counter{0};
        const long pid =
#if defined(_WIN32)
            static_cast<long>(::_getpid());
#else
            static_cast<long>(::getpid());
#endif
        std::error_code ec;
        const fs::path src(logic_path);
        const fs::path staged =
            src.parent_path() /
            (src.stem().string() + ".initial." + std::to_string(pid) + "." +
             std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
             src.extension().string());
        fs::copy_file(src, staged, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            runtime::log_warn("[reload-shell] could not stage initial logic ({}); "
                              "loading in place — a rebuild that overwrites it while "
                              "live may corrupt the image on Linux", ec.message());
            return logic_path;
        }
        return staged.string();
    }

    // Load the initial logic image so descriptor()/define_parameters() reflect
    // the DSP's real contract. Gated like a reload (ABI version + fingerprint)
    // so a stale/incompatible bundled library degrades to passthrough instead of
    // crashing. The image is retained for the shell's lifetime because it backs
    // the initial Processor's code until that Processor is swapped out and
    // destroyed.
    void load_initial() {
        descriptor_ = default_descriptor();
        if (logic_path_.empty()) {
            runtime::log_info("[reload-shell] no logic path (set PULP_RELOAD_LOGIC_PATH "
                              "or pass one); running as passthrough");
            return;
        }
        // Load the initial image from a PRIVATE staged copy, never the watched
        // path directly. The watched path is the developer's build output, which a
        // rebuild overwrites — often in place (truncate + rewrite of the same
        // inode, as a `cp`/install step or `std::filesystem::copy_file` does, vs a
        // temp-file-then-rename that yields a fresh inode). dlopen keeps the file
        // mapped; on Linux an in-place overwrite of a mapped image bleeds the new
        // bytes into the still-live mapping (MAP_PRIVATE pages not yet
        // copy-on-written), corrupting the initial Processor's code and vtable.
        // That goes unnoticed until the initial Processor is rendered as the
        // fading-out side of the first hot-swap's crossfade — then a virtual call
        // jumps through an unrelocated vtable and crashes. Staging puts the live
        // mapping on a path no rebuild touches, so the watched path can be
        // overwritten freely. (The reload path already stages every version; see
        // ReloadController. macOS tolerates the in-place overwrite, so this only
        // bites on Linux.)
        const std::string load_path = stage_initial(logic_path_);

        // Same fail-closed gate sequence the reload transaction uses (single
        // source of truth — gate_logic_image). On rejection we degrade to
        // passthrough; note a logic that defines parameters can't be adopted by a
        // later hot-reload (its contract won't match the empty one the host
        // cached) — that needs a full plugin reload. A parameter-less logic still
        // hot-swaps in via the watcher.
        auto gated = gate_logic_image(load_path, current_build_fingerprint());
        if (auto* rejected = std::get_if<ReloadOutcome>(&gated)) {
            runtime::log_warn("[reload-shell] initial logic rejected ({}) — passthrough; "
                              "a logic with parameters needs a full plugin reload to take effect",
                              rejected->detail);
            // Surface the structured per-field diff (fingerprint/contract) so a
            // rejection says WHAT differs (e.g. cpp_standard host vs logic), not
            // just "mismatch" — the difference between a 5-minute fix and an hour.
            for (const auto& issue : rejected->issues)
                runtime::log_warn("[reload-shell]   diff: {}", issue);
            return;
        }
        GatedImage& image = std::get<GatedImage>(gated);
        std::unique_ptr<Processor> p(image.create());
        if (!p) {
            runtime::log_warn("[reload-shell] initial logic create returned null — passthrough");
            return;
        }
        descriptor_ = p->descriptor();
        initial_latency_ = p->latency_samples();        // freeze the host-cached latency
        initial_ = std::move(p);
        initial_images_.push_back(std::move(image.lib)); // keep the code mapped
        runtime::log_info("[reload-shell] loaded initial logic: {}", logic_path_);
    }

    static PluginDescriptor default_descriptor() {
        return {.name = "Pulp Reloadable",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.reloadable-shell",
                .version = "1.0.0",
                .category = PluginCategory::Effect,
                .input_buses = {{"Audio In", 2}},
                .output_buses = {{"Audio Out", 2}}};
    }

    void start_watcher() {
#if PULP_RELOAD_DEV_WATCHER
        if (running_.exchange(true)) return;  // already running
        watcher_ = std::thread([this] { watcher_loop(); });
#endif
    }

    void stop_watcher() {
#if PULP_RELOAD_DEV_WATCHER
        if (!running_.exchange(false)) return;
        if (watcher_.joinable()) watcher_.join();
#endif
    }

#if PULP_RELOAD_DEV_WATCHER
    // PULP_RELOAD_SHIP_FIXTURE_MARK (test-only) force-emits this symbol so the
    // ship-safety symbol-absence test has a deterministic marker: present in the
    // dev fixture, gone from the gate-off ship fixture. Normal builds set nothing
    // here — the method emits only when ODR-used, as usual.
#if defined(PULP_RELOAD_SHIP_FIXTURE_MARK)
    [[gnu::used]]
#endif
    void watcher_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            std::function<void()> on_reloaded;
            {
                std::lock_guard<std::mutex> g(controller_mutex_);
                if (controller_) {
                    if (auto outcome = controller_->poll()) {
                        record(*outcome);
                        if (outcome->ok()) {
                            reload_generation_.fetch_add(1, std::memory_order_release);  // editor rebuild (1.9)
                            on_reloaded = on_reloaded_snapshot();
                        }
                    }
                }
            }
            if (on_reloaded) on_reloaded();   // fire outside the lock (re-entrant-safe)
            // Free a processor whose crossfade has finished (RT-safe reclaim:
            // the audio thread only marks the fade done; we free it here).
            slot_.reclaim();
            // Sleep in small slices so stop_watcher() is responsive.
            for (int i = 0; i < 10 && running_.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(kPollInterval / 10);
        }
    }
#endif

    void record(const ReloadOutcome& outcome) {
        reload_attempts_.fetch_add(1, std::memory_order_relaxed);
        last_status_.store(outcome.status, std::memory_order_relaxed);
        if (outcome.ok()) {
            successful_reloads_.fetch_add(1, std::memory_order_relaxed);
            last_reload_ms_.store(outcome.metrics.total_ms, std::memory_order_relaxed);
            // `swapped in NNN ms` dev diagnostic + per-phase breakdown (item 1.2).
            const auto& m = outcome.metrics;
            runtime::log_info(
                "[reload-shell] swapped DSP in {:.2f} ms "
                "(load+gate {:.2f} / construct {:.2f} / prepare {:.2f} / swap {:.2f}): {}",
                m.total_ms, m.load_gate_ms, m.construct_ms, m.prepare_ms, m.swap_ms,
                outcome.detail);
        } else {
            runtime::log_warn("[reload-shell] reload rejected after {:.2f} ms: {}",
                              outcome.metrics.total_ms, outcome.detail);
            // Surface the structured per-field diff so a rejection says WHAT
            // differs (e.g. which parameter's contract changed), not just the
            // category — the difference between a 5-minute fix and an hour.
            for (const auto& issue : outcome.issues)
                runtime::log_warn("[reload-shell]   reject-diff: {}", issue);
        }
    }

    // Snapshot the on-reloaded callback (caller holds controller_mutex_). The
    // caller fires it AFTER releasing the lock, so a callback that re-enters the
    // shell (reload_now / create_view) can't self-deadlock on controller_mutex_.
    std::function<void()> on_reloaded_snapshot() const { return on_reloaded_; }

    std::string logic_path_;
    PluginDescriptor descriptor_;
    int initial_latency_ = 0;                     // host-cached latency of the initial logic
    std::unique_ptr<Processor> initial_;          // pre-prepare; moved into slot_
    std::vector<ReloadLibrary> initial_images_;   // retains the initial logic's code
    PrepareContext prepare_ctx_;
    ProcessorHotSwapSlot slot_;

    std::mutex controller_mutex_;                 // serializes poll() vs reload_now()
    std::optional<ReloadSession> session_;
    std::optional<ReloadController> controller_;

    std::thread watcher_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> reload_attempts_{0};
    std::atomic<std::uint64_t> successful_reloads_{0};
    std::atomic<double> last_reload_ms_{0.0};     // wall-clock of the last swap (item 1.2)
    std::atomic<ReloadOutcome::Status> last_status_{ReloadOutcome::Status::RejectedLoadFailed};
    std::atomic<std::uint64_t> reload_generation_{0};  // editor rebuild counter (1.9; UI-thread read)
    std::function<void()> on_reloaded_;           // host editor-rebuild hook (control thread)
};

} // namespace pulp::format::reload
