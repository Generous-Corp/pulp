// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
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
  rgbaPixel,
  runCapture,
} from "./capture_integration_support.mjs";
import { captureStableScreenshot, freezeMotion } from "./settle.mjs";

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

const OPTIONS = Object.freeze({
  format: "png",
  captureBeyondViewport: false,
  fromSurface: true,
  clip: { x: 0, y: 0, width: 640, height: 400, scale: 1 },
});

const STYLE = "<style>html,body{margin:0;background:#1b2330}" +
  "#m{position:fixed;left:20px;top:100px;width:160px;height:120px;" +
  "background:#f0f}</style>";

const pageUrl = (body) =>
  "data:text/html," + encodeURIComponent(`<!doctype html>${STYLE}${body}`);

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
    // Page.navigate answers once the new document is committed, so the
    // readiness read below is the new page's.
    const open = async (url) => {
      await page("Page.navigate", { url });
      await page("Runtime.evaluate", {
        expression: "new Promise(r => document.readyState === 'complete' ? r() :" +
          " addEventListener('load', () => r(), { once: true }))" +
          ".then(() => document.fonts.ready).then(() => true)",
        awaitPromise: true,
      });
    };
    await body({ cdp: { call: page }, open });
  } finally {
    if (launched) await terminateBrowser(launched.child);
    await rm(profile, { recursive: true, force: true });
  }
}

// The end state a finite Web Animation comes to rest on, painted statically.
const RESTING = pageUrl(
  "<div id=m style='transform:translateX(400px)'></div>");
const sliding = (durationMs) => pageUrl(
  "<div id=m></div><script>document.getElementById('m').animate(" +
  "[{transform:'translateX(0)'},{transform:'translateX(400px)'}]," +
  `{duration:${durationMs},fill:'forwards'})</script>`);

for (const durationMs of [4000, 8000]) {
  test(`a ${durationMs / 1000} s Web Animation is captured at its end state`,
    { timeout: captureCaseTimeout(2), skip: process.platform === "win32" },
    async (context) => {
      await withPage(context, async ({ cdp, open }) => {
        await open(RESTING);
        const resting = await captureStableScreenshot(cdp, OPTIONS);
        assert.ok(resting, "the resting page is still");

        // The control: left running, an animation still moving when the
        // horizon ends kept its decisive frames apart, so the frame is
        // rejected. A horizon slowed past the animation's end by host load
        // judges the resting page instead and proves nothing either way.
        await open(sliding(durationMs));
        const unfrozen = await captureStableScreenshot(cdp, OPTIONS);
        const { result } = await cdp.call("Runtime.evaluate", {
          expression: "document.getAnimations()[0].playState",
          returnByValue: true,
        });
        if (result.value === "running") {
          assert.equal(unfrozen, undefined,
            "an unfrozen Web Animation keeps the horizon from settling");
        } else {
          context.diagnostic(`the horizon outlasted the ${durationMs} ms animation`);
        }

        await open(sliding(durationMs));
        const receipt = await freezeMotion(cdp);
        assert.deepEqual(receipt.animations,
          { finished: 1, paused: 0, left_to_css_freeze: 0, not_time_driven: 0 });
        const frozen = await captureStableScreenshot(cdp, OPTIONS);
        assert.ok(frozen, "the frozen animation is accepted");
        assert.ok(frozen.equals(resting),
          "the accepted bytes are the animation's resting frame");
      });
    });
}

// A page-settle reads layout, so a Web Animation that moves a box keeps the
// document from settling unless the freeze reaches it first, including one the
// page starts after the settle has begun. Every page here goes through the
// capture entry point end to end.
test("a full capture ships moving Web Animations at their resting or first frame",
  { timeout: captureCaseTimeout(7), skip: process.platform === "win32" },
  async (context) => {
    const browser = await installedBrowser();
    if (!browser) return context.skip("no compatible system browser is installed");
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-motion-"));
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    const page = (body) => `<!doctype html><style>html,body{margin:0;` +
      "width:640px;height:400px;background:#1b2330}#m{position:absolute;" +
      `left:20px;top:100px;width:160px;height:120px;background:#f0f}</style>${body}`;
    const slide = (timing, delayMs = 0) => "<div id=m></div><script>" +
      `setTimeout(() => document.getElementById('m').animate(` +
      "[{transform:'translateX(0)'},{transform:'translateX(400px)'}]," +
      `${timing}), ${delayMs})</script>`;
    const pages = {
      resting: "<div id=m style='transform:translateX(400px)'></div>",
      start: "<div id=m></div>",
      "4 s": slide("{duration:4000,fill:'forwards'}"),
      "8 s": slide("{duration:8000,fill:'forwards'}"),
      infinite: slide("{duration:700,iterations:Infinity}"),
      // Started 500 ms after load, once the page-settle is already sampling.
      "late 4 s": slide("{duration:4000,fill:'forwards'}", 500),
      "late infinite": slide("{duration:700,iterations:Infinity}", 500),
    };
    try {
      const captured = {};
      for (const [name, body] of Object.entries(pages)) {
        const file = name.replace(/\W+/g, "-");
        const input = path.join(root, `${file}.html`);
        await writeFile(input, page(body));
        const output = path.join(root, `capture-${file}`);
        await runCapture(script, browser, input, root, output, 640, 400);
        captured[name] = {
          png: await readFile(path.join(output, "browser.png")),
          animations: JSON.parse(await readFile(path.join(output, "capture.json"),
            "utf8")).provenance.motion.animations,
        };
      }
      for (const [name, frame, kind] of [["4 s", "resting", "finished"],
        ["8 s", "resting", "finished"], ["late 4 s", "resting", "finished"],
        ["infinite", "start", "paused"], ["late infinite", "start", "paused"]]) {
        assert.equal(captured[name].animations[kind], 1, name);
        assert.ok(captured[name].png.equals(captured[frame].png),
          `the ${name} animation ships its ${frame} frame`);
      }
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("an infinite Web Animation is held on its first frame",
  { timeout: captureCaseTimeout(2), skip: process.platform === "win32" },
  async (context) => {
    await withPage(context, async ({ cdp, open }) => {
      const pulsing = pageUrl(
        "<div id=m></div><script>document.getElementById('m').animate(" +
        "[{transform:'translateX(0)'},{transform:'translateX(400px)'}]," +
        "{duration:700,iterations:Infinity})</script>");
      await open(pulsing);
      assert.equal(await captureStableScreenshot(cdp, OPTIONS), undefined,
        "an unfrozen infinite animation never settles");

      await open(pageUrl("<div id=m></div>"));
      const start = await captureStableScreenshot(cdp, OPTIONS);
      await open(pulsing);
      // Let it run well into its cycle, so a pause that kept the current time
      // would hold a different frame from the first.
      await cdp.call("Runtime.evaluate", {
        expression: "new Promise((resolve) => { const animation =" +
          " document.getAnimations()[0]; const tick = () =>" +
          " animation.currentTime > 250 ? resolve(true) : requestAnimationFrame(tick);" +
          " tick(); })",
        awaitPromise: true,
      });
      const receipt = await freezeMotion(cdp);
      assert.equal(receipt.animations.paused, 1);
      assert.equal(receipt.animations.finished, 0);
      const frozen = await captureStableScreenshot(cdp, OPTIONS);
      assert.ok(frozen && start && frozen.equals(start),
        "the frozen frame is the animation at time zero");

      // A page that restarts it is settled again by the next pass, which
      // still counts the animation once.
      await cdp.call("Runtime.evaluate",
        { expression: "document.getAnimations()[0].play()" });
      const again = await freezeMotion(cdp);
      assert.equal(again.animations.paused, 1);
      const { result } = await cdp.call("Runtime.evaluate", {
        expression: "document.getAnimations()[0].playState",
        returnByValue: true,
      });
      assert.equal(result.value, "paused");
    });
  });

// A looping video recorded in the page from a canvas whose colour changes on
// every frame, so the clip has no two frames alike, playing when this returns.
const PLAYING_VIDEO = "<video id=v muted playsinline width=160 height=120></video>" +
  "<script>window.videoPlaying = (async () => {" +
  "const canvas = document.createElement('canvas'); canvas.width = 160;" +
  " canvas.height = 120; const paint = canvas.getContext('2d');" +
  "const recorder = new MediaRecorder(canvas.captureStream(30)," +
  " { mimeType: 'video/webm' }); const chunks = [];" +
  "recorder.ondataavailable = (event) => chunks.push(event.data);" +
  "const stopped = new Promise((resolve) => { recorder.onstop = resolve; });" +
  "let frame = 0; const draw = () => { paint.fillStyle =" +
  " `hsl(${frame * 37 % 360},80%,50%)`; paint.fillRect(0, 0, 160, 120);" +
  " frame += 1; if (frame < 24) requestAnimationFrame(draw); else recorder.stop(); };" +
  "recorder.start(); draw(); await stopped;" +
  "const video = document.getElementById('v'); video.loop = true;" +
  "video.src = URL.createObjectURL(new Blob(chunks, { type: 'video/webm' }));" +
  "await video.play();" +
  "await new Promise((resolve) => { const tick = () => video.currentTime > 0.2 ?" +
  " resolve() : requestAnimationFrame(tick); tick(); }); return true; })();" +
  "</script>";

test("a playing video is paused on its first frame",
  { timeout: captureCaseTimeout(1), skip: process.platform === "win32" },
  async (context) => {
    await withPage(context, async ({ cdp, open }) => {
      await open(pageUrl(PLAYING_VIDEO));
      await cdp.call("Runtime.evaluate",
        { expression: "window.videoPlaying", awaitPromise: true });
      const receipt = await freezeMotion(cdp);
      assert.deepEqual(receipt.videos, { paused: 1, rewound: 1, poster: 0 });
      const { result } = await cdp.call("Runtime.evaluate", {
        expression: "(() => { const video = document.getElementById('v');" +
          " return { paused: video.paused, time: video.currentTime }; })()",
        returnByValue: true,
      });
      assert.deepEqual(result.value, { paused: true, time: 0 });
    });
  });

test("a video still showing its poster is paused and left on it",
  { timeout: captureCaseTimeout(1), skip: process.platform === "win32" },
  async (context) => {
    await withPage(context, async ({ cdp, open }) => {
      const poster = "data:image/svg+xml," + encodeURIComponent(
        "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>" +
        "<rect width='4' height='4' fill='#0f0'/></svg>");
      await open(pageUrl(`<video id=v width=160 height=120 poster="${poster}"></video>`));
      const receipt = await freezeMotion(cdp);
      assert.deepEqual(receipt.videos, { paused: 1, rewound: 0, poster: 1 });
    });
  });

// One-pixel GIF frames: a graphic control block with a 100 ms delay, an image
// descriptor and the three-code LZW stream (clear, colour index, end). With no
// looping extension the image plays once and rests on its last frame.
function gif(frameIndices) {
  const frames = frameIndices.flatMap((index) => [
    0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0x00,
    0x02, 0x02, index === 0 ? 0x44 : 0x4c, 0x01, 0x00,
  ]);
  return Buffer.from([
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 1, 0, 1, 0, 0x80, 0, 0,
    0xff, 0, 0, 0, 0, 0xff, ...frames, 0x3b,
  ]);
}

test("a full capture holds an infinite animation still and declares animated images",
  { timeout: captureCaseTimeout(3), skip: process.platform === "win32" },
  async (context) => {
    const browser = await installedBrowser();
    if (!browser) return context.skip("no compatible system browser is installed");
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-motion-"));
    const input = path.join(root, "panel.html");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(path.join(root, "once.gif"), gif([0, 1]));
      await writeFile(path.join(root, "still.gif"), gif([1]));
      // A colour pulse never moves a box, so the page settles and only the
      // frozen clock decides which colour the frame holds.
      await writeFile(input, `<!doctype html><style>
        html,body{margin:0;width:120px;height:80px;background:#246}
        #pulse{position:absolute;left:10px;top:10px;width:40px;height:40px}
        img{position:absolute;top:10px;width:20px;height:20px;image-rendering:pixelated}
      </style><div id="pulse"></div>
      <img src="once.gif" style="left:60px"><img src="still.gif" style="left:90px">
      <script>document.getElementById("pulse").animate(
        [{backgroundColor:"#ff0000"},{backgroundColor:"#00ff00"}],
        {duration:500,iterations:Infinity});</script>`);
      const captures = [];
      for (let run = 0; run < 3; run += 1) {
        const output = path.join(root, `capture-${run}`);
        await runCapture(script, browser, input, root, output, 120, 80);
        captures.push({
          png: await readFile(path.join(output, "browser.png")),
          envelope: JSON.parse(
            await readFile(path.join(output, "capture.json"), "utf8")),
        });
      }
      for (const { png } of captures.slice(1)) {
        assert.ok(png.equals(captures[0].png), "every run ships the same bytes");
      }
      assert.deepEqual(rgbaPixel(captures[0].png, 60, 60), [255, 0, 0, 255],
        "the pulse is held on its first keyframe");
      const motion = captures[0].envelope.provenance.motion;
      assert.equal(motion.schema, "pulp-browser-capture-motion-v1");
      assert.equal(motion.animations.paused, 1);
      assert.equal(motion.images_examined, 2);
      assert.deepEqual(motion.volatile_sources, [{
        kind: "animated-image",
        format: "gif",
        element: "img",
        url: "pulp-capture:///once.gif",
        byte_length: gif([0, 1]).length,
      }]);
      for (const { envelope } of captures.slice(1)) {
        assert.deepEqual(envelope.provenance.motion, motion);
      }
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
