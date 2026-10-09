# DSPX-09 independent validation

DSPX-09 is a small, host-independent proof suite for the DSP contracts used by
DSPX-02 through DSPX-08. It gives each contract an independent reference oracle
and a typed negative control while leaving product-specific control ownership in the
canonical registries.

## What the proof covers

The original `pulp-test-dspx09-independent-validation` target runs three focused
cases. The follow-up target `pulp-test-dspx09-followup` adds four executable
cases in the same grouped sampler/host binary:

- a sinusoidal allpass magnitude oracle, which checks the public TPT allpass
  path against the analytic unity-magnitude property;
- a retained-history refusal fixture, which proves that an explicit `Refuse`
  policy remains bounded and cannot silently become `Adopt`;
- a base-vs-offset automation oracle, which checks effective value composition
  and typed refusals for non-finite and out-of-range values.

The follow-up cases add:

- a bounded delay impulse oracle with negative and non-finite delay refusals;
- retained-history adopt and incompatible-identity refusal through the live swap
  fixture, with graph topology unchanged on refusal;
- ordered dense automation delivery and explicit queue-overflow refusal;
- a projection comparator that accepts the Pulp lane while keeping Forge
  development-only and Spectr/GPU-NAM `DeferredExternalOwner`.

The suite is deliberately independent of sample-region kernels. It does not
register a second graph, control broker, scheduler, or Forge authority.

## Surface and evidence boundary

The namespaced exposure row records the executable test and its dispositions
for the installed SDK, offline controls, live product control, and design-time
manifest. A green local test proves the public Pulp contract only. The Forge
projection dependency is the development SDK/catalog receipt at
[Forge PR 236](https://github.com/Generous-Corp/forge/pull/236), commit
[`4b3b4bc52dc4ade8a0d774a165131e2f3ee0dd75`](https://github.com/Generous-Corp/forge/commit/4b3b4bc52dc4ade8a0d774a165131e2f3ee0dd75),
with its receipt at
[docs/dspx08-forge-projection-20261007.md](https://github.com/Generous-Corp/forge/blob/4b3b4bc52dc4ade8a0d774a165131e2f3ee0dd75/docs/dspx08-forge-projection-20261007.md).
That development receipt does not establish packaged distribution or GPU execution. Spectr and GPU-NAM remain `DeferredExternalOwner` lanes and are not
registered by this packet.

Run the focused proof after configuring a Release build:

```bash
pulp build --target pulp-test-dspx09-followup
./build/test/pulp-test-group-sampler-host-graph '[dspx-09][follow-up]'
```

The test binary is grouped with the sampler host graph target; the tag filter
selects only the four follow-up cases. The fixture translation unit also carries
the existing DSPX-03 lifecycle cases, so the focused receipt includes their
assertions while the filter selects only the new follow-up names.
