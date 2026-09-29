#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <pulp/gpu_audio/gpu_spectral_mask.hpp>
#include <vector>

namespace {

using Session = pulp::gpu_audio::GpuSpectralMaskSession;

Session::Config valid_config(std::vector<float>& gains) {
    gains.assign(129, 1.0f);
    return {.fft_size = 256,
            .hop = 64,
            .channels = 2,
            .sample_rate = 48'000,
            .slots = 3,
            .gains = gains};
}

void require_invalid(Session::Config config) {
    const auto result = Session::create(config);
    CHECK_FALSE(result.session);
    CHECK(result.error == Session::Error::InvalidConfig);
    CHECK_FALSE(result);
    const auto per_hop = Session::create_with_per_hop_gains(config);
    CHECK_FALSE(per_hop.session);
    CHECK(per_hop.error == Session::Error::InvalidConfig);
    CHECK_FALSE(per_hop);
}

TEST_CASE("spectral mask rejects every invalid static configuration",
          "[gpu_audio][spectral][configuration]") {
    std::vector<float> gains;
    auto config = valid_config(gains);

    config.fft_size = 128;
    require_invalid(config);
    config = valid_config(gains);
    config.fft_size = 300;
    require_invalid(config);
    config = valid_config(gains);
    config.fft_size = 32'768;
    require_invalid(config);
    config = valid_config(gains);
    config.hop = 0;
    require_invalid(config);
    config = valid_config(gains);
    config.hop = 129;
    require_invalid(config);
    config = valid_config(gains);
    config.hop = 100;
    require_invalid(config);
    config = valid_config(gains);
    config.channels = 0;
    require_invalid(config);
    config = valid_config(gains);
    config.channels = 9;
    require_invalid(config);
    config = valid_config(gains);
    config.sample_rate = 0;
    require_invalid(config);
    config = valid_config(gains);
    config.slots = 1;
    require_invalid(config);
    config = valid_config(gains);
    config.slots = 65;
    require_invalid(config);
    config = valid_config(gains);
    gains.pop_back();
    config.gains = gains;
    require_invalid(config);
    config = valid_config(gains);
    gains[7] = -1.0f;
    config.gains = gains;
    require_invalid(config);
    config = valid_config(gains);
    gains[7] = std::numeric_limits<float>::quiet_NaN();
    config.gains = gains;
    require_invalid(config);
    config = valid_config(gains);
    gains[7] = std::numeric_limits<float>::infinity();
    config.gains = gains;
    require_invalid(config);
}

TEST_CASE("valid spectral configuration reports backend capability honestly",
          "[gpu_audio][spectral][configuration]") {
    std::vector<float> gains;
    const auto config = valid_config(gains);
    const auto result = Session::create(config);
    const auto per_hop = Session::create_with_per_hop_gains(config);

    // CPU-only coverage builders must fail closed. An exact-provider runner
    // may prepare the session, in which case lifecycle remains observable.
    if (!result.session) {
        CHECK(result.error == Session::Error::ProviderUnavailable);
    } else {
        CHECK(result);
        CHECK(result.session->prepared());
        CHECK(result.session->latency_samples() == 320);
        CHECK(result.session->release());
    }
    if (!per_hop.session) {
        CHECK(per_hop.error == Session::Error::ProviderUnavailable);
    } else {
        CHECK(per_hop);
        CHECK(per_hop.session->prepared());
        CHECK(per_hop.session->latency_samples() == 320);
        CHECK(per_hop.session->release());
    }
}

} // namespace
