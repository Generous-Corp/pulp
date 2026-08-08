#include <pulp/signal/dynamics_contract.hpp>

void dynamics_contract_header_self_containment() {
    pulp::signal::EnvelopeFollower follower;
    follower.prepare(48000.0f);
    (void)follower.process(0.0f);
    (void)pulp::signal::GainReduction::from_signed_db(-3.0);
}
