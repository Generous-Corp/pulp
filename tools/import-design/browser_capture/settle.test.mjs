// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  captureStableScreenshot,
  freezeAndMeasureDocumentExtent,
  freezeDynamicTime,
  validateCaptureDimensions,
} from "./settle.mjs";

test("stable screenshot capture allows a bounded compositor drain", async () => {
  const frames = ["one", "two", "three", "three"];
  let calls = 0;
  const cdp = {
    async call(method) {
      assert.equal(method, "Page.captureScreenshot");
      const frame = frames[calls++] ?? "four";
      return { data: Buffer.from(frame).toString("base64") };
    },
  };

  const screenshot = await captureStableScreenshot(cdp, {}, 6, 0);

  assert.equal(screenshot.toString(), "three");
  assert.equal(calls, 4);
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
