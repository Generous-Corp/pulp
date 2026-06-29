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

#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace pulp::format::reload {

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

    bool ok() const { return status == Status::Swapped; }
    explicit operator bool() const { return ok(); }
};

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

    // Reject-before-construction: the image backed no C++ object, so it is
    // quiescible — unload it immediately rather than leaking it for the process
    // lifetime (failed reloads are common in a live dev session).
    auto reject_quiescible = [](ReloadLibrary& lib, ReloadOutcome::Status status,
                                std::string detail,
                                std::vector<std::string> issues = {}) {
        lib.set_leak_policy(LeakPolicy::CloseOnDestroy);
        return ReloadOutcome{status, std::move(detail), std::move(issues)};
    };

    // 1. Load the image. A failed load has no handle to retain or close.
    ReloadLibrary lib(library_path);
    if (!lib.valid()) {
        return {ReloadOutcome::Status::RejectedLoadFailed, lib.error()};
    }

    // 2. Reload-ABI-version gate — coarsest compatibility check, first.
    auto abi_version_fn = lib.symbol<ReloadAbiVersionFn>(kAbiVersionSymbol);
    if (!abi_version_fn) {
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedNoEntryPoints,
                                 "missing reload-ABI-version symbol");
    }
    if (const int v = abi_version_fn(); v != kReloadAbiVersion) {
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedAbiVersion,
                                 "reload ABI version " + std::to_string(v) +
                                     " != host " + std::to_string(kReloadAbiVersion));
    }

    // 3. Build-fingerprint gate — refuse a C++-ABI-incompatible image before
    //    constructing anything across the seam.
    auto fingerprint_fn = lib.symbol<ReloadFingerprintFn>(kFingerprintSymbol);
    if (!fingerprint_fn) {
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedNoEntryPoints,
                                 "missing fingerprint symbol");
    }
    BuildFingerprint logic_fingerprint{};
    fingerprint_fn(&logic_fingerprint);
    if (!fingerprints_match(host_fingerprint, logic_fingerprint)) {
        return reject_quiescible(
            lib, ReloadOutcome::Status::RejectedFingerprint, "build fingerprint mismatch",
            fingerprint_diff(host_fingerprint, logic_fingerprint));
    }

    // 4. Resolve the factory.
    auto create_fn = lib.symbol<ReloadCreateFn>(kCreateSymbol);
    if (!create_fn) {
        return reject_quiescible(lib, ReloadOutcome::Status::RejectedNoEntryPoints,
                                 "missing create symbol");
    }

    // We are about to instantiate from this image — retain it for the process
    // lifetime (its code may back a live, or transiently-constructed, Processor;
    // see reload_library.hpp's leak policy). The function pointers resolved above
    // stay valid: the image remains loaded via retained_images.
    retained_images.push_back(std::move(lib));

    // 5. Construct + gate + commit. Wrapped so a throwing candidate (ctor,
    //    define_parameters, prepare) yields a graceful Rejected outcome instead
    //    of escaping as an exception — catching across the seam is sound because
    //    the fingerprint gate above proved an identical exception ABI/stdlib.
    try {
        std::unique_ptr<Processor> candidate(create_fn());
        if (!candidate) {
            return {ReloadOutcome::Status::RejectedNoEntryPoints, "create returned null"};
        }

        // Parameter-contract gate — the candidate must present the same
        // automatable surface as the live plugin. Define into a scratch store so
        // the comparison never touches live state; the candidate is discarded
        // (destroyed on this control thread) if it fails.
        state::StateStore scratch;
        candidate->define_parameters(scratch);
        if (!param_contracts_match(live_store, scratch)) {
            return {ReloadOutcome::Status::RejectedContract, "parameter contract differs",
                    param_contract_diff(live_store, scratch)};
        }

        // Commit: bind the candidate to the LIVE store (same params + current
        // values by the gate above), prepare, and swap. Cross-module delete is
        // safe here only because the fingerprint gate proved shell and logic
        // share one C++ runtime (operator new/delete + heap); the displaced
        // processor is destroyed on this control thread on return. (A future
        // static-C++-runtime logic build would route deletion through
        // kDestroySymbol instead — see reload_abi.hpp.)
        candidate->set_state_store(&live_store);
        candidate->prepare(prepare_ctx);
        std::unique_ptr<Processor> displaced = slot.swap(std::move(candidate));
        (void)displaced;  // ~Processor() here — control thread, never the audio thread.

        return {ReloadOutcome::Status::Swapped, library_path};
    } catch (const std::exception& e) {
        return {ReloadOutcome::Status::RejectedCandidateThrew,
                std::string("candidate threw: ") + e.what()};
    } catch (...) {
        return {ReloadOutcome::Status::RejectedCandidateThrew,
                "candidate threw (non-std exception)"};
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
