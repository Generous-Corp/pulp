# Live-swap on CPU vs GPU — what actually swaps

An honest map of where live-swap works today and where it's a reserved seam, so
nobody assumes "GPU" means the DSP itself hot-swaps on the GPU. (Live-swap plan
item 2.6.)

## UX (the scripted UI / editor) — swaps on CPU, paints on GPU

A UI hot-reload rebuilds the widget tree — parse the new script, build widgets,
run Yoga layout, restore widget state — which is **CPU work**. The GPU only
*paints* the result. So the reload's cost is dominated by the CPU rebuild, not
the GPU:

- **Measured (item 1.2 baseline):** a 24-knob panel rebuilds in **p50 ≈ 13.7 ms
  / p95 ≈ 14.75 ms** (Release, arm64) — about one 60 fps frame. Add the GPU
  first-paint of the new tree (~1 frame, content-independent).
- **Perceived blink** without mitigation is therefore ~1–2 frames. **P0.2's
  hold-last-frame** (rebuild the content subtree under the still-attached host
  view while holding the last rendered frame, swap on the new tree's first
  paint) turns that into a seamless swap.

This holds whether the host is the GPU (Dawn/Skia) window host or the CPU host —
the rebuild is the same CPU tree walk; only the paint backend differs. The
GPU-host gotcha (a CPU-only build silently degrades viewport/aspect handling)
is orthogonal — see the `import-design` skill.

## DSP — swaps on CPU today; GPU-DSP is a RESERVED seam (not built)

Every live-swappable DSP path in the public tree runs on the **CPU** audio
thread: `ProcessorHotSwapSlot` (Processor swap), `ConvolverIrSwapper` (IR swap),
`SignalGraph` (node graph). There is **no live GPU-compute audio-DSP lane** in
the public tree, so there is nothing to hot-swap on the GPU yet.

The Phase-2 `SwapUnit` contract **reserves** a GPU-DSP seam without building it:
- A GPU-DSP resource (device buffers, pipelines, bind groups) must **not** be
  freed on the audio thread — the same rule as CPU retirement, but the "free"
  is a GPU device call. The right pattern is **generation-ack** retirement (see
  `docs/reference/rt-retirement-patterns.md`): the audio thread acks the GPU
  resource generation it adopted; the publisher frees the prior generation's
  device resources off-thread only once acked. (`SampleSlotBank`'s generation-ack
  is the CPU analogue already in the tree.)
- **Build nothing here until a live GPU-audio lane exists.** Reserving the seam
  keeps the `SwapUnit` from needing a format bump later; the retirement pattern
  is documented so a future GPU-DSP lane drops in without reinventing RT-safe
  teardown.

## One-line summary

**UX live-swap: real, CPU-rebuild + GPU-repaint, ~1-frame, hold-last-frame makes
it seamless. DSP live-swap: real on CPU; GPU-compute DSP is a documented reserved
seam (generation-ack retirement), not yet built.**
