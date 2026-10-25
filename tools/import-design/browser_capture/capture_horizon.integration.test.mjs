// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { rm } from "node:fs/promises";
import test from "node:test";

import {
  createEmptyProfile,
  launchBrowser,
  pageTarget,
  terminateBrowser,
} from "./browser_process.mjs";
import {
  CAPTURE_DEADLINE_MS,
  captureCaseTimeout,
  installedBrowser,
} from "./capture_integration_support.mjs";
import {
  captureStableScreenshot,
  SCREENSHOT_HORIZON_FRAMES,
  STABLE_RUN_FRAMES,
} from "./settle.mjs";

function cdpSession(pipe) {
  let nextId = 1000;
  const pending = new Map();
  pipe.onMessage((data) => {
    const message = JSON.parse(data);
    const settle = pending.get(message.id);
    if (!settle) return;
    pending.delete(message.id);
    settle(message);
  });
  return (method, params = {}, sessionId = undefined) =>
    new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, (message) => message.error
        ? reject(new Error(`${method}: ${message.error.message}`))
        : resolve(message.result));
      pipe.send(JSON.stringify(
        { id, method, params, ...(sessionId ? { sessionId } : {}) }));
    });
}

// The rule with every frame taken at the caller's own options: the horizon as
// it was before presentation frames, kept here as the equivalence oracle.
async function referenceHorizon(cdp, options) {
  let previous;
  let run = 0;
  for (let index = 0; index < SCREENSHOT_HORIZON_FRAMES; index += 1) {
    const shot = await cdp.call("Page.captureScreenshot", options);
    const current = Buffer.from(shot.data, "base64");
    run = previous?.equals(current) ? run + 1 : 1;
    previous = current;
    if (index + 1 < SCREENSHOT_HORIZON_FRAMES)
      await new Promise((resolve) => setTimeout(resolve, 16));
  }
  return run >= STABLE_RUN_FRAMES ? previous : undefined;
}

const PAGE = "data:text/html," + encodeURIComponent(
  "<!doctype html><style>html,body{margin:0;background:#1b2330}" +
  "#box{position:absolute;left:40px;top:40px;width:200px;height:120px;" +
  "background:linear-gradient(90deg,#3a7bd5,#00d2ff);border-radius:12px}" +
  "</style><div id=box></div><p style='position:absolute;top:200px;" +
  "left:40px;color:#fff;font:16px sans-serif'>horizon</p>");

const OPTIONS = Object.freeze({
  format: "png",
  captureBeyondViewport: true,
  fromSurface: true,
  clip: { x: 0, y: 0, width: 640, height: 400, scale: 1 },
});

async function withPage(context, body) {
  const browser = await installedBrowser();
  if (!browser) {
    context.skip("no compatible system browser is installed");
    return;
  }
  const profile = await createEmptyProfile();
  let launched;
  try {
    launched = await launchBrowser(browser, profile, CAPTURE_DEADLINE_MS);
    const call = cdpSession(launched.pipe);
    const target = await pageTarget(call, CAPTURE_DEADLINE_MS);
    const { sessionId } = await call(
      "Target.attachToTarget", { targetId: target.targetId, flatten: true });
    const page = (method, params) => call(method, params, sessionId);
    await page("Page.enable");
    await page("Emulation.setDeviceMetricsOverride", {
      width: 640, height: 400, deviceScaleFactor: 2, mobile: false });
    await page("Page.navigate", { url: PAGE });
    await page("Runtime.evaluate", {
      expression: "document.fonts.ready.then(() => true)", awaitPromise: true });
    // Presentations are counted in an isolated world, whose requestAnimationFrame
    // a capture's freeze never stubs, so the count observes the compositor and
    // not the page's own scheduling.
    const { frameTree } = await page("Page.getFrameTree");
    const { executionContextId } = await page("Page.createIsolatedWorld", {
      frameId: frameTree.frame.id, worldName: "pulp-presentation-counter" });
    await page("Runtime.evaluate", {
      contextId: executionContextId,
      expression: "globalThis.presented = 0;" +
        "(function tick() { presented += 1; requestAnimationFrame(tick); })();",
    });
    const presented = async () => (await page("Runtime.evaluate", {
      contextId: executionContextId, expression: "presented",
      returnByValue: true })).result.value;
    await body({ page, presented });
  } finally {
    if (launched) await terminateBrowser(launched.child);
    await rm(profile, { recursive: true, force: true });
  }
}

// A CDP handle whose Nth screenshot first repaints the page, so the change
// lands in exactly that presented frame. The page starts from its authored
// paint, so two arms given the same frame see the same sequence.
async function changingAt(page, frame) {
  await page("Runtime.evaluate", {
    expression: "document.getElementById('box').style.background = ''",
  });
  await page("Runtime.evaluate", {
    expression: "new Promise(r => requestAnimationFrame(() => r(true)))",
    awaitPromise: true,
  });
  let screenshots = 0;
  return {
    async call(method, params) {
      if (method === "Page.captureScreenshot" && ++screenshots === frame) {
        await page("Runtime.evaluate", {
          expression: "document.getElementById('box').style.background = " +
            `'#${(0x200000 + frame * 4099).toString(16)}'`,
        });
        await page("Runtime.evaluate", {
          expression: "new Promise(r => requestAnimationFrame(() => r(true)))",
          awaitPromise: true,
        });
      }
      return page(method, params);
    },
  };
}

test("presentation frames read the same pixels and present as many frames",
  { timeout: captureCaseTimeout(2), skip: process.platform === "win32" },
  async (context) => {
    await withPage(context, async ({ page, presented }) => {
      const cdp = { call: page };
      const before = await presented();
      const reference = await referenceHorizon(cdp, OPTIONS);
      const between = await presented();
      const optimized = await captureStableScreenshot(cdp, OPTIONS);
      const after = await presented();

      assert.ok(reference, "a still page passes the reference horizon");
      assert.ok(optimized, "a still page passes the optimized horizon");
      assert.ok(reference.equals(optimized),
        "both horizons ship byte-identical PNG bytes");
      assert.ok(between - before >= SCREENSHOT_HORIZON_FRAMES,
        `reference horizon presented ${between - before} frames`);
      assert.ok(after - between >= SCREENSHOT_HORIZON_FRAMES,
        `optimized horizon presented ${after - between} frames`);
    });
  });

test("a change inside the decisive run is rejected and one before it is not",
  { timeout: captureCaseTimeout(4), skip: process.platform === "win32" },
  async (context) => {
    await withPage(context, async ({ page }) => {
      // Frame 31 is inside the decisive run: both horizons must reject.
      assert.equal(await referenceHorizon(await changingAt(page, 31), OPTIONS),
        undefined);
      assert.equal(await captureStableScreenshot(await changingAt(page, 31), OPTIONS),
        undefined);
      // Frame 28 precedes it and the page is still from then on: both accept,
      // with identical bytes.
      const reference = await referenceHorizon(await changingAt(page, 28), OPTIONS);
      const optimized =
        await captureStableScreenshot(await changingAt(page, 28), OPTIONS);
      assert.ok(reference && optimized);
      assert.ok(reference.equals(optimized));
    });
  });
