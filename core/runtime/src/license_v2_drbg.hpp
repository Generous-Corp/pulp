#pragma once

// Internal to pulp-runtime: how the v2 licence generator seeds the CTR-DRBG
// that draws its AES-GCM IVs. Kept in a header so a test can check that the
// whole personalization string reaches the DRBG.

#include <mbedtls/ctr_drbg.h>

#include <cstddef>
#include <string_view>

namespace pulp::runtime::detail {

/// Domain-separates this DRBG from any other CTR-DRBG instance in the runtime.
inline constexpr std::string_view kLicenseV2DrbgPersonalization = "pulp::license::v2";

using DrbgEntropySource = int (*)(void* context, unsigned char* output, std::size_t length);

/// Seeds `drbg` with the full personalization string; its length comes from
/// the string itself, so respelling the string cannot truncate or overrun it.
inline int seed_license_v2_drbg(mbedtls_ctr_drbg_context& drbg,
                                DrbgEntropySource entropy,
                                void* entropy_context) {
    return mbedtls_ctr_drbg_seed(
        &drbg, entropy, entropy_context,
        reinterpret_cast<const unsigned char*>(kLicenseV2DrbgPersonalization.data()),
        kLicenseV2DrbgPersonalization.size());
}

} // namespace pulp::runtime::detail
