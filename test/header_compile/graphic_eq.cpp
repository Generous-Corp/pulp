#include <pulp/signal/graphic_eq.hpp>

// Proves the header is self-contained: it uses BiquadT, biquad_is_stable, and
// BiquadCoefficientsT transitively, so it must not rely on a consumer having
// already included biquad.hpp.
template class pulp::signal::GraphicEqT<float, 4>;
template class pulp::signal::GraphicEqT<double, 4>;
