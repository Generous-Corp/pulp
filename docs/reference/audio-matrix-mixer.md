# Audio Matrix Mixer

`pulp::signal::AudioMatrixMixerT` is the reusable fixed-topology routing block
for mixing N internal signal paths into M outputs inside one `Processor`. It is
not a `SignalGraph`: the caller supplies planar buffers and owns the surrounding
DSP topology.

## Gain and summing contract

Every cell is a finite signed linear gain. Plans store gains in output-major
order, so `gains[output * input_count + input]` routes one input into one
output. The mixer overwrites each destination with the row sum; it does not
smooth, sanitize samples, or add an implicit output trim. `Direct` does not
clip or saturate. The normalized law clamps only floating-point roundoff beyond
the row's proven maximum-input-magnitude bound.

Two closed summing laws are available:

- `Direct` computes the authored signed sum exactly.
- `NormalizeAbsoluteSum` scales each output by
  `1 / max(1, sum(abs(row gains)))`. It never boosts a quiet row and guarantees
  that inputs bounded by magnitude A produce an output bounded by A.

Normalization is applied to every prepared cell coefficient before processing,
not after accumulating the row. That ordering prevents individually finite
large authored gains from overflowing an intermediate sum before the headroom
scale can take effect. Preparation derives the scale from ratios to the row's
largest magnitude, so computing the scale itself cannot overflow even when a
platform's `long double` has the same range as `double`. If coefficient rounding
would put the stored absolute sum above unity, preparation moves the stored
coefficients one representable value toward zero. Normalized rows accumulate in
a wide scalar and constrain numerical overshoot to the maximum input magnitude
before the final sample conversion, preserving the bound for non-power-of-two
row sizes and platforms where `long double` equals `double`.

The normalization law is deliberately conservative. It cannot infer whether
sources are correlated, so it does not attempt loudness compensation.

## Prepared publication

Build a complete `AudioMatrixPlanT` on the control thread with
`AudioMatrixPlanT::prepare()`. Counts, gain cardinality, finite values, and the
closed summing-law enum are validated before a plan exists. Publish the plan
through the mixer's single-writer `publish()` operation. The audio thread sees
the complete old or new plan at the next `process()` call; it never observes a
partially edited coefficient row.

Plan publication is boundary-latched. Moving a publication boundary can change
which block first uses a new plan. With an unchanged plan, processing uses a
fixed input order for every sample and is bit-exact across block partitions.

## Realtime and buffer rules

`process()` is allocation-free, lock-free, bounded by the template capacities,
and performs no I/O. The default aliases reserve 16 inputs and 16 outputs;
smaller custom capacities reduce the three prepared-plan copies held by the
publication channel.

All consumed spans must contain the requested frame count. Source and
destination frame regions must not overlap, and destination regions must be
pairwise disjoint. Invalid buffer sets fail before any destination is written.
Extra input or output spans are ignored beyond the active plan's declared
counts.

```cpp
using Plan = pulp::signal::AudioMatrixPlanT<float, 4, 2>;
using Mixer = pulp::signal::AudioMatrixMixerT<float, 4, 2>;

// output 0 = input 0 + 0.5 * input 1
// output 1 = input 2 - input 3
const std::array gains{1.0f, 0.5f, 0.0f, 0.0f,
                       0.0f, 0.0f, 1.0f, -1.0f};
auto plan = Plan::prepare(4, 2, gains);
if (!plan)
    return;

Mixer mixer(*plan);
// In process(): mixer.process(input_spans, output_spans, frame_count);
```
