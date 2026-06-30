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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
            session_.emplace(slot_, state(), current_build_fingerprint(), prepare_ctx_);
            controller_.emplace(*session_, logic_path_);
        }
        start_watcher();
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
        std::lock_guard<std::mutex> g(controller_mutex_);
        if (!controller_) return {ReloadOutcome::Status::RejectedLoadFailed, "not prepared"};
        auto outcome = controller_->reload_now();
        record(outcome);
        return outcome;
    }

    const std::string& logic_path() const { return logic_path_; }
    std::uint64_t reload_attempts() const { return reload_attempts_.load(std::memory_order_relaxed); }
    std::uint64_t successful_reloads() const { return successful_reloads_.load(std::memory_order_relaxed); }
    ReloadOutcome::Status last_status() const { return last_status_.load(std::memory_order_relaxed); }
    std::uint64_t contention_blocks() const { return slot_.contention_blocks(); }
    bool has_active_dsp() const { return slot_.has_active(); }

private:
    static std::string resolve_logic_path(std::string explicit_path) {
        if (!explicit_path.empty()) return explicit_path;
        if (const char* env = std::getenv("PULP_RELOAD_LOGIC_PATH"); env && *env)
            return std::string(env);
        return {};
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
        ReloadLibrary lib(logic_path_);
        if (!lib.valid()) {
            // Passthrough with NO parameters. A logic that defines parameters
            // cannot be adopted by a later hot-reload (its contract won't match
            // the empty one the host cached) — that needs a full plugin reload.
            // A parameter-less logic will still hot-swap in via the watcher.
            runtime::log_warn("[reload-shell] initial logic load failed: {} — passthrough; "
                              "a logic with parameters needs a full plugin reload to take effect",
                              lib.error());
            return;
        }
        auto abi_fn = lib.symbol<ReloadAbiVersionFn>(kAbiVersionSymbol);
        auto fp_fn = lib.symbol<ReloadFingerprintFn>(kFingerprintSymbol);
        auto create_fn = lib.symbol<ReloadCreateFn>(kCreateSymbol);
        if (!abi_fn || !fp_fn || !create_fn) {
            runtime::log_warn("[reload-shell] initial logic missing reload symbols — passthrough");
            return;
        }
        if (abi_fn() != kReloadAbiVersion) {
            runtime::log_warn("[reload-shell] initial logic reload-ABI mismatch — passthrough");
            return;
        }
        BuildFingerprint logic_fp{};
        fp_fn(&logic_fp);
        if (!fingerprints_match(current_build_fingerprint(), logic_fp)) {
            runtime::log_warn("[reload-shell] initial logic build-fingerprint mismatch — passthrough");
            return;
        }
        std::unique_ptr<Processor> p(create_fn());
        if (!p) {
            runtime::log_warn("[reload-shell] initial logic create returned null — passthrough");
            return;
        }
        descriptor_ = p->descriptor();
        initial_latency_ = p->latency_samples();   // freeze the host-cached latency
        initial_ = std::move(p);
        initial_images_.push_back(std::move(lib));  // keep the code mapped
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
        if (running_.exchange(true)) return;  // already running
        watcher_ = std::thread([this] { watcher_loop(); });
    }

    void stop_watcher() {
        if (!running_.exchange(false)) return;
        if (watcher_.joinable()) watcher_.join();
    }

    void watcher_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> g(controller_mutex_);
                if (controller_) {
                    if (auto outcome = controller_->poll()) record(*outcome);
                }
            }
            // Free a processor whose crossfade has finished (RT-safe reclaim:
            // the audio thread only marks the fade done; we free it here).
            slot_.reclaim();
            // Sleep in small slices so stop_watcher() is responsive.
            for (int i = 0; i < 10 && running_.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(kPollInterval / 10);
        }
    }

    void record(const ReloadOutcome& outcome) {
        reload_attempts_.fetch_add(1, std::memory_order_relaxed);
        last_status_.store(outcome.status, std::memory_order_relaxed);
        if (outcome.ok()) {
            successful_reloads_.fetch_add(1, std::memory_order_relaxed);
            runtime::log_info("[reload-shell] swapped DSP: {}", outcome.detail);
        } else {
            runtime::log_warn("[reload-shell] reload rejected: {}", outcome.detail);
        }
    }

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
    std::atomic<ReloadOutcome::Status> last_status_{ReloadOutcome::Status::RejectedLoadFailed};
};

} // namespace pulp::format::reload
