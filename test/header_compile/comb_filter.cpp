#include <pulp/signal/comb_filter.hpp>

void instantiate_comb_filter_header() {
    pulp::signal::CombFilter comb;
    (void)comb.prepare(8u);
    (void)comb.configure({pulp::signal::CombFilterMode::feedforward, 4u, 0.5});
    (void)comb.process(1.0f);
}
