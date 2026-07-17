# docs: Capability plan — oversampling seam, half-band latency lane, real inverse FFT, AAX entry point

We're a commercial audio-plugin team building on Pulp — as far as we know, ours will be the first commercial plugin to ship on it. This PR contributes a plan rather than patches: for each gap we describe the capability, the evidence, and the published method, so the design and implementation stay entirely in the Pulp team's hands. We're glad to supply test vectors, characterisation data, and review for any of them.

Four gaps, roughly in the order we hit them. Honest priorities at the end of each.

## 1. Joint-channel block oversampling

**Gap.** The oversampler is fused and single-channel: an instance runs its entire N-sample loop before returning, so there is no seam between the upsample and the downsample stages. Any processing that must see more than one channel at the oversampled rate — a stereo-linked sidechain detector, a shared dither draw across channels — is structurally impossible to express through the current API, not merely awkward.

**Ask.** A block-oriented API that exposes the seam: upsample a block, hand the caller the oversampled buffers for all channels together, then downsample. The existing fused per-sample path stays as the convenience wrapper on top.

**Acceptance.** The test *is* the argument: a two-channel test whose inter-stage work reads both channels' oversampled data (e.g., a max-of-channels gain law) and produces output that no fused per-channel run can. Plus: output identical to the fused path when the inter-stage work is per-channel independent, and an allocation probe confirming nothing allocates between prepare and process.

**Priority.** High for us — with #2 it unblocks a latency-critical processing path.

## 2. A latency-compatible half-band lane

**Gap.** The current half-band design carries roughly 6 samples of group delay per stage. A caller with an existing latency contract, or a fixed latency budget, has no alternative lane to select.

**Ask.** An additional, selectable half-band kind: elliptic polyphase-allpass half-band filters, designed by Valenzuela & Constantinides' published method ("Digital signal processing schemes for efficient interpolation and decimation", IEE Proceedings G, 1983). In our characterisation, an allpass design of this family measures ≈2.34 samples of group delay at 2×, versus the ~6 per stage of the current design — the two are measurably not interchangeable, which is precisely why the caller needs the choice. Both designs are legitimate; neither replaces the other.

**Acceptance.** Characterisation tests: passband ripple and stopband attenuation against a stated spec, measured group delay, reset fully clearing filter state, and a real-time-safety allocation probe on the process path.

**Priority.** High, paired with #1.

## 3. A real-valued inverse FFT

**Gap.** The FFT type has a forward real transform but no inverse. Every consumer that needs one composes it by hand from the complex machinery — repeatedly, and each one can get the normalisation wrong independently.

**Ask.** A real-valued inverse alongside the forward, with its normalisation convention stated in the docs.

**Acceptance.** Round-trip fidelity (forward → inverse ≈ identity within a stated tolerance) on **both** the accelerated path and the portable fallback. The fallback must be exercised explicitly: a round-trip test that takes the accelerated path on Apple hardware cannot fail in the fallback branch and proves nothing about it.

**One correctness trap, noted honestly:** the FFT type has a hand-written move constructor. If the inverse adds a scratch buffer as a new member, that constructor must transfer it — otherwise transforming through a moved-into instance is undefined behaviour. So the plan should include a move-then-transform test that reaches the fallback branch, for the same reason as above.

**Priority.** Lowest of the four — a convenience, but one with a real trap attached, so worth doing deliberately whenever the type is next touched.

## 4. The AAX entry point

**Gap.** `pulp_add_plugin(... FORMATS aax)` wires up `PULP_ENABLE_AAX` / `PULP_AAX_SDK_DIR` and creates the module target, then errors with *"no aax_entry.cpp was found"* — that translation unit does not exist in the tree. AAX is reachable in the build system but not shippable.

**Context.** The vendor SDK we hold is v2.9.0, CMake-native, exporting proper targets, so the integration surface is small. We can supply the SDK on our own machines for testing against; the entry translation unit itself has to live in Pulp.

**Acceptance.** With a valid `PULP_AAX_SDK_DIR`, `FORMATS aax` produces a bundle that loads in the host and passes the format holder's validation tool; without the SDK dir, configuration fails with a clear message (as it already does).

**Priority.** For us this is the only hard blocker in the list — it gates an entire distribution format. In fairness: nobody has shipped this format on Pulp yet, so today we may be the only ones waiting on it.

## A packaging question (not a defect report)

The install step appears to ship about 5 of the ~19 state headers — the preset/undo manager, store, parameter, properties-file, and midi-parameter-map headers are absent from the installed set. Is that an intentional public-API boundary, or a packaging oversight? If the latter, we're happy to enumerate exactly which headers we consume.

---

None of this is a demand. It's the list of the four places where we've had to work around the library rather than with it, written down so the fixes can be designed once, upstream, by the people who own the design. Test vectors, ripple/group-delay characterisation data, and review bandwidth are on offer for all of it.
