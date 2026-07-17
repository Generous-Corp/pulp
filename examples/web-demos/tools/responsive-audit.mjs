#!/usr/bin/env node
// responsive-audit.mjs — screenshot a Pulp web-demo page at desktop / tablet / phone
// widths so an agent (or a human) can SEE overlapping text, clipped controls, and the
// mobile layout before shipping — instead of finding out from user feedback.
//
// This is the browser pass the pulp-web-demo / web-plugins skills call for: a
// full-canvas custom-UI editor MUST be looked at across sizes, because its layout is
// drawn (Skia), not flowed by CSS, so a breakpoint bug never shows up in a unit test.
//
// Usage:
//   node responsive-audit.mjs --url https://<preview>/<plugin>/ [--out /tmp/audit] \
//        [--canvas '#pulp-ui-canvas'] [--start '#ov-start'] [--settle 6000]
//
// It writes, per viewport, a full-page PNG and a tight editor-canvas PNG, and prints
// the editor's CSS size + the computed touch-action. THEN OPEN THE PNGs AND LOOK:
// the script proves the page renders; only your eyes prove it looks right.
//
// Requires playwright-core + a local Chrome. Deterministic-ish (fixed viewports); the
// live field animation means pixels differ run to run — compare layout, not exact bytes.

import { chromium } from "playwright-core";
import { mkdir } from "node:fs/promises";
import { join } from "node:path";

const arg = (k, d) => {
  const i = process.argv.indexOf(k);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : d;
};
const URL_ = arg("--url", null);
if (!URL_) { console.error("responsive-audit: --url is required"); process.exit(2); }
const OUT = arg("--out", "/tmp/pulp-responsive-audit");
const CANVAS = arg("--canvas", "#pulp-ui-canvas");
const START = arg("--start", "#ov-start");
const SETTLE = parseInt(arg("--settle", "6000"), 10);
const CHROME = arg("--chrome", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");

// The three shapes that matter: a wide desktop, a portrait tablet, and a phone. The
// phone is the one that historically broke (a tall skinny canvas has a large UI scale
// and a small width — the exact case that collides a centered header with the wordmark).
const VIEWS = [
  { name: "desktop", width: 1440, height: 900, dsf: 2, mobile: false },
  { name: "tablet",  width: 834,  height: 1112, dsf: 2, mobile: false },
  { name: "phone",   width: 390,  height: 844, dsf: 3, mobile: true },
];

await mkdir(OUT, { recursive: true });
const b = await chromium.launch({
  executablePath: CHROME, headless: true,
  args: ["--use-gl=angle", "--use-angle=metal", "--enable-unsafe-swiftshader",
         "--autoplay-policy=no-user-gesture-required"],
});

let failed = 0;
for (const v of VIEWS) {
  const ctx = await b.newContext({
    viewport: { width: v.width, height: v.height },
    deviceScaleFactor: v.dsf, isMobile: v.mobile, hasTouch: v.mobile,
  });
  const pg = await ctx.newPage();
  try {
    await pg.goto(URL_, { waitUntil: "load", timeout: 45000 });
    await pg.waitForSelector(START, { timeout: 20000 }).catch(() => {});
    await pg.click(START).catch(() => {});
    await pg.waitForSelector(CANVAS, { timeout: 20000 }).catch(() => {});
    await pg.evaluate((ms) => new Promise((r) => setTimeout(r, ms)), SETTLE);
    await pg.screenshot({ path: join(OUT, `${v.name}-page.png`), fullPage: false });
    const el = await pg.$(CANVAS);
    if (el) {
      const box = await el.boundingBox();
      await pg.screenshot({ path: join(OUT, `${v.name}-editor.png`),
                            clip: { x: box.x, y: box.y, width: box.width, height: box.height } });
      const ta = await pg.evaluate((sel) => getComputedStyle(document.querySelector(sel)).touchAction, CANVAS);
      console.log(`  ${v.name.padEnd(7)} editor ${Math.round(box.width)}x${Math.round(box.height)} css  touch-action=${ta}  -> ${v.name}-editor.png`);
    } else {
      console.log(`  ${v.name.padEnd(7)} NO CANVAS (${CANVAS}) — page-only shot`);
      failed++;
    }
  } catch (e) {
    console.log(`  ${v.name.padEnd(7)} FAIL: ${e && e.message ? e.message : e}`);
    failed++;
  } finally {
    await ctx.close();
  }
}
await b.close();
console.log(`\nWrote screenshots to ${OUT}. NOW OPEN THEM and check each size for:` +
  `\n  • overlapping / clipped text (header wordmark vs tabs vs engine chip; slider label vs value)` +
  `\n  • the info ("i") overlay fitting its card` +
  `\n  • controls filling the width evenly (no dead trailing gap)` +
  `\n  • on phone: does a two-line readout stay two lines? does the file picker open? does the page scroll?`);
process.exit(failed ? 1 : 0);
