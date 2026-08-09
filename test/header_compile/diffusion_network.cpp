#include <pulp/signal/diffusion_network.hpp>

void diffusion_network_header_self_containment() {
    pulp::signal::DiffusionNetwork network;
    (void)network.prepare(48000.0, 50.0);
    float left = 0.0f;
    float right = 0.0f;
    network.process_sample(1.0f, 0.0f, left, right);
}
