// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  animatedImageKind,
  captureStableFrameWithHiddenPaint,
  captureStableScreenshot,
  presentationFrameOptions,
  PRESENTATION_FRAME_ENCODING,
  SCREENSHOT_HORIZON_FRAMES,
  STABLE_RUN_FRAMES,
  freezeAndMeasureDocumentExtent,
  freezeDynamicTime,
  freezeMotion,
  horizonPresentationEncoding,
  MOTION_COMMIT_TIMEOUT_MS,
  installDynamicWorkTracker,
  resumeDynamicTime,
  validateCaptureDimensions,
  waitForStable,
} from "./settle.mjs";

test("pixels are read with virtual time running again", async () => {
  const calls = [];
  await resumeDynamicTime({
    async call(method, params) {
      calls.push({ method, params });
    },
  });
  assert.deepEqual(calls, [{
    method: "Emulation.setVirtualTimePolicy",
    params: { policy: "advance" },
  }]);
});

test("page settle reaches a transition inside its minimum readiness window", async () => {
  const minimumElapsedMs = 35;
  const observedVisibility = new Set();
  let transitionAt;
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
        const firstSample = transitionAt === undefined;
        if (firstSample) transitionAt = Date.now() + 2;
        const visible = firstSample || Date.now() < transitionAt ? 1 : 2;
        observedVisibility.add(visible);
        return { result: { value: sample(visible) } };
      }
      return { result: { value: true } };
    },
  };

  const result = await waitForStable(cdp, {
    stableRounds: 2,
    maximumRounds: 50,
    intervalMs: 1,
    minimumElapsedMs,
  });

  assert.ok(result.elapsedMs >= minimumElapsedMs);
  assert.deepEqual([...observedVisibility], [1, 2]);
});

test("page settle ignores mutation-only React churn", async () => {
  let samples = 0;
  const cdp = {
    async call(method, options) {
      assert.equal(method, "Runtime.evaluate");
      if (options.expression.includes("JSON.stringify({")) {
        samples += 1;
        return { result: { value: JSON.stringify({
          ready: "complete",
          fonts: "loaded",
          width: 1320,
          height: 860,
          visible: 83,
          geometry: 2365765203,
          textLength: 92,
          mutationEpoch: samples,
        }) } };
      }
      return { result: { value: true } };
    },
  };

  const result = await waitForStable(cdp, {
    stableRounds: 2,
    maximumRounds: 8,
    intervalMs: 0,
  });

  assert.equal(result.stableRounds, 2);
  assert.equal(samples, 3);
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

test("a frame that hid nothing is the accepted frame, with no new horizon",
  async () => {
  let calls = 0;
  const cdp = {
    async call() {
      calls += 1;
      return { data: Buffer.from("replacement").toString("base64") };
    },
  };
  const accepted = Buffer.from("accepted");

  const frame = await captureStableFrameWithHiddenPaint(cdp, {}, accepted, 0);

  assert.equal(frame, accepted);
  assert.equal(calls, 0);
});

test("a frame that hid paint, or cannot say, observes its own horizon",
  async () => {
  for (const hiddenCount of [2, undefined, null, "0"]) {
    let calls = 0;
    const cdp = {
      async call(method) {
        assert.equal(method, "Page.captureScreenshot");
        calls += 1;
        return { data: Buffer.from("plate").toString("base64") };
      },
    };

    const frame = await captureStableFrameWithHiddenPaint(
      cdp, {}, Buffer.from("accepted"), hiddenCount);

    assert.equal(frame.toString(), "plate", `hidden count ${hiddenCount}`);
    assert.equal(calls, 32, `hidden count ${hiddenCount}`);
  }
});

test("a hidden-paint frame gets one fresh horizon when the first misses its tail",
  async () => {
  let calls = 0;
  const cdp = {
    async call() {
      const frame = calls < 32 ? `moving-${calls}` : "plate";
      calls += 1;
      return { data: Buffer.from(frame).toString("base64") };
    },
  };

  const frame = await captureStableFrameWithHiddenPaint(
    cdp, {}, Buffer.from("accepted"), 1);

  assert.equal(frame.toString(), "plate");
  assert.equal(calls, 64);
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

// One-pixel GIF frames: a graphic control block, an image descriptor and the
// three-code LZW stream (clear, the colour index, end) for that pixel.
function gifBytes(frameIndices) {
  const frames = frameIndices.flatMap((index) => [
    0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0x00,
    0x02, 0x02, index === 0 ? 0x44 : 0x4c, 0x01, 0x00,
  ]);
  return Uint8Array.from([
    ...ascii("GIF89a"),
    1, 0, 1, 0, 0x80, 0, 0,
    0xff, 0, 0, 0, 0, 0xff,
    ...frames, 0x3b,
  ]);
}

const ascii = (text) => text.split("").map((c) => c.charCodeAt(0));

function pngBytes(frameCount) {
  const be = (n) => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
  const withCrc = (type, data) =>
    [...be(data.length), ...ascii(type), ...data, 0, 0, 0, 0];
  return Uint8Array.from([
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    ...withCrc("IHDR", new Array(13).fill(0)),
    ...(frameCount === undefined ? [] : withCrc("acTL", [...be(frameCount), ...be(0)])),
    ...withCrc("IDAT", [1, 2, 3]),
    ...withCrc("IEND", []),
  ]);
}

function webpBytes(frameCount) {
  const le = (n) => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255];
  const chunk = (type, data) => [...ascii(type), ...le(data.length), ...data];
  const body = [
    ...ascii("WEBP"),
    ...chunk("VP8X", [0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ...chunk("ANIM", [0, 0, 0, 0, 0, 0]),
    // An odd-sized frame exercises the RIFF pad byte.
    ...Array.from({ length: frameCount }, () => [...chunk("ANMF", [7, 7, 7]), 0])
      .flat(),
  ];
  return Uint8Array.from([...ascii("RIFF"), ...le(body.length), ...body]);
}

test("animated images are told apart from still ones by their frames", () => {
  assert.equal(animatedImageKind(gifBytes([0, 1])), "gif");
  assert.equal(animatedImageKind(gifBytes([0])), null);
  assert.equal(animatedImageKind(pngBytes(2)), "apng");
  // A one-frame APNG and a plain PNG both show a single image.
  assert.equal(animatedImageKind(pngBytes(1)), null);
  assert.equal(animatedImageKind(pngBytes(undefined)), null);
  assert.equal(animatedImageKind(webpBytes(2)), "webp");
  assert.equal(animatedImageKind(webpBytes(1)), null);
  assert.equal(animatedImageKind(Uint8Array.from([0xff, 0xd8, 0xff, 0xe0])), null);
  assert.equal(animatedImageKind(new Uint8Array()), null);
  // A truncated second frame is not proof of animation.
  const truncated = gifBytes([0, 1]);
  assert.equal(animatedImageKind(truncated.subarray(0, 40)), null);
});

test("the motion freeze runs once in the page and returns its receipt",
  async () => {
    const calls = [];
    const receipt = { schema: "pulp-browser-capture-motion-v1" };
    const returned = await freezeMotion({
      async call(method, params) {
        calls.push({ method, params });
        return { result: { value: { receipt, changed: false } } };
      },
    });
    // Nothing changed, so nothing waits for a commit.
    assert.deepEqual(returned, { ...receipt,
      frame_commit_waits: { presented: 0, timed_out: 0, unavailable: 0 } });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].method, "Runtime.evaluate");
    assert.equal(calls[0].params.awaitPromise, true);
    assert.equal(calls[0].params.returnByValue, true);
    const expression = calls[0].params.expression;
    // The page receives the same classifier the unit tests exercise.
    assert.ok(expression.includes(animatedImageKind.toString()));
    assert.doesNotThrow(() => new Function(`return ${expression};`));
    for (const step of ["getAnimations", "finish()", "pause()",
      "currentTime = 0", "querySelectorAll(\"video\")", "volatile_sources"]) {
      assert.ok(expression.includes(step), step);
    }
  });

test("a freeze that changed motion waits for a commit, bounded in real time",
  async () => {
    // Every pass changes something; the commit wait is the call that asks
    // the page for animation frames.
    const page = (frames) => {
      const calls = [];
      return { calls, async call(method, params) {
        calls.push(params.expression);
        if (!params.expression.startsWith("new Promise")) {
          return { result: { value: { receipt: {}, changed: true } } };
        }
        return frames();
      } };
    };
    const presented = page(async () => ({ result: { value: true } }));
    assert.deepEqual((await freezeMotion(presented)).frame_commit_waits,
      { presented: 1, timed_out: 0, unavailable: 0 });
    assert.ok(presented.calls[1].includes("requestAnimationFrame"));

    // Frames that never come (a paused virtual clock): the pass still returns,
    // within the real-time bound, and says it timed out.
    const started = Date.now();
    const stalled = page(() => new Promise(() => {}));
    assert.deepEqual((await freezeMotion(stalled)).frame_commit_waits,
      { presented: 0, timed_out: 1, unavailable: 0 });
    const elapsed = Date.now() - started;
    assert.ok(elapsed >= MOTION_COMMIT_TIMEOUT_MS && elapsed < MOTION_COMMIT_TIMEOUT_MS + 1000,
      `${elapsed} ms`);

    // Each page connection keeps its own tally across passes.
    await freezeMotion(stalled);
    assert.equal((await freezeMotion(stalled)).frame_commit_waits.timed_out, 3);
  });

test("a motion freeze the page rejects fails the capture by name", async () => {
  await assert.rejects(
    freezeMotion({
      async call() {
        return { exceptionDetails: { text: "Uncaught", exception: {
          description: "TypeError: boom" } } };
      },
    }),
    (error) => error.code === "capture-motion-freeze-failed" &&
      /TypeError: boom/.test(error.message));
});

// A page whose presented frame N shows `pixelsAt(N)`, behind an encoder whose
// output depends on the requested format the way a real one does.
function encodingPage(pixelsAt) {
  const requests = [];
  return {
    requests,
    async call(method, options) {
      assert.equal(method, "Page.captureScreenshot");
      requests.push(options);
      const pixels = pixelsAt(requests.length);
      const encoded = `${options.format}/${options.quality ?? "-"}/${pixels}`;
      return { data: Buffer.from(encoded).toString("base64") };
    },
  };
}

// The rule as it stood before presentation frames: every frame with the
// caller's options. Option-for-option equivalence is judged against it.
async function referenceHorizon(cdp, options, frames, required) {
  let previous;
  let run = 0;
  for (let index = 0; index < frames; index += 1) {
    const shot = await cdp.call("Page.captureScreenshot", options);
    const current = Buffer.from(shot.data, "base64");
    run = previous?.equals(current) ? run + 1 : 1;
    previous = current;
  }
  return run >= required ? previous : undefined;
}

const canonicalOptions = Object.freeze({
  format: "png",
  captureBeyondViewport: true,
  fromSurface: true,
  clip: { x: 0, y: 0, width: 1280, height: 800, scale: 1 },
});

test("the horizon length and stable run are pinned", () => {
  // Presentation frames are equivalent only while the stable run equals the
  // number of decisive frames; captureStableScreenshot ties the two together,
  // and this pins the values every capture horizon has been proven at.
  assert.equal(SCREENSHOT_HORIZON_FRAMES, 32);
  assert.equal(STABLE_RUN_FRAMES, 3);
});

test("only the decisive frames carry the caller's encoding; geometry never changes",
  async () => {
    const cdp = encodingPage(() => "still");
    await captureStableScreenshot(cdp, canonicalOptions, undefined, 0);
    assert.equal(cdp.requests.length, SCREENSHOT_HORIZON_FRAMES);
    const presentation = SCREENSHOT_HORIZON_FRAMES - STABLE_RUN_FRAMES;
    for (const [index, options] of cdp.requests.entries()) {
      if (index >= presentation) {
        assert.deepEqual(options, canonicalOptions);
        continue;
      }
      const { format, quality, ...geometry } = options;
      const { format: _format, ...canonicalGeometry } = canonicalOptions;
      assert.deepEqual(geometry, canonicalGeometry);
      assert.equal(format, PRESENTATION_FRAME_ENCODING.format);
      assert.equal(quality, PRESENTATION_FRAME_ENCODING.quality);
    }
    assert.deepEqual(presentationFrameOptions(canonicalOptions).clip,
      canonicalOptions.clip);
  });

test("presentation frames decide nothing the original rule did not", async () => {
  const frames = SCREENSHOT_HORIZON_FRAMES;
  const cases = {
    // Still throughout: accepted, the shipped bytes are the canonical frame.
    still: () => "settled",
    // Changes at frame 28, still from 29 on: accepted identically.
    "change at 28": (n) => (n < 28 ? "draining" : "settled"),
    // Changes at frame 31: rejected by both.
    "change at 31": (n) => (n < 31 ? "settled" : "moved"),
    // Never still: rejected by both.
    "always moving": (n) => `frame-${n}`,
  };
  for (const [name, pixelsAt] of Object.entries(cases)) {
    const optimized = await captureStableScreenshot(
      encodingPage(pixelsAt), canonicalOptions, frames, 0);
    const reference = await referenceHorizon(
      encodingPage(pixelsAt), canonicalOptions, frames, STABLE_RUN_FRAMES);
    assert.deepEqual(optimized, reference, name);
  }
  const accepted = await captureStableScreenshot(
    encodingPage((n) => (n < 28 ? "draining" : "settled")),
    canonicalOptions, frames, 0);
  assert.equal(accepted.toString(), "png/-/settled");
  assert.equal(await captureStableScreenshot(
    encodingPage((n) => (n < 31 ? "settled" : "moved")),
    canonicalOptions, frames, 0), undefined);
});

test("a shorter horizon keeps its own decisive run", async () => {
  // The isolated-canvas horizon takes 6 frames and needs a run of 2.
  const cdp = encodingPage(() => "canvas");
  const bytes = await captureStableScreenshot(cdp, canonicalOptions, 6, 0, 2);
  assert.equal(bytes.toString(), "png/-/canvas");
  assert.deepEqual(cdp.requests.slice(4), [canonicalOptions, canonicalOptions]);
  assert.equal(cdp.requests.slice(0, 4).every(
    (options) => options.format === PRESENTATION_FRAME_ENCODING.format), true);
});

test("a declared animated image keeps every horizon frame in the caller's encoding",
  async () => {
    const volatile = { volatile_sources: [{ kind: "animated-image", format: "gif" }] };
    assert.equal(horizonPresentationEncoding(volatile), null);
    assert.equal(horizonPresentationEncoding({ volatile_sources: [] }),
      PRESENTATION_FRAME_ENCODING);
    const cdp = encodingPage(() => "still");
    const bytes = await captureStableScreenshot(cdp, canonicalOptions,
      undefined, 0, undefined, horizonPresentationEncoding(volatile));
    assert.equal(bytes.toString(), "png/-/still");
    assert.equal(cdp.requests.length, SCREENSHOT_HORIZON_FRAMES);
    for (const options of cdp.requests) assert.deepEqual(options, canonicalOptions);
  });

test("a hidden-paint horizon takes the encoding it is given", async () => {
  const cdp = encodingPage(() => "hidden");
  const bytes = await captureStableFrameWithHiddenPaint(
    cdp, canonicalOptions, Buffer.from("accepted"), 1, null);
  assert.equal(bytes.toString(), "png/-/hidden");
  for (const options of cdp.requests) assert.deepEqual(options, canonicalOptions);
});
