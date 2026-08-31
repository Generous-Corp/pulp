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

`design_fir_least_squares()` does not itself perform Remez exchange; see the
equiripple section below. Minimum-order search, IIR fitting, and frequency
warping remain separate algorithms with distinct validation and lifecycle
contracts, and are not provided here.

## Equiripple design

`design_fir_equiripple()` solves the weighted-minimax problem by the Remez
exchange, the Parks and McClellan method. Where the least-squares designer
minimizes weighted squared error and lets the worst case fall where it may,
this equalizes the weighted error across the approximation bands, which is what
lets a caller state stopband depth and transition width as *requirements*
rather than discover them after the fact.

Input is a set of ascending, disjoint `FirEquirippleBand` requirements in
radians/sample on `[0, pi]`, each with a signed zero-phase amplitude and a
positive weight. Frequencies between bands are transition regions: they are
neither approximated nor weighted. A band weighted `k` times higher converges to
`k` times smaller ripple, so the weight ratio is the design's ripple ratio.

The result reports the achieved per-band ripple, the alternation set, the
equalized minimax error, the peak error measured on the design grid, and the
iteration count. For `r` independent coefficients a converged design exhibits
`r + 1` alternating extrema of equal weighted magnitude; that alternation
property is the acceptance oracle, checkable from the returned taps alone.

This is offline design work. It allocates and iterates, is never audio-callback
reachable, and costs `O(iterations * r^3)` in the number of independent
coefficients, so large tap counts are deliberately an offline expense. It is
deterministic: grid construction, the initial alternation set, pivoting, and
extremum tie-breaking are all fixed.

Failure is closed and explicit. `not_converged` reports both an exhausted
iteration budget and a converged design whose minimax error exceeds a stated
`maximum_minimax_error`, which is how an infeasible spec at a given tap count is
distinguished from a filter that silently misses it. Malformed band sets,
non-positive weights, frequencies outside `[0, pi]`, and a tap count that does
not match the requested linear-phase type are rejected as `invalid_argument`.
Every non-success result carries empty coefficients.

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
