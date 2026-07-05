#pragma once

/// @file reload_transaction.hpp
/// The verify-before-commit reload transaction (v2 plan §4.4 / Phase 1) — ties
/// the Phase-0 primitives into one operation: load a candidate logic library,
/// gate it on reload-ABI version, build-fingerprint, and parameter-contract
/// compatibility, and only then construct + swap it into the live
/// ProcessorHotSwapSlot.
///
/// Ordering is deliberately fail-closed: every gate that can reject does so
/// BEFORE the audio-visible swap, so a bad candidate never reaches the audio
/// thread. The displaced processor is returned by the slot and destroyed on the
/// CALLER's (control) thread.
///
/// State continuity (one canonical answer): the new processor binds to the
/// caller's LIVE StateStore, which already holds the current parameter values
/// and — by the contract gate — the same parameter set, so the swap preserves
/// the sound with no value copying. param_contract.hpp::carry_state() is the
/// ALTERNATE model (copy live values into a processor that owns its own store);
/// it is NOT used on this shell path and exists for hosts that give each
/// processor an independent store. The transaction is the canonical path.
///
/// Most callers should use ReloadSession (below), which owns the session-stable
/// state; reload_processor_from_library() is the stateless core it delegates to.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/build_fingerprint.hpp>
#include <pulp/format/reload/param_contract.hpp>
#include <pulp/format/reload/processor_hotswap_slot.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/format/reload/reload_library.hpp>
#include <pulp/state/store.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace pulp::format::reload {

/// DSP-axis reload timings (control-thread, off the audio path). Populated by
/// reload_processor_from_library() on every attempt — partial on early
/// rejection (later phases stay 0). Milliseconds. Feeds the `swapped in NNN ms`
/// dev diagnostic and p50/p95 baselines.
struct ReloadMetrics {
    double load_gate_ms = 0.0;  ///< dlopen + ABI/fingerprint gates + resolve create.
    double construct_ms = 0.0;  ///< create() + define_parameters + contract gate.
    double prepare_ms = 0.0;    ///< the candidate's prepare().
    double swap_ms = 0.0;       ///< slot.swap() + displaced dtor (this thread).
    double total_ms = 0.0;      ///< end-to-end (load → swap).
};

struct ReloadOutcome {
    enum class Status {
        Swapped,               ///< Success: the new processor is live.
        RejectedLoadFailed,    ///< dlopen / LoadLibrary failed.
        RejectedAbiVersion,    ///< Built against a different reload-ABI version.
        RejectedNoEntryPoints, ///< Missing a required symbol, or create returned null.
        RejectedFingerprint,   ///< C++-ABI-incompatible build (fingerprint mismatch).
        RejectedContract,      ///< Parameter contract differs (needs full reload).
        RejectedCandidateThrew,///< create/define/prepare threw before the swap.
    };
    Status status = Status::RejectedLoadFailed;
    std::string detail;               ///< Short human summary (one line).
    std::vector<std::string> issues;  ///< Structured per-field diffs (fingerprint/contract).
    ReloadMetrics metrics;            ///< DSP-axis phase timings (item 1.2).

    bool ok() const { return status == Status::Swapped; }
    explicit operator bool() const { return ok(); }
};

/// A logic image that passed the pre-construction gates (reload-ABI version +
/// build fingerprint) with its factory resolved. The caller adopts @p lib (and
/// must retain it for as long as anything it constructs lives — see the leak
/// policy) and calls @p create to instantiate the logic Processor.
struct GatedImage {
    ReloadLibrary lib;
    ReloadCreateFn create = nullptr;
};

/// Load @p library_path and run the pre-construction gates — reload-ABI version,
/// then build fingerprint — and resolve the create symbol. The SINGLE source of
/// truth for that gate ordering, shared by reload_processor_from_library() and
/// ReloadableShell's initial load so they can never disagree about what
/// "compatible" means. Returns the GatedImage on success (caller retains lib +
/// calls create), or a fail-closed ReloadOutcome on rejection (the image backed
/// no C++ object, so it is set CloseOnDestroy and unloaded rather than leaked).
inline std::variant<GatedImage, ReloadOutcome>
gate_logic_image(const std::string& library_path, const BuildFingerprint& host_fingerprint) {
    auto reject_quiescible = [](ReloadLibrary& lib, ReloadOutcome::Status status,
                                std::string detail, std::vector<std::string> issues = {}) {
        lib.set_leak_policy(LeakPolicy::CloseOnDestroy);
        return ReloadOutcome{status, std::move(detail), std::move(issues)};
    };

    // 1. Load the image. A failed load has no handle to retain or close.
    ReloadLibrary lib(library_path);
    if (!lib.valid()) return ReloadOutcome{ReloadOutcome::Status::RejectedLoadFailed, lib.error()};

    // 2. Reload-ABI-version gate — coarsest compatibility check, first.
    auto abi_version_fn = lib.symbol<ReloadAbiVersionFn>(kAbiVersionSymbol);
    if (!abi_version_fn)
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedNoEntryPoints,
                                 "missing reload-ABI-version symbol");
    if (const int v = abi_version_fn(); v != kReloadAbiVersion)
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedAbiVersion,
                                 "reload ABI version " + std::to_string(v) +
                                     " != host " + std::to_string(kReloadAbiVersion));

    // 3. Build-fingerprint gate — refuse a C++-ABI-incompatible image before
    //    constructing anything across the seam.
    auto fingerprint_fn = lib.symbol<ReloadFingerprintFn>(kFingerprintSymbol);
    if (!fingerprint_fn)
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedNoEntryPoints,
                                 "missing fingerprint symbol");
    BuildFingerprint logic_fingerprint{};
    fingerprint_fn(&logic_fingerprint);
    if (!fingerprints_match(host_fingerprint, logic_fingerprint))
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedFingerprint,
                                 "build fingerprint mismatch",
                                 fingerprint_diff(host_fingerprint, logic_fingerprint));

    // 4. Resolve the factory.
    auto create_fn = lib.symbol<ReloadCreateFn>(kCreateSymbol);
    if (!create_fn)
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedNoEntryPoints,
                                 "missing create symbol");

    return GatedImage{std::move(lib), create_fn};
}

/// Attempt to hot-reload @p slot from the logic library at @p library_path.
/// Stateless core — see ReloadSession for the owning convenience wrapper.
///
/// @param slot              the live RT-safe slot to swap into.
/// @param library_path      path to the candidate logic shared library.
/// @param host_fingerprint  the shell's own build fingerprint (compare target).
/// @param live_store        the live parameter store; the new processor binds to
///                          it (must outlive the swapped-in processor).
/// @param prepare_ctx       prepare() arguments (sample rate / block size).
/// @param retained_images   images that backed a constructed processor are
///                          appended here and kept alive for the process
///                          lifetime; images rejected before any construction
///                          are unloaded immediately.
/// @returns an outcome; only Status::Swapped means the slot changed.
inline ReloadOutcome reload_processor_from_library(
    ProcessorHotSwapSlot& slot,
    const std::string& library_path,
    const BuildFingerprint& host_fingerprint,
    state::StateStore& live_store,
    const PrepareContext& prepare_ctx,
    std::vector<ReloadLibrary>& retained_images) {

    // Phase timing (item 1.2). steady_clock; this whole function is control-thread
    // (off the audio path), so timing here is never an RT concern.
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // 1-4. Load + gate (ABI version, fingerprint) + resolve the factory — the
    //       shared, fail-closed gate sequence (see gate_logic_image).
    auto gated = gate_logic_image(library_path, host_fingerprint);
    const auto t_gate = clock::now();
    if (auto* rejected = std::get_if<ReloadOutcome>(&gated)) {
        rejected->metrics.load_gate_ms = ms(t0, t_gate);
        rejected->metrics.total_ms = ms(t0, t_gate);
        return std::move(*rejected);
    }
    GatedImage& image = std::get<GatedImage>(gated);
    const ReloadCreateFn create_fn = image.create;

    // We are about to instantiate from this image — retain it for the process
    // lifetime (its code backs the constructed Processor; see reload_library.hpp's
    // leak policy). create_fn stays valid: the image remains loaded.
    retained_images.push_back(std::move(image.lib));

    // Stamp partial metrics onto an early-rejection outcome (later phases 0).
    const auto reject = [&](ReloadOutcome o) {
        o.metrics.load_gate_ms = ms(t0, t_gate);
        o.metrics.total_ms = ms(t0, clock::now());
        return o;
    };

    // 5. Construct + gate + commit. Wrapped so a throwing candidate (ctor,
    //    define_parameters, prepare) yields a graceful Rejected outcome instead
    //    of escaping as an exception — catching across the seam is sound because
    //    the fingerprint gate above proved an identical exception ABI/stdlib.
    try {
        std::unique_ptr<Processor> candidate(create_fn());
        if (!candidate) {
            return reject({ReloadOutcome::Status::RejectedNoEntryPoints, "create returned null"});
        }

        // Parameter-contract gate — the candidate must present the same
        // automatable surface as the live plugin. Define into a scratch store so
        // the comparison never touches live state; the candidate is discarded
        // (destroyed on this control thread) if it fails.
        state::StateStore scratch;
        candidate->define_parameters(scratch);
        if (!param_contracts_match(live_store, scratch)) {
            return reject({ReloadOutcome::Status::RejectedContract, "parameter contract differs",
                           param_contract_diff(live_store, scratch)});
        }
        const auto t_construct = clock::now();

        // Commit: bind the candidate to the LIVE store (same params + current
        // values by the gate above), prepare, and swap. Cross-module delete is
        // safe here only because the fingerprint gate proved shell and logic
        // share one C++ runtime (operator new/delete + heap); the displaced
        // processor is destroyed on this control thread on return. (A future
        // static-C++-runtime logic build would route deletion through
        // kDestroySymbol instead — see reload_abi.hpp.)
        candidate->set_state_store(&live_store);
        candidate->prepare(prepare_ctx);
        const auto t_prepare = clock::now();

        // Behavioral entry-point probe (item 1.10): a candidate can pass every
        // STATIC gate (load / ABI / fingerprint / contract) yet still misbehave
        // at runtime — most dangerously by emitting NaN/Inf, which a swap would
        // push straight to the audio thread and silently corrupt the signal.
        // Render ONE silence block through the freshly-prepared candidate into
        // scratch and reject if the output is not finite, so a garbage-producing
        // build never goes live. Cheap, pre-commit (old DSP stays on rejection),
        // and a universal invariant — silence in → finite out holds for effects
        // AND instruments (NaN/Inf is always a bug). Runs inside the try, so a
        // throwing process() is caught like any other candidate fault.
        {
            const int probe_frames =
                std::max(1, std::min(64, prepare_ctx.max_buffer_size > 0
                                             ? prepare_ctx.max_buffer_size : 64));
            const std::size_t frames = static_cast<std::size_t>(probe_frames);
            std::vector<float> in_l(frames, 0.0f), in_r(frames, 0.0f);
            std::vector<float> out_l(frames, 0.0f), out_r(frames, 0.0f);
            const float* in_ptrs[2] = {in_l.data(), in_r.data()};
            float* out_ptrs[2] = {out_l.data(), out_r.data()};
            audio::BufferView<const float> probe_in(in_ptrs, 2, frames);
            audio::BufferView<float> probe_out(out_ptrs, 2, frames);
            midi::MidiBuffer probe_mi, probe_mo;
            candidate->process(probe_out, probe_in, probe_mi, probe_mo, ProcessContext{});
            for (std::size_t c = 0; c < probe_out.num_channels(); ++c) {
                auto o = probe_out.channel(c);
                for (std::size_t n = 0; n < frames; ++n) {
                    if (!std::isfinite(o[n])) {
                        return reject({ReloadOutcome::Status::RejectedCandidateThrew,
                                       "candidate produced non-finite output on a silence probe"});
                    }
                }
            }
        }

        std::unique_ptr<Processor> displaced = slot.swap(std::move(candidate));
        (void)displaced;  // ~Processor() here — control thread, never the audio thread.
        const auto t_swap = clock::now();

        ReloadOutcome ok{ReloadOutcome::Status::Swapped, library_path};
        ok.metrics.load_gate_ms = ms(t0, t_gate);
        ok.metrics.construct_ms = ms(t_gate, t_construct);
        ok.metrics.prepare_ms = ms(t_construct, t_prepare);
        ok.metrics.swap_ms = ms(t_prepare, t_swap);
        ok.metrics.total_ms = ms(t0, t_swap);
        return ok;
    } catch (const std::exception& e) {
        return reject({ReloadOutcome::Status::RejectedCandidateThrew,
                       std::string("candidate threw: ") + e.what()});
    } catch (...) {
        return reject({ReloadOutcome::Status::RejectedCandidateThrew,
                       "candidate threw (non-std exception)"});
    }
}

/// Owns the session-stable state for a series of hot reloads of one plugin
/// instance — the slot, the live store, the host fingerprint, the prepare
/// context, and the retained images — so callers (a standalone shell, a
/// file-watch controller) issue `reload(path)` without re-threading five
/// coupled arguments, and the lifetime contract is enforced by construction.
class ReloadSession {
public:
    ReloadSession(ProcessorHotSwapSlot& slot, state::StateStore& live_store,
                  const BuildFingerprint& host_fingerprint, const PrepareContext& prepare_ctx)
        : slot_(slot), live_store_(live_store),
          host_fingerprint_(host_fingerprint), prepare_ctx_(prepare_ctx) {}

    /// Attempt one reload from @p library_path. See reload_processor_from_library.
    ReloadOutcome reload(const std::string& library_path) {
        return reload_processor_from_library(slot_, library_path, host_fingerprint_,
                                             live_store_, prepare_ctx_, retained_images_);
    }

    /// Images retained for the process lifetime (one per constructed candidate).
    std::size_t retained_image_count() const { return retained_images_.size(); }

private:
    ProcessorHotSwapSlot& slot_;
    state::StateStore& live_store_;
    BuildFingerprint host_fingerprint_;
    PrepareContext prepare_ctx_;
    std::vector<ReloadLibrary> retained_images_;
};

} // namespace pulp::format::reload
