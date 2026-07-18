import { test } from "node:test";
import assert from "node:assert/strict";
import { recommendLatencyBlocks, DepthController } from "./adaptive-depth.mjs";

const BUDGET = 512 / 48000 * 1e6;   // ≈ 10667 µs, one 512-sample block at 48k

test("a fast GPU (round trip << budget) picks the minimum depth", () => {
  // Headless-Chrome-here measured ~3200µs round trip → 1.5*3200/10667 = 0.45 → clamps to min.
  assert.equal(recommendLatencyBlocks(3200, BUDGET), 2);
  assert.equal(recommendLatencyBlocks(200, BUDGET), 2);
});

test("a jittery GPU whose round trip EXCEEDS the budget picks a deeper pipeline", () => {
  // Real-Safari measured ~20800µs (195% of budget) → ceil(1.5*20800/10667) = ceil(2.93) = 3 blocks.
  assert.equal(recommendLatencyBlocks(20800, BUDGET), 3);
  // ~12900µs (120%) → ceil(1.5*12900/10667) = ceil(1.81) = 2.
  assert.equal(recommendLatencyBlocks(12900, BUDGET), 2);
  // A very slow device (round trip ~4 budgets) → ceil(1.5*4) = 6.
  assert.equal(recommendLatencyBlocks(4 * BUDGET, BUDGET), 6);
});

test("headroom widens the pipeline; clamps hold", () => {
  assert.equal(recommendLatencyBlocks(20800, BUDGET, { headroom: 3 }), 6);   // ceil(3*1.95)=6
  assert.equal(recommendLatencyBlocks(1e9, BUDGET, { max: 4 }), 4);          // clamp to max
  assert.equal(recommendLatencyBlocks(1e9, BUDGET, { min: 3, max: 12 }), 12);
  assert.equal(recommendLatencyBlocks(0, BUDGET), 2);                        // degenerate → min
  assert.equal(recommendLatencyBlocks(5000, 0), 2);                         // degenerate → min
});

test("DepthController debounces: a new depth applies only after it holds", () => {
  const c = new DepthController(2, { hold: 3 });
  assert.equal(c.update(4), 2);   // 1st sample of 4 — not yet
  assert.equal(c.update(4), 2);   // 2nd
  assert.equal(c.update(4), 4);   // 3rd → applies
  assert.equal(c.update(4), 4);   // stays
});

test("DepthController rejects noise: a one-off spike does not flip the depth", () => {
  const c = new DepthController(2, { hold: 3 });
  assert.equal(c.update(6), 2);   // spike
  assert.equal(c.update(2), 2);   // back to current → resets the pending count
  assert.equal(c.update(6), 2);   // spike again, count restarts at 1
  assert.equal(c.update(6), 2);   // 2
  assert.equal(c.update(6), 6);   // 3 sustained → applies
});
