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
#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/state/store.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
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
        RejectedSignature,     ///< Swap-pack Ed25519 signature/signer verification failed,
                               ///< rejected before the image is loaded.
        RejectedIntegrity,     ///< Swap-pack per-file SHA-256 failed, or the load target was
                               ///< not a verified member of the pack; rejected before load.
        RejectedRevoked,       ///< Swap-pack signer/artifact is on the signed revocation list;
                               ///< rejected before load.
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

/// Trust material for verifying a swap-pack-delivered logic image BEFORE it is
/// loaded. A swap pack is a signed manifest (Ed25519, pinned key) plus its files,
/// laid out under @p pack_root. When this is supplied to gate_logic_image(), the
/// pack's signature + per-file SHA-256 integrity are verified on the RAW bytes on
/// disk BEFORE any dlopen/LoadLibrary — because loading a native image runs its
/// static constructors immediately, so a signature/integrity check placed after
/// the load can never stop a malicious image's constructors from executing.
/// Absent (the default), gate_logic_image loads directly: the unsigned local-dev
/// path, unchanged. If you opt IN to trust, the pack MUST be signed by the pinned
/// key or verification fails closed.
struct SwapPackTrust {
    std::filesystem::path pack_root;               ///< dir the manifest paths are relative to.
    SwapPackManifest manifest;                     ///< parsed manifest (incl. signer + signature).
    std::vector<std::uint8_t> trusted_public_key;  ///< the pinned Ed25519 key the signer must be.
};

/// Verify @p trust on the RAW bytes on disk and confirm @p library_path is a
/// member of the verified pack — the strict pre-dlopen trust phase. Returns
/// std::nullopt when the pack is trustworthy AND @p library_path is one of its
/// verified files (safe to load those exact bytes); otherwise a fail-closed
/// ReloadOutcome. Ordering mirrors swap_pack::verify_swap_pack — authenticate the
/// signature first (it binds every file hash), THEN re-hash the files, THEN the
/// revocation hook, THEN the loaded-file membership check. No dlopen happens here
/// or anywhere upstream of a passing result.
inline std::optional<ReloadOutcome>
verify_pack_before_load(const SwapPackTrust& trust, const std::string& library_path) {
    // 1. Signature (authenticate the manifest) + 2. per-file integrity (the files
    //    match the SIGNED hashes). verify_swap_pack is the sanctioned entry point
    //    for both, in that order. Map its taxonomy onto the reload taxonomy.
    const auto v = verify_swap_pack(trust.pack_root, trust.manifest, trust.trusted_public_key);
    if (!v.ok()) {
        const bool sig = v.status == SwapPackVerify::UntrustedSigner ||
                         v.status == SwapPackVerify::BadSignature;
        return ReloadOutcome{
            sig ? ReloadOutcome::Status::RejectedSignature
                : ReloadOutcome::Status::RejectedIntegrity,
            (sig ? "swap-pack signature rejected: " : "swap-pack integrity rejected: ") +
                v.detail};
    }

    // Hook for the signed-revocation-list check: once the pack is authenticated,
    // reject a signer or artifact that has since been revoked (a leaked signing
    // key must be killable without re-shipping every consumer). The revocation
    // list reader (reload/revocation.hpp) is not present yet, so this is a
    // deliberate not-revoked stub structured so wiring it is a one-liner here:
    //        if (auto rev = revocation::check_revoked(trust.manifest, srl); rev.revoked)
    //            return ReloadOutcome{ReloadOutcome::Status::RejectedRevoked, rev.detail};
    // The stub is fail-OPEN on revocation ONLY: the signature and integrity gates
    // above are already fail-closed, so a not-revoked default can never admit an
    // unsigned or tampered pack — it only defers killing an already-trusted one.
    // Signed policy fields (pack version, allowed kind) will be checked at this
    // same point once they are bound inside the signed manifest.
    const bool revoked = false;  // stub: revocation list reader not present yet.
    if (revoked) {
        return ReloadOutcome{ReloadOutcome::Status::RejectedRevoked,
                             "swap-pack signer/artifact is revoked"};
    }

    // 3. Bind verification to the load: the exact file we are about to dlopen must
    //    be one of the files we JUST hashed. Otherwise a caller could verify a pack
    //    yet load an arbitrary sibling path that was never in the signed set. Using
    //    std::filesystem::equivalent (same file on disk, not string equality) also
    //    defeats a symlink/relative-path detour to an unverified target.
    bool member = false;
    for (const auto& f : trust.manifest.files) {
        std::error_code ec;
        if (std::filesystem::equivalent(trust.pack_root / f.path, library_path, ec) && !ec) {
            member = true;
            break;
        }
    }
    if (!member) {
        return ReloadOutcome{ReloadOutcome::Status::RejectedIntegrity,
                             "load target is not a verified member of the swap pack"};
    }
    return std::nullopt;  // trustworthy — safe to load these exact bytes.
}

/// Load @p library_path and run the pre-construction gates — reload-ABI version,
/// then build fingerprint — and resolve the create symbol. The SINGLE source of
/// truth for that gate ordering, shared by reload_processor_from_library() and
/// ReloadableShell's initial load so they can never disagree about what
/// "compatible" means. Returns the GatedImage on success (caller retains lib +
/// calls create), or a fail-closed ReloadOutcome on rejection (the image backed
/// no C++ object, so it is set CloseOnDestroy and unloaded rather than leaked).
///
/// When @p trust is non-null the swap-pack signature + integrity (+ revocation
/// hook) are verified on the RAW bytes BEFORE the dlopen — dlopen runs a native
/// image's static constructors, so trust MUST precede load. When null (the
/// unsigned local-dev default) the image is loaded directly, unchanged.
inline std::variant<GatedImage, ReloadOutcome>
gate_logic_image(const std::string& library_path, const BuildFingerprint& host_fingerprint,
                 const SwapPackTrust* trust = nullptr) {
    auto reject_quiescible = [](ReloadLibrary& lib, ReloadOutcome::Status status,
                                std::string detail, std::vector<std::string> issues = {}) {
        lib.set_leak_policy(LeakPolicy::CloseOnDestroy);
        return ReloadOutcome{status, std::move(detail), std::move(issues)};
    };

    // 0. Trust gate — verify signed-pack bytes + integrity (+ revocation) on the
    //    RAW file BEFORE any dlopen. dlopen/LoadLibrary executes a native image's
    //    static constructors immediately, so any signature/revocation check placed
    //    AFTER the load is worthless for native code — the attacker's constructor
    //    already ran. This phase touches only the bytes on disk (no code from the
    //    image is mapped or executed) and fails closed. Skipped when trust is null.
    if (trust != nullptr) {
        if (auto rejected = verify_pack_before_load(*trust, library_path)) {
            return std::move(*rejected);
        }
    }

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
/// @param trust             optional swap-pack trust material; when non-null the
///                          signed pack is verified on the RAW bytes BEFORE the
///                          dlopen. Null = unsigned local-dev load, unchanged.
/// @returns an outcome; only Status::Swapped means the slot changed.
inline ReloadOutcome reload_processor_from_library(
    ProcessorHotSwapSlot& slot,
    const std::string& library_path,
    const BuildFingerprint& host_fingerprint,
    state::StateStore& live_store,
    const PrepareContext& prepare_ctx,
    std::vector<ReloadLibrary>& retained_images,
    const SwapPackTrust* trust = nullptr) {

    // Phase timing (item 1.2). steady_clock; this whole function is control-thread
    // (off the audio path), so timing here is never an RT concern.
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // Trust (if a signed pack is supplied) + load + gate (ABI version,
    // fingerprint) + resolve the factory — the shared, fail-closed gate sequence
    // (see gate_logic_image). Trust verification precedes dlopen.
    auto gated = gate_logic_image(library_path, host_fingerprint, trust);
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
