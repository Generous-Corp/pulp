# GPU-audio P4 stress audit — 2026-10-04

Status: **gap audit; no stress receipt**.

The audit used a clean detached worktree at `origin/main` (`bcf5d11a30ba02b8a90de88fd88f1030dd03f9ce`). The checked-in P4 evidence was inspected without relabeling or extending it.

## Existing evidence

The paced matched screening receipt has SHA-256 `84d39fd73f146ef28b8a89e831b53a801829ad371cb43e0bd50570e1085dd127`, with source commit `d619e6c6e05c247739a029a2b70a262b19966ebe` and benchmark executable SHA-256 `63deef5ba87037cc4ca74188bd8f5aca64f4214b2be1491d0fb795a3e68e0d11`. It observed 12/12 staged and shared terminal records, but reported 10 shared deadline misses, `performance_verdict: "unassigned"`, and `raw_receipt: "not_emitted"`.

The 32-frame lead sweep manifest SHA-256 is `7872373cf7ba07c1ab06b76fd316ed52639885361785e8d1fd4e9b9e16ba7a2a`. Every lead/wakeup run recorded 1,024 misses and only two produced blocks, so it is an ingress-saturation diagnostic. The 128-frame manifest SHA-256 is `ff2ea7bf8eeb78cfc0a4a3152c8a0a8e7b1d5675a5b632280fb0db4ef3b88b1a`; all five runs recorded zero misses and 145–152 produced blocks, but remain explicitly diagnostic and unassigned.

## Host gate

At `2026-10-04T07:43:17Z`, the M5 Max host (`Mac17,7`, 18 cores, 128 GiB, macOS build `25G83`) reported load averages `15.03 15.96 19.07`. It was on AC power with a full battery. A live Virtualization.framework VM was using approximately 99.5% CPU at the final sample (an earlier sample was approximately 590% CPU). `pmset -g therm` reported no recorded thermal/performance warning and no CPU power status; that is not a power receipt. Because the host was already heavily contended, adding CPU or graphics load would be unsafe and would not provide attributable P4 evidence. No stress run was executed.

## Remaining acceptance gap

The next run needs a quiescent Apple Silicon host and a strict `pulp.gpu-audio.p4.raw.v1` writer with honest callback/result timing and transfer-counter provenance. The campaign still requires the declared matched confirmation matrix (at least 30 complete pairs and 10,000 bootstrap resamples) plus a separate 100,000-block default-profile long-tail run under concurrent UI/GPU load. The current 12-block matched diagnostic cannot satisfy those gates or claim a performance verdict.
