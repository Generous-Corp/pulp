// The standalone DSP hot-reload transaction, end to end.
//
// Drives reload_processor_from_library() against two real dlopen'd logic-library
// fixtures (paths injected as RELOAD_LOGIC_COMPATIBLE / RELOAD_LOGIC_INCOMPATIBLE)
// to prove: a compatible candidate is gated, loaded, bound to the live store,
// and swapped in (the audio output changes while the parameter value is
// preserved); an incompatible candidate is rejected at the contract gate with
// the slot left untouched; an ABI/fingerprint mismatch is rejected; and a
// missing library fails cleanly.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_transaction.hpp>
#include <pulp/format/reload/reload_swap_units.hpp>
#include <pulp/format/reload/live_swap_transaction.hpp>
#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/state/store.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using namespace pulp;
using namespace pulp::format::reload;

#if !defined(RELOAD_LOGIC_COMPATIBLE) || !defined(RELOAD_LOGIC_INCOMPATIBLE) || \
    !defined(RELOAD_LOGIC_THROWING)
#error "RELOAD_LOGIC_{COMPATIBLE,INCOMPATIBLE,THROWING} must be defined to the fixture paths"
#endif

namespace {

// The live plugin's initial DSP: unity-times-gain (the "before" behavior the
// compatible fixture replaces with 2x). Same contract as both fixtures' id 1.
class InitialGain final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadGain", .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.gain", .version = "0.1.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "",
                         .range = {0.0f, 2.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float g = state().get_value(1) * 1.0f;  // unity behavior
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * g;
        }
    }
};

// Run one block of all-ones through the slot; return out channel-0 sample 0.
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

}  // namespace

TEST_CASE("hot-reload swaps in a compatible logic library and preserves state",
          "[reload][transaction]") {
    // Live plugin: gain param at 0.5, initial unity DSP.
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;  // 48k / 512 defaults
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    // Before reload: unity × 0.5 → 0.5.
    REQUIRE(render_one(slot) == 0.5f);

    // Reload the compatible candidate (2x gain, same contract).
    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_COMPATIBLE, host,
                                           live, ctx, images);
    INFO("detail: " << r.detail);
    REQUIRE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::Swapped);

    // After reload: 2x × the SAME 0.5 gain (state preserved) → 1.0.
    REQUIRE(render_one(slot) == 1.0f);
    REQUIRE(live.get_value(1) == 0.5f);  // live parameter value untouched
}

TEST_CASE("hot-reload rejects a contract-incompatible library without swapping",
          "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    const float before = render_one(slot);  // 0.5 (unity)
    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_INCOMPATIBLE, host,
                                           live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedContract);
    // Structured diff — no prose matching on `detail`.
    bool reported_added = false;
    for (const auto& issue : r.issues)
        if (issue.find("added in candidate") != std::string::npos) reported_added = true;
    REQUIRE(reported_added);

    // The slot is untouched — still the initial unity DSP.
    REQUIRE(render_one(slot) == before);
}

TEST_CASE("hot-reload rejects an ABI/fingerprint mismatch", "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;

    // A host fingerprint that deliberately disagrees with the (matching) logic
    // build — simulating shell and logic compiled with different toolchains.
    BuildFingerprint host = current_build_fingerprint();
    host.compiler[0] = 'X';
    host.compiler[1] = '\0';

    const float before = render_one(slot);
    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_COMPATIBLE, host,
                                           live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedFingerprint);
    REQUIRE(render_one(slot) == before);  // unchanged
}

TEST_CASE("hot-reload fails cleanly on a missing library", "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    auto r = reload_processor_from_library(slot, "/no/such/pulp-logic.dylib", host,
                                           live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedLoadFailed);
    REQUIRE(slot.has_active());  // still usable
}

TEST_CASE("hot-reload catches a throwing candidate instead of escaping",
          "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    const float before = render_one(slot);
    // The factory throws after passing the ABI/fingerprint gates; the
    // transaction must convert that into a clean rejection, not propagate it.
    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_THROWING, host,
                                           live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedCandidateThrew);
    REQUIRE(r.detail.find("boom") != std::string::npos);
    REQUIRE(render_one(slot) == before);  // slot untouched
}

TEST_CASE("ReloadSession owns the session state across multiple reloads",
          "[reload][transaction]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    ReloadSession session(slot, live, current_build_fingerprint(), format::PrepareContext{});

    // A compatible reload succeeds and retains one image...
    REQUIRE(session.reload(RELOAD_LOGIC_COMPATIBLE).ok());
    REQUIRE(render_one(slot) == 1.0f);
    REQUIRE(session.retained_image_count() == 1);

    // ...an incompatible reload is rejected at the contract gate and leaves the
    // slot on the previous good processor. Its image IS retained (a candidate
    // was constructed from it during the contract check, so it is not
    // quiescible) — only pre-construction rejects (fingerprint / missing symbol)
    // are unloaded immediately.
    auto bad = session.reload(RELOAD_LOGIC_INCOMPATIBLE);
    REQUIRE(bad.status == ReloadOutcome::Status::RejectedContract);
    REQUIRE(session.retained_image_count() == 2);  // compatible + (constructed) incompatible
    REQUIRE(render_one(slot) == 1.0f);             // still the compatible processor
}

TEST_CASE("hot-reload records DSP-axis phase metrics",
          "[reload][transaction][metrics]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    SECTION("a successful reload populates all phases and a consistent total") {
        auto r = reload_processor_from_library(slot, RELOAD_LOGIC_COMPATIBLE, host,
                                               live, ctx, images);
        REQUIRE(r.ok());
        const auto& m = r.metrics;
        // Every phase is measured (>= 0; a phase can round to 0 on a very fast host).
        CHECK(m.load_gate_ms >= 0.0);
        CHECK(m.construct_ms >= 0.0);
        CHECK(m.prepare_ms >= 0.0);
        CHECK(m.swap_ms >= 0.0);
        // The phases are contiguous, so the total covers each of them and their
        // sum (within a small slack for the clock reads between stamps).
        CHECK(m.total_ms >= m.load_gate_ms);
        CHECK(m.total_ms >= m.construct_ms);
        CHECK(m.total_ms >= m.prepare_ms);
        CHECK(m.total_ms >= m.swap_ms);
        const double sum = m.load_gate_ms + m.construct_ms + m.prepare_ms + m.swap_ms;
        CHECK(m.total_ms + 1.0 >= sum);   // total ≈ sum of contiguous phases
    }

    SECTION("an early rejection still stamps load_gate + total (later phases 0)") {
        auto r = reload_processor_from_library(slot, "/no/such/logic.dylib", host,
                                               live, ctx, images);
        REQUIRE(r.status == ReloadOutcome::Status::RejectedLoadFailed);
        CHECK(r.metrics.load_gate_ms >= 0.0);
        CHECK(r.metrics.total_ms >= 0.0);
        CHECK(r.metrics.construct_ms == 0.0);   // never reached
        CHECK(r.metrics.prepare_ms == 0.0);
        CHECK(r.metrics.swap_ms == 0.0);
    }
}

#ifdef RELOAD_LOGIC_NAN
// Behavioral probe: a candidate that passes every static gate but
// emits NaN at runtime must be rejected PRE-commit — the live DSP stays.
TEST_CASE("hot-reload rejects a candidate that fails the behavioral probe (NaN)",
          "[reload][transaction][probe]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);

    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    std::vector<ReloadLibrary> images;
    const BuildFingerprint host = current_build_fingerprint();

    REQUIRE(render_one(slot) == 0.5f);                 // initial unity × 0.5

    auto r = reload_processor_from_library(slot, RELOAD_LOGIC_NAN, host, live, ctx, images);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == ReloadOutcome::Status::RejectedCandidateThrew);
    INFO("detail: " << r.detail);
    REQUIRE(r.detail.find("non-finite") != std::string::npos);
    REQUIRE(render_one(slot) == 0.5f);                 // live DSP untouched (no swap)
}
#endif

#ifdef RELOAD_LOGIC_CTOR_MARKER
// Verify-before-load: the trust gate MUST run on the raw pack bytes BEFORE any
// dlopen, because dlopen executes a native image's static constructors the
// instant it maps the image — so a signature/integrity check placed after the
// load is worthless for native code. These tests prove ordering with a fixture
// whose static constructor writes a marker file at load time: if a rejected pack
// is never loaded, the marker is never created.
namespace {

void set_env(const char* key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key, value.c_str());
#else
    ::setenv(key, value.c_str(), 1);
#endif
}

// Process-unique suffix so temp pack roots and marker paths never collide across
// test cases (each stage_pack copies the fixture to a distinct path → a distinct
// dlopen mapping, so the load-time ctor fires on first load of each copy).
std::string unique_suffix() {
    static int counter = 0;
    return std::to_string(++counter);
}

std::string file_sha256(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    return runtime::sha256_hex(bytes.data(), bytes.size());
}

// Build a SwapPackTrust that points at the (real, loadable) ctor-marker library,
// laid out under a temp pack root as a single file. @p tamper_hash injects a bad
// declared hash (integrity failure); otherwise the true hash is used. The
// manifest is signed with @p signer AFTER the hash is set, so the signature is
// always internally consistent — an integrity failure is then a genuine
// file-vs-manifest mismatch, not a broken signature.
struct StagedPack {
    std::filesystem::path root;
    std::filesystem::path lib;   // full path to the staged library (the load target)
    SwapPackTrust trust;
};

StagedPack stage_pack(const std::string& tag, const runtime::Ed25519KeyPair& signer,
                      const std::vector<std::uint8_t>& trusted_key, bool tamper_hash) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("pulp-c1-" + tag + "-" + unique_suffix());
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string leaf = "logic.mod";
    const fs::path lib = root / leaf;
    fs::copy_file(RELOAD_LOGIC_CTOR_MARKER, lib, fs::copy_options::overwrite_existing);

    SwapPackManifest m;
    m.id = "c1-pack";
    m.plugin_id = "com.pulp.reload.gain";
    // Declare the real hash, or (tamper) a hash that does not match the file bytes.
    const std::string declared =
        tamper_hash ? runtime::sha256_hex(std::string_view("not-the-real-bytes"))
                    : file_sha256(lib);
    m.files = {{leaf, declared, SwapPackKind::DspGraph}};

    // Sign the FINAL manifest so signature verification passes; integrity is the
    // axis under test in the tampered case.
    m.signer_public_key = signer.public_key;
    const auto msg = swap_pack_signed_message(m);
    auto sig = runtime::ed25519_sign(signer.private_key.data(), signer.private_key.size(),
                                     msg.data(), msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;

    return StagedPack{root, lib, SwapPackTrust{root, std::move(m), trusted_key}};
}

}  // namespace

TEST_CASE("verify-before-load: a trusted signed pack loads (ctor runs) and gates normally",
          "[reload][transaction][trust]") {
    auto kp = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    auto pack = stage_pack("trusted", *kp, kp->public_key, /*tamper_hash=*/false);

    // Marker path that does not yet exist; the fixture's load-time ctor creates it.
    const auto marker = std::filesystem::temp_directory_path() /
                        ("pulp-c1-marker-ok-" + unique_suffix());
    std::filesystem::remove(marker);
    set_env("PULP_RELOAD_CTOR_MARKER", marker.string());
    REQUIRE_FALSE(std::filesystem::exists(marker));

    const BuildFingerprint host = current_build_fingerprint();
    auto gated = gate_logic_image(pack.lib.string(), host, &pack.trust);

    // Trust passed → the image was loaded and gated. The load-time ctor ran, so
    // the marker now exists (this is the FIRST load of this fixture in-process).
    REQUIRE(std::holds_alternative<GatedImage>(gated));
    REQUIRE(std::filesystem::exists(marker));

    std::filesystem::remove(marker);
    std::filesystem::remove_all(pack.root);
}

TEST_CASE("verify-before-load: an ill-signed pack is rejected BEFORE any load (ctor never runs)",
          "[reload][transaction][trust]") {
    auto kp = runtime::ed25519_keypair_generate();
    auto attacker = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    REQUIRE(attacker.has_value());
    // Signed by the attacker's key, but the host trusts kp → UntrustedSigner.
    auto pack = stage_pack("badsig", *attacker, kp->public_key, /*tamper_hash=*/false);

    const auto marker = std::filesystem::temp_directory_path() /
                        ("pulp-c1-marker-badsig-" + unique_suffix());
    std::filesystem::remove(marker);
    set_env("PULP_RELOAD_CTOR_MARKER", marker.string());

    const BuildFingerprint host = current_build_fingerprint();
    auto gated = gate_logic_image(pack.lib.string(), host, &pack.trust);

    REQUIRE(std::holds_alternative<ReloadOutcome>(gated));
    REQUIRE(std::get<ReloadOutcome>(gated).status == ReloadOutcome::Status::RejectedSignature);
    // THE proof: the library's static constructor never ran, so it was never
    // dlopen'd — verification strictly preceded load.
    REQUIRE_FALSE(std::filesystem::exists(marker));

    std::filesystem::remove_all(pack.root);
}

TEST_CASE("verify-before-load: a tampered pack file is rejected BEFORE any load (ctor never runs)",
          "[reload][transaction][trust]") {
    auto kp = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    // Validly signed manifest, but the declared file hash does not match the bytes.
    auto pack = stage_pack("tamper", *kp, kp->public_key, /*tamper_hash=*/true);

    const auto marker = std::filesystem::temp_directory_path() /
                        ("pulp-c1-marker-tamper-" + unique_suffix());
    std::filesystem::remove(marker);
    set_env("PULP_RELOAD_CTOR_MARKER", marker.string());

    const BuildFingerprint host = current_build_fingerprint();
    auto gated = gate_logic_image(pack.lib.string(), host, &pack.trust);

    REQUIRE(std::holds_alternative<ReloadOutcome>(gated));
    REQUIRE(std::get<ReloadOutcome>(gated).status == ReloadOutcome::Status::RejectedIntegrity);
    REQUIRE_FALSE(std::filesystem::exists(marker));  // never loaded

    std::filesystem::remove_all(pack.root);
}

TEST_CASE("verify-before-load: loading a file outside the verified pack is rejected (not a member)",
          "[reload][transaction][trust]") {
    auto kp = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    auto pack = stage_pack("member", *kp, kp->public_key, /*tamper_hash=*/false);

    // The pack verifies, but we ask to load a DIFFERENT library that the signed
    // manifest never covered — must be refused, or trust could be laundered onto
    // an arbitrary sibling path.
    const BuildFingerprint host = current_build_fingerprint();
    auto gated = gate_logic_image(RELOAD_LOGIC_COMPATIBLE, host, &pack.trust);

    REQUIRE(std::holds_alternative<ReloadOutcome>(gated));
    REQUIRE(std::get<ReloadOutcome>(gated).status == ReloadOutcome::Status::RejectedIntegrity);

    std::filesystem::remove_all(pack.root);
}

TEST_CASE("verify-before-load: opting into trust with an unsigned pack fails closed",
          "[reload][transaction][trust]") {
    // A pack with no signer/signature must NOT be accepted just because trust was
    // requested — verification is fail-closed on the signature axis.
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("pulp-c1-unsigned-" + unique_suffix());
    fs::create_directories(root);
    const fs::path lib = root / "logic.mod";
    fs::copy_file(RELOAD_LOGIC_CTOR_MARKER, lib, fs::copy_options::overwrite_existing);

    SwapPackManifest m;
    m.id = "c1-unsigned";
    m.plugin_id = "com.pulp.reload.gain";
    m.files = {{"logic.mod", file_sha256(lib), SwapPackKind::DspGraph}};
    // No signer_public_key / signature set → unsigned.

    auto kp = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    SwapPackTrust trust{root, std::move(m), kp->public_key};

    const auto marker = fs::temp_directory_path() /
                        ("pulp-c1-marker-unsigned-" + unique_suffix());
    fs::remove(marker);
    set_env("PULP_RELOAD_CTOR_MARKER", marker.string());

    const BuildFingerprint host = current_build_fingerprint();
    auto gated = gate_logic_image(lib.string(), host, &trust);

    REQUIRE(std::holds_alternative<ReloadOutcome>(gated));
    REQUIRE(std::get<ReloadOutcome>(gated).status == ReloadOutcome::Status::RejectedSignature);
    REQUIRE_FALSE(fs::exists(marker));  // never loaded

    fs::remove_all(root);
}

TEST_CASE("verify-before-load: a revoked signer key is rejected before load (ctor never runs)",
          "[reload][transaction][trust][revocation]") {
    auto kp = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    auto pack = stage_pack("revoked-signer", *kp, kp->public_key, /*tamper_hash=*/false);

    // The host would authenticate this list against the trusted revocation key and
    // enforce its epoch floor; here we only need it to name the pack's signer.
    SignedRevocationList srl;
    srl.epoch = 1;
    srl.revoked_signer_key_fprs = {srl_hex_encode(kp->public_key)};
    pack.trust.srl = &srl;

    const auto marker = std::filesystem::temp_directory_path() /
                        ("pulp-marker-revoked-" + unique_suffix());
    std::filesystem::remove(marker);
    set_env("PULP_RELOAD_CTOR_MARKER", marker.string());

    const BuildFingerprint host = current_build_fingerprint();
    auto gated = gate_logic_image(pack.lib.string(), host, &pack.trust);

    REQUIRE(std::holds_alternative<ReloadOutcome>(gated));
    REQUIRE(std::get<ReloadOutcome>(gated).status == ReloadOutcome::Status::RejectedRevoked);
    REQUIRE_FALSE(std::filesystem::exists(marker));  // signature was valid, but revoked → never loaded
    std::filesystem::remove_all(pack.root);
}

TEST_CASE("verify-before-load: a revoked artifact hash is rejected before load",
          "[reload][transaction][trust][revocation]") {
    auto kp = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    auto pack = stage_pack("revoked-artifact", *kp, kp->public_key, /*tamper_hash=*/false);

    SignedRevocationList srl;
    srl.epoch = 1;
    srl.revoked_artifact_hashes = {pack.trust.manifest.files.front().sha256_hex};
    pack.trust.srl = &srl;

    const auto marker = std::filesystem::temp_directory_path() /
                        ("pulp-marker-revoked-art-" + unique_suffix());
    std::filesystem::remove(marker);
    set_env("PULP_RELOAD_CTOR_MARKER", marker.string());

    const BuildFingerprint host = current_build_fingerprint();
    auto gated = gate_logic_image(pack.lib.string(), host, &pack.trust);

    REQUIRE(std::holds_alternative<ReloadOutcome>(gated));
    REQUIRE(std::get<ReloadOutcome>(gated).status == ReloadOutcome::Status::RejectedRevoked);
    REQUIRE_FALSE(std::filesystem::exists(marker));
    std::filesystem::remove_all(pack.root);
}

TEST_CASE("verify-before-load: a revocation list that does not name this pack lets it load",
          "[reload][transaction][trust][revocation]") {
    auto kp = runtime::ed25519_keypair_generate();
    auto other = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    REQUIRE(other.has_value());
    auto pack = stage_pack("revoked-other", *kp, kp->public_key, /*tamper_hash=*/false);

    SignedRevocationList srl;
    srl.epoch = 1;
    srl.revoked_signer_key_fprs = {srl_hex_encode(other->public_key)};  // a different signer
    srl.revoked_artifact_hashes = {runtime::sha256_hex(std::string_view("some-other-build"))};
    pack.trust.srl = &srl;

    const auto marker = std::filesystem::temp_directory_path() /
                        ("pulp-marker-revoked-none-" + unique_suffix());
    std::filesystem::remove(marker);
    set_env("PULP_RELOAD_CTOR_MARKER", marker.string());

    const BuildFingerprint host = current_build_fingerprint();
    auto gated = gate_logic_image(pack.lib.string(), host, &pack.trust);

    REQUIRE(std::holds_alternative<GatedImage>(gated));   // not named → loads
    REQUIRE(std::filesystem::exists(marker));
    std::filesystem::remove(marker);
    std::filesystem::remove_all(pack.root);
}
#endif  // RELOAD_LOGIC_CTOR_MARKER

#if defined(RELOAD_LOGIC_COMPATIBLE) && defined(RELOAD_LOGIC_INCOMPATIBLE)
// The real DSP SwapUnit adapter drives a reload through the
// unified transaction — a compatible pack swaps, an incompatible one fails the
// transaction and leaves the live DSP untouched (atomic; DSP stage is terminal).
TEST_CASE("DspReloadSwapUnit swaps DSP through apply_live_swap",
          "[reload][transaction][swap-unit][1.8]") {
    state::StateStore live;
    auto initial = std::make_unique<InitialGain>();
    initial->define_parameters(live);
    initial->set_state_store(&live);
    live.set_value(1, 0.5f);
    ProcessorHotSwapSlot slot(std::move(initial));
    format::PrepareContext ctx;
    const BuildFingerprint host = current_build_fingerprint();
    ReloadSession session(slot, live, host, ctx);

    REQUIRE(render_one(slot) == 0.5f);   // initial unity × 0.5

    SECTION("a compatible reload succeeds via the transaction") {
        DspReloadSwapUnit unit(session, RELOAD_LOGIC_COMPATIBLE);
        std::vector<SwapUnit*> units{&unit};
        auto r = apply_live_swap(units);
        REQUIRE(r.ok);
        REQUIRE(unit.last_outcome().ok());
        REQUIRE(render_one(slot) == 1.0f);   // DSP swapped
    }
    SECTION("an incompatible reload fails the transaction, live DSP untouched") {
        DspReloadSwapUnit unit(session, RELOAD_LOGIC_INCOMPATIBLE);
        std::vector<SwapUnit*> units{&unit};
        auto r = apply_live_swap(units);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.failed_stage == "dsp");
        REQUIRE(render_one(slot) == 0.5f);   // atomic: unchanged
    }
}
#endif
