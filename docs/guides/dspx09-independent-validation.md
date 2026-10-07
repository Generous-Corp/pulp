# DSPX-09 independent validation

DSPX-09 is a small, host-independent proof suite for the DSP contracts used by
DSPX-02 through DSPX-08. It gives each contract an independent reference oracle
and a typed negative control while leaving product-specific control ownership in the
canonical registries.

## What the proof covers

`pulp-test-dspx09-independent-validation` runs three focused cases:

- a sinusoidal allpass magnitude oracle, which checks the public TPT allpass
  path against the analytic unity-magnitude property;
- a retained-history refusal fixture, which proves that an explicit `Refuse`
  policy remains bounded and cannot silently become `Adopt`;
- a base-vs-offset automation oracle, which checks effective value composition
  and typed refusals for non-finite and out-of-range values.

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
pulp build --target pulp-test-dspx09-independent-validation
./build/test/pulp-test-group-sampler-host-graph '[dspx-09]'
```

The test binary is grouped with the sampler host graph target; the tag filter
selects only the three DSPX-09 cases.
