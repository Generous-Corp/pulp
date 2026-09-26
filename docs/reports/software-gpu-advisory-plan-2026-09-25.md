# Software-GPU advisory plan (2026-09-25)

This is the implementation companion to the canonical GPU import plan on
`danielraffel/pulp-planning` (`plan/import-graphite-design-acceleration-20260918`).
It records the next useful cross-platform evidence that can run on ordinary
GitHub-hosted runners.

## Decision and boundary

Pulp will not depend on a real Linux or Windows GPU runner in this phase. The
Mac Pro and `300 pulp-win-ci` are not part of the lane. Linux uses Mesa's
lavapipe software Vulkan adapter; Windows uses Dawn's fallback D3D12 adapter
(WARP). These checks are advisory, never required, and never merge-queue
blocking. A failure opens or updates the existing platform tracking issue.

The lane proves GPU API execution and readback through the existing typed
`pulp.gpu-probe-result.v1` contract. It does not claim native GPU performance,
native presentation, or production capture ownership. #8457 capture, #8448/
#8449 scheduling, #8458 observability, and Vellum-owned rendering remain
separate workstreams.

## Priority order

1. **P0 — software adapter selection and identity.** Set
   `PULP_GPU_SOFTWARE_ADAPTER=1` only in these lanes. The receipt must identify
   an authentic `software` adapter and the platform backend (Vulkan on Linux,
   D3D12 on Windows).
2. **P0 — render/readback proof.** Run the existing hardcoded-cube recipe and
   retain its non-empty `final.png`, dimensions, and SHA-256 receipt. The
   portable structure oracle is accepted where no platform-specific golden is
   scoped.
3. **P0 — break-confirm.** Run the positive proof, then the seeded
   `pre-submit-framebuffer-downscale` mutation. The second run must return a
   typed `fail` at the content boundary. If the adapter or receipt is missing,
   the lane fails closed and the platform tracker records follow-up work.
4. **P1 — platform parity backlog.** Keep native-driver performance, exact
   platform goldens, and real-GPU presentation as separately staged issues.
   They can be promoted when owned hardware or a reproducible hosted driver
   becomes available.

## Evidence vocabulary

Use TTFP, TTNI, IFNF, and `cache-state` only for the existing observability
slice and its twelve-fixture differential corpus. This lane contributes GPU
adapter, render, readback, content, and artifact evidence; it does not add
threading guidance or reinterpret those observability metrics.

## Deferred work

Issues #8745 (Linux real-GPU runner) and #8746 (Windows real-GPU runner) are
deferred infrastructure. Their acceptance criteria should remain available for
future hardware, but they are not prerequisites for this advisory phase.
