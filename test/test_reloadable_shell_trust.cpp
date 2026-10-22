// ReloadableShell initial-load trust under both watcher configurations.
//
// test/cmake/format_reload_tests.cmake builds this file twice: once with the
// dev watcher compiled in (the default) and once with PULP_RELOAD_DEV_WATCHER=0,
// the shipping configuration. The expectations switch on that macro:
//   - with the watcher, an unsigned logic image loads (the dev loop) and
//     PULP_RELOAD_LOGIC_PATH names the logic when no path is passed;
//   - without it, the same unsigned image is refused before any dlopen and the
//     shell runs as passthrough, and the environment variable is never read;
//   - in both, a signed pack under the pinned key loads and a pack signed by
//     another key is refused, which is the positive control for the gate.
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/reload/reloadable_shell.hpp>
#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/state/store.hpp>

#include "support/portable_env.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format::reload;
namespace fs = std::filesystem;

#if !defined(RELOAD_LOGIC_UNITY) || !defined(RELOAD_WATCH_DIR)
#error "RELOAD_LOGIC_UNITY and RELOAD_WATCH_DIR must be defined"
#endif

namespace {

// Each case gets its own copy of the unity logic inside the build tree (never
// /tmp: macOS dyld kills a process that dlopens an unsigned .dylib from a
// world-writable temp dir), so a sidecar written for one case cannot leak into
// another.
fs::path fresh_logic_copy(const char* tag) {
    static std::atomic<int> counter{0};
    const fs::path src(RELOAD_LOGIC_UNITY);
    const fs::path dst = fs::path(RELOAD_WATCH_DIR) /
                         ("pulp_shell_trust_" + std::string(tag) + "_" +
                          std::to_string(counter.fetch_add(1)) + src.extension().string());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    std::error_code ec;
    fs::remove(reload_sidecar_manifest_path(dst), ec);
    return dst;
}

std::string read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Write a signed sidecar manifest for `logic` and return the signer's public key.
std::vector<std::uint8_t> write_signed_sidecar(const fs::path& logic) {
    auto kp = runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    SwapPackManifest m;
    m.id = "shell-trust";
    m.plugin_id = "com.pulp.reloadable-shell";
    m.files = {{logic.filename().string(), runtime::sha256_hex(read_bytes(logic)),
                SwapPackKind::DspGraph}};
    m.signer_public_key = kp->public_key;
    const auto msg = swap_pack_signed_message(m);
    auto sig = runtime::ed25519_sign(kp->private_key.data(), kp->private_key.size(), msg.data(),
                                     msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;
    std::ofstream(reload_sidecar_manifest_path(logic), std::ios::binary)
        << serialize_swap_pack_manifest(m);
    return kp->public_key;
}

ReloadTrustPolicy signed_only(std::vector<std::uint8_t> key) {
    ReloadTrustPolicy policy;
    policy.require_signed = true;
    policy.trusted_public_key = std::move(key);
    return policy;
}

// Render one block of all-ones input through the shell and return out[0][0].
float render_one(format::Processor& shell) {
    constexpr int frames = 64;
    audio::Buffer<float> in(2, frames), out(2, frames);
    for (int n = 0; n < frames; ++n) {
        in.channel(0)[n] = 1.0f;
        in.channel(1)[n] = 1.0f;
    }
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, frames);
    auto ov = out.view();
    midi::MidiBuffer a, b;
    shell.process(ov, iv, a, b, format::ProcessContext{});
    return out.channel(0)[0];
}

// Drive the shell through the adapter path and report whether the logic went
// live: a loaded unity logic mirrors one parameter and renders 1.0 * 0.5; a
// refused one mirrors nothing and passes the input straight through.
struct Driven {
    std::size_t params = 0;
    bool active = false;
    float sample = 0.0f;
};

Driven drive(ReloadableShell& shell) {
    Driven d;
    state::StateStore store;
    shell.define_parameters(store);
    d.params = store.all_params().size();
    shell.set_state_store(&store);
    if (d.params == 1)
        store.set_value(1, 0.5f);
    format::PrepareContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.max_buffer_size = 64;
    shell.prepare(ctx);
    d.active = shell.has_active_dsp();
    d.sample = render_one(shell);
    shell.release();
    return d;
}

class ScopedEnvVar {
  public:
    explicit ScopedEnvVar(const char* name) : name_(name) {
        if (const char* value = std::getenv(name))
            previous_ = value;
    }
    ~ScopedEnvVar() {
        if (previous_)
            static_cast<void>(test::set_env_var(name_.c_str(), previous_->c_str()));
        else
            static_cast<void>(test::unset_env_var(name_.c_str()));
    }
    void set(const char* value) const {
        REQUIRE(test::set_env_var(name_.c_str(), value) == 0);
    }
    void unset() const {
        REQUIRE(test::unset_env_var(name_.c_str()) == 0);
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

} // namespace

TEST_CASE("ReloadableShell default trust policy follows the watcher gate",
          "[reload][shell][trust]") {
#if PULP_RELOAD_DEV_WATCHER
    CHECK_FALSE(ReloadableShell::default_trust_policy().require_signed);
#else
    CHECK(ReloadableShell::default_trust_policy().require_signed);
#endif
}

TEST_CASE("ReloadableShell initial load of an unsigned logic image follows the watcher gate",
          "[reload][shell][trust]") {
    const fs::path logic = fresh_logic_copy("unsigned");
    ReloadableShell shell(logic.string());
    const Driven d = drive(shell);
#if PULP_RELOAD_DEV_WATCHER
    // The dev loop: whatever dylib sits at the path loads.
    CHECK(shell.initial_load_outcome().ok());
    CHECK(d.params == 1);
    CHECK(d.active);
    CHECK(d.sample == 0.5f);
#else
    // Shipping: refused before dlopen, passthrough with no parameters.
    CHECK(shell.initial_load_outcome().status == ReloadOutcome::Status::RejectedSignature);
    CHECK(d.params == 0);
    CHECK_FALSE(d.active);
    CHECK(d.sample == 1.0f);
#endif
    std::error_code ec;
    fs::remove(logic, ec);
}

TEST_CASE("ReloadableShell reads PULP_RELOAD_LOGIC_PATH only beside the dev watcher",
          "[reload][shell][trust]") {
    const fs::path logic = fresh_logic_copy("env");
    ScopedEnvVar env("PULP_RELOAD_LOGIC_PATH");
    env.set(logic.string().c_str());
    ReloadableShell shell; // no explicit path
    const Driven d = drive(shell);
#if PULP_RELOAD_DEV_WATCHER
    CHECK(shell.logic_path() == logic.string());
    CHECK(shell.initial_load_outcome().ok());
    CHECK(d.active);
#else
    CHECK(shell.logic_path().empty());
    CHECK(shell.initial_load_outcome().status == ReloadOutcome::Status::RejectedLoadFailed);
    CHECK_FALSE(d.active);
    CHECK(d.sample == 1.0f);
#endif
    std::error_code ec;
    fs::remove(logic, ec);
}

TEST_CASE(
    "ReloadableShell loads a signed initial image under the pinned key in both configurations",
    "[reload][shell][trust]") {
    const fs::path logic = fresh_logic_copy("signed");
    const auto key = write_signed_sidecar(logic);
    ReloadableShell shell(logic.string(), signed_only(key));
    const Driven d = drive(shell);
    CHECK(shell.initial_load_outcome().ok());
    CHECK(d.params == 1);
    CHECK(d.active);
    CHECK(d.sample == 0.5f);
    std::error_code ec;
    fs::remove(reload_sidecar_manifest_path(logic), ec);
    fs::remove(logic, ec);
}

TEST_CASE("ReloadableShell refuses a signed initial image from an untrusted signer in both "
          "configurations",
          "[reload][shell][trust]") {
    const fs::path logic = fresh_logic_copy("wrongkey");
    (void)write_signed_sidecar(logic);
    auto other = runtime::ed25519_keypair_generate();
    REQUIRE(other.has_value());
    ReloadableShell shell(logic.string(), signed_only(other->public_key));
    const Driven d = drive(shell);
    CHECK(shell.initial_load_outcome().status == ReloadOutcome::Status::RejectedSignature);
    CHECK(d.params == 0);
    CHECK_FALSE(d.active);
    CHECK(d.sample == 1.0f);
    std::error_code ec;
    fs::remove(reload_sidecar_manifest_path(logic), ec);
    fs::remove(logic, ec);
}
