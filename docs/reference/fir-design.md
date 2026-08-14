# FIR design from sampled targets

`<pulp/signal/fir_design.hpp>` turns a sampled specification or measurement into
portable real FIR coefficients. It complements the parameter-driven
windowed-sinc helpers: use windowed sinc for a conventional cutoff, and this API
when the curve itself is the input.

Both designers allocate and may perform substantial numerical work. Call them
from an offline tool, setup path, or control thread—not from an audio callback.
The returned `std::vector<double>` can be installed directly in `FirFilter64`.
For `FirFilter`, explicitly narrow the coefficients to `float` after checking
that the precision and range are appropriate for the product.

## Weighted least-squares linear-phase FIR

`design_fir_least_squares()` accepts frequency points in any order. `omega` is
angular frequency in radians/sample over `[0, pi]`; each positive `weight`
contributes `weight * error^2` to the objective. The function uses a
column-pivoted Householder QR rather than normal equations. It reports
rank-deficient and ill-conditioned inputs instead of returning unstable
coefficients.

```cpp
#include <pulp/signal/fir_design.hpp>

using namespace pulp::signal;

std::vector<FirDesignPoint> target{
    {0.0, 1.0, 10.0},
    {0.3, 1.0, 10.0},
    {0.5, 0.0, 1.0},
    {3.141592653589793, 0.0, 20.0},
};

auto design = design_fir_least_squares(
    target,
    {.tap_count = 3,
     .type = LinearPhaseFirType::type_i_symmetric_odd});
if (!design) {
    // Inspect design.status before using any coefficients.
}
```

The four standard real linear-phase forms are explicit:

| Type | Length | Coefficient symmetry | Forced endpoints |
|---|---:|---|---|
| I | odd | symmetric | none |
| II | even | symmetric | zero at Nyquist |
| III | odd | antisymmetric | zero at DC and Nyquist |
| IV | even | antisymmetric | zero at DC |

Targets use signed, phase-removed amplitude—not absolute magnitude. For Types I
and II, remove the linear delay and take the real component. For Types III and
IV, take the coefficient of `+j`; equivalently, the implementation reconstructs
each left/right pair as `+c, -c`. The helper
`linear_phase_fir_amplitude()` applies exactly this convention. This signed
contract lets differentiators and Hilbert-style filters express direction; it
also makes an incompatible nonzero endpoint target show up honestly as error.

On success the result contains coefficients, measured amplitude and signed
error at every input point, weighted RMS error, and maximum absolute error.
`qr_diagonal_condition_estimate` is the ratio of the largest to smallest
accepted pivoted-R diagonal. It is a useful deterministic rejection metric,
but it is not a full matrix condition number. Tune `rank_tolerance` and
`maximum_diagonal_condition_estimate` only with product-level numerical tests.

`maximum_workspace_bytes` admits the complete retained workspace with checked
integer geometry. The default is 256 MiB. A larger grid is accepted when its
actual matrix and result storage fit the caller's budget; there is no unrelated
small point-count cap.

Any non-success result is fail-closed: coefficient and measurement vectors are
empty, including when finite input drives an intermediate calculation outside
the representable range. `rank_tolerance` must be in `(0, 1]`; rank loss and
non-finite arithmetic are reported separately.

## Minimum-phase reconstruction

`reconstruct_minimum_phase_fir()` accepts exactly `N/2 + 1` finite,
nonnegative magnitude bins from DC through Nyquist. The implied `N` must be an
even radix-2 FFT size. It reuses `FftT<double>` with its unscaled forward and
`1/N` inverse normalization:

1. floor magnitudes and form the even real log spectrum;
2. inverse-transform to the real cepstrum;
3. retain DC, double positive quefrencies below `N/2`, retain the Nyquist
   quefrency, and zero negative quefrencies;
4. transform, take the complex exponential, and inverse-transform to the causal
   impulse response.

```cpp
std::vector<double> magnitude_bins = measured_curve; // N/2 + 1 bins
auto minimum_phase = reconstruct_minimum_phase_fir(
    magnitude_bins,
    {.coefficient_count = 256,
     .log_magnitude_floor = 1.0e-10});
```

The logarithm cannot represent an exact zero. Every bin below
`log_magnitude_floor`, including zero, is therefore reconstructed at the floor.
The implementation checks the FFT, complex exponential, coefficients, and
reported measurements for non-finite results.

With `coefficient_count = 0`, all `N` circular impulse samples are retained.
Keeping fewer coefficients is a causal truncation and changes the target
response; it is not a hidden normalization step. `measured_magnitudes`,
`errors`, RMS error, and maximum error always describe the returned,
post-truncation coefficients against the floored target. Increase `N`, retain
more coefficients, or apply a deliberate product-specific window when the
reported truncation error is too large.

The reconstruction is minimum phase within the finite, floored FFT geometry.
It does not perform Remez exchange, minimum-order search, IIR fitting, or
frequency warping.
