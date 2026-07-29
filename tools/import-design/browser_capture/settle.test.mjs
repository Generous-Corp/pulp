// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  captureStableScreenshot,
  freezeAndMeasureDocumentExtent,
  freezeDynamicTime,
  installDynamicWorkTracker,
  validateCaptureDimensions,
  waitForStable,
} from "./settle.mjs";

test("page settle observes a readiness window after an early plateau", async () => {
  let samples = 0;
  const sample = (visible) => JSON.stringify({
    ready: "complete",
    fonts: "loaded",
    width: 100,
    height: 100,
    visible,
    geometry: visible,
    textLength: visible,
  });
  const cdp = {
    async call(method, options) {
      assert.equal(method, "Runtime.evaluate");
      if (options.expression.includes("JSON.stringify({")) {
        samples += 1;
        return { result: { value: sample(samples < 5 ? 1 : 2) } };
      }
      return { result: { value: true } };
    },
  };

  const result = await waitForStable(cdp, {
    stableRounds: 1,
    maximumRounds: 12,
    intervalMs: 10,
    minimumElapsedMs: 35,
  });

  assert.ok(result.elapsedMs >= 35);
  assert.ok(samples >= 4);
});

test("stable screenshot capture uses the settled tail, not an early plateau",
  async () => {
  const frames = ["one", "one", "two", "two", "two"];
  let calls = 0;
  const cdp = {
    async call(method) {
      assert.equal(method, "Page.captureScreenshot");
      const frame = frames[calls++];
      return { data: Buffer.from(frame).toString("base64") };
    },
  };

  const screenshot = await captureStableScreenshot(cdp, {}, frames.length, 0);

  assert.equal(screenshot.toString(), "two");
  assert.equal(calls, frames.length);
});

test("stable screenshot capture rejects a changing compositor", async () => {
  let calls = 0;
  const cdp = {
    async call(method) {
      assert.equal(method, "Page.captureScreenshot");
      return { data: Buffer.from(`frame-${calls++}`).toString("base64") };
    },
  };

  const screenshot = await captureStableScreenshot(cdp, {}, 4, 0);

  assert.equal(screenshot, undefined);
  assert.equal(calls, 4);
});

test("default screenshot horizon drains a late full-page compositor", async () => {
  let calls = 0;
  const cdp = {
    async call(method) {
      assert.equal(method, "Page.captureScreenshot");
      const frame = calls < 20 ? `draining-${calls}` : "settled";
      calls += 1;
      return { data: Buffer.from(frame).toString("base64") };
    },
  };

  const screenshot = await captureStableScreenshot(cdp, {}, undefined, 0);

  assert.equal(screenshot.toString(), "settled");
  assert.equal(calls, 32);
});

test("final capture extent rejects content wider or taller than the axis budget",
  () => {
    for (const [width, height] of [[8193, 1], [1, 8193]]) {
      assert.throws(
        () => validateCaptureDimensions(
          width, height, 2, "final capture extent"),
        (error) => {
          assert.equal(error.code, "capture-viewport-too-large");
          assert.match(error.message, /final capture extent/);
          assert.match(error.message, /8192px axis or 64 megapixel/);
          return true;
        });
    }
  });

test("final capture extent uses the same device-pixel budget as the viewport",
  () => {
    assert.doesNotThrow(
      () => validateCaptureDimensions(4096, 4096, 2));
    assert.throws(
      () => validateCaptureDimensions(
        4097, 4096, 2, "final capture extent"),
      (error) => error.code === "capture-viewport-too-large");
  });

test("same-frame capture cancels page animation before pausing virtual time",
  async () => {
    const calls = [];
    await freezeDynamicTime({
      async call(method, params) {
        calls.push({ method, params });
      },
    });
    assert.deepEqual(calls, [
      {
        method: "Runtime.evaluate",
        params: {
          expression: `(() => {
      window.__pulpFreezeDynamicWork?.();
      window.setTimeout = () => 0;
      window.clearTimeout = () => {};
      window.setInterval = () => 0;
      window.clearInterval = () => {};
      window.requestAnimationFrame = () => 0;
      window.cancelAnimationFrame = () => {};
      return true;
    })()`,
          returnByValue: true,
        },
      },
      {
        method: "Emulation.setVirtualTimePolicy",
        params: { policy: "pause" },
      },
    ]);
  });

test("capture tracks page timers before source execution", async () => {
  const calls = [];
  await installDynamicWorkTracker({
    async call(method, params) {
      calls.push({ method, params });
    },
  });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].method, "Page.addScriptToEvaluateOnNewDocument");
  assert.match(calls[0].params.source, /__pulpFreezeDynamicWork/);
  assert.match(calls[0].params.source, /nativeClearInterval/);
});

test("same-frame capture measures extent only after virtual time is frozen",
  async () => {
    const calls = [];
    const extent = {
      left: 0,
      top: 0,
      width: 640,
      height: 480,
      primarySurface: "document",
    };
    const measured = await freezeAndMeasureDocumentExtent({
      async call(method, params) {
        calls.push({ method, params });
        if (calls.length === 3) return { result: { value: extent } };
        return { result: { value: true } };
      },
    });
    assert.equal(calls[0].method, "Runtime.evaluate");
    assert.equal(calls[1].method, "Emulation.setVirtualTimePolicy");
    assert.equal(calls[2].method, "Runtime.evaluate");
    assert.deepEqual(measured, extent);
  });
