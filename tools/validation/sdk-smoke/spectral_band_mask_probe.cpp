#include <pulp/signal/spectral_band_mask.hpp>

#include <array>
#include <complex>

int main() {
    pulp::signal::SpectralBandLayout layout;
    layout.active_bands = 32;
    layout.min_hz = 20.0f;
    layout.max_hz = 20000.0f;
    for (auto& band : layout.bands) band.muted = true;

    pulp::signal::SpectralMaskTable table;
    if (!pulp::signal::build_spectral_mask(layout, 1024, 48000.0f, table))
        return 1;

    std::array<std::complex<float>, 513> bins;
    bins.fill({1.0f, -0.5f});
    std::complex<float>* frames[] = {bins.data()};
    if (!pulp::signal::apply_spectral_mask(frames, 1, 513, table))
        return 2;
    for (const auto value : bins)
        if (value != std::complex<float>{}) return 3;
    return 0;
}
