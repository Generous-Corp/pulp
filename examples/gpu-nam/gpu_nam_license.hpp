#pragma once

// Optional licensing gate — a reference for commercial plugins built on Pulp.
//
// The shipped GPU NAM is free (MIT) and leaves this in FREE-TIER mode: nothing is
// restricted. It exists to show how a paid plugin would gate a premium feature
// behind Pulp's runtime::LicenseValidator. Here the gated feature is the opt-in
// GPU acceleration; an unlicensed build falls back to the CPU engine — which is
// bit-exact, so the amp sound is never degraded, only the GPU path is gated. That
// keeps the demonstration honest: the free tier is fully usable.
//
// A developer enables it by building with -DGPU_NAM_WITH_LICENSE and calling one
// of the configure_* methods with their public key (v1 RSA) or shared secret
// (v2 AES-GCM) plus the user's license key (e.g. from an env var or a file). With
// no configuration the gate stays free-tier and unlocked.

#include <pulp/runtime/license.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::examples {

class LicenseGate {
public:
    LicenseGate() = default;

    // v1 gate: an RSA public key is embedded in the plugin; the server signs keys
    // with the matching private key. An empty PEM keeps the gate free-tier.
    void configure_rsa(std::string_view public_key_pem, std::string_view license_key) {
        if (public_key_pem.empty()) { free_tier_ = true; return; }
        free_tier_ = false;
        runtime::LicenseValidator v;
        v.set_public_key(public_key_pem);
        status_ = v.validate(license_key);
    }

    // v2 gate: a shared secret (symmetric). An empty secret keeps the gate
    // free-tier. Simpler to test/deploy; v1 is preferred when the binary is public.
    void configure_shared(const std::uint8_t* secret, std::size_t secret_size,
                          std::string_view license_key) {
        if (!secret || secret_size == 0) { free_tier_ = true; return; }
        free_tier_ = false;
        runtime::LicenseValidator v;
        v.set_shared_secret(secret, secret_size);
        status_ = v.validate(license_key);
    }

    // True when the premium (GPU) path is permitted: always in free-tier mode,
    // otherwise only for a valid license.
    bool unlocked() const {
        return free_tier_ || status_ == runtime::LicenseStatus::Valid;
    }

    // Human-readable status for the editor's About panel.
    std::string status_text() const {
        if (free_tier_) return "Free (MIT) build";
        if (unlocked()) return "Registered";
        return "Unregistered — GPU acceleration locked (CPU engine active)";
    }

    runtime::LicenseStatus status() const { return status_; }

private:
    bool free_tier_ = true;
    runtime::LicenseStatus status_ = runtime::LicenseStatus::Valid;
};

}  // namespace pulp::examples
