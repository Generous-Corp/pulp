# FIR design from sampled targets

`<pulp/signal/fir_design.hpp>` turns a sampled specification or measurement into
portable real FIR coefficients. It complements the parameter-driven
windowed-sinc helpers: use windowed sinc for a conventional cutoff, and this API
when the curve itself is the input.

The designer allocates and may perform substantial numerical work. Call it
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

Type I is the conventional magnitude-EQ form. Types II-IV are included because
they reuse the same pivoted-QR solver and differ only in their analytic basis
and symmetry reconstruction; the focused suite independently recovers every
form and pins its forced endpoints. They are useful for even-length magnitude
filters, differentiators, and Hilbert-style filters without introducing a
second solver or design framework.

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

Targets are absolute signed amplitudes. The designer performs no implicit DC,
peak, energy, or coefficient-sum normalization. Apply any product-specific
normalization explicitly after checking how it changes the weighted objective.

Tap count is bounded at 1,023 and grid size at 65,536 points. Within those
limits, `maximum_workspace_bytes` admits the complete retained workspace with
checked integer geometry; the default is 256 MiB. The returned result owns all
coefficient and measurement vectors and retains no caller spans. When
exceptions are enabled, allocation failure is translated to
`FirDesignStatus::allocation_failure`; in a no-exceptions build, the platform's
allocator failure policy applies.

Any non-success result is fail-closed: coefficient and measurement vectors are
empty, including when finite input drives an intermediate calculation outside
the representable range. `rank_tolerance` must be in `(0, 1]`; rank loss and
non-finite arithmetic are reported separately.

This API does not perform minimum-phase reconstruction, Remez exchange,
minimum-order search, IIR fitting, or frequency warping. Those are separate
algorithms with distinct validation and lifecycle contracts.
