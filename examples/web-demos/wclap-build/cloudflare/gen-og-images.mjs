#!/usr/bin/env node
// gen-og-images.mjs — render a per-page Open Graph preview image for every
// WebCLAP demo page, matching the WAM sites' convention exactly.
//
// WHY: assemble-gallery.mjs bakes the og:image / twitter:card=summary_large_image
// block into a page ONLY when a sibling og.png exists. This script produces those
// PNGs. It drives each ASSEMBLED page through the real worklet-resident CLAP host
// in headless Chrome and screenshots the same element the WAM generator does —
// the started editor panel (`#panel`) for a plugin page, the card grid (`.wrap`)
// for a gallery — at deviceScaleFactor 2. Run it AFTER assemble-gallery.mjs, then
// re-run assemble-gallery.mjs so the freshly-present images turn into og:image
// tags. The images are deploy artifacts (public/ is gitignored), same as the wasm.
//
// WebCLAP needs cross-origin isolation (COOP/COEP) to instantiate its threaded
// shared-memory module, so the page is served through serve-headers.mjs (which
// applies the deploy dir's _headers) — NOT a plain static server.
//
//   node gen-og-images.mjs [--out <deployDir>] [--browser <chrome>] [--port <n>]
//     --out   the assembled deploy dir to screenshot (default: ./public)
//
// Chrome is located via --browser, PLAYWRIGHT_CHROMIUM_PATH, CHROME_PATH, or a
// short list of common install paths (mirrors the SDK web-plugins lane). Requires
// playwright-core on the module path (CI: `npm install --no-save playwright-core`).
import { spawn } from "node:child_process";
import { readFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import { chromium } from "playwright-core";

const HERE = dirname(fileURLToPath(import.meta.url));
function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const OUT = resolve(HERE, arg("--out", "./public"));
const PORT = Number(arg("--port", 8793));
const BASE = `http://localhost:${PORT}`;
if (!existsSync(join(OUT, "index.html"))) {
  console.error(`error: ${OUT}/index.html not found — assemble the site first (assemble-gallery.mjs).`);
  process.exit(2);
}

const CANDIDATES = [
  arg("--browser", null),
  process.env.PLAYWRIGHT_CHROMIUM_PATH,
  process.env.CHROME_PATH,
  "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Chromium.app/Contents/MacOS/Chromium",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium-browser",
  "/usr/bin/chromium",
].filter(Boolean);
const CHROME = CANDIDATES.find((p) => existsSync(p));
if (!CHROME) {
  console.error("error: no Chrome/Chromium binary found (set CHROME_PATH or pass --browser).");
  process.exit(2);
}

// The two galleries this site hosts. Each gallery's index.html embeds the same
// `const plugins = [...]` array assemble-gallery.mjs wrote, so we read the demo
// dirs straight from it (single source of truth, mirrors WAM's gen-og-images).
const SECTIONS = ["example-plugins", "classic-effects"];

// super-convolver is not a plugins-array gallery — it is ONE plugin presented in
// two ABIs, so its dirs are fixed rather than parsed. Same convention as the
// sections above: each demo page is shot STARTED (`#panel` only exists once the
// player has mounted, so the image is the plugin actually running, not a
// click-to-start overlay), and the index is shot as its card grid.
const SUPER_CONVOLVER = { slug: "super-convolver", dirs: ["wam", "wclap"] };

// The two standalone pages assemble.mjs / assemble-player.mjs write. They are not
// galleries and have no plugins array, but they are shareable URLs like any other
// — a bare unfurl is a bare unfurl — so they get previews too. `/player/` mounts
// the shared player, so it is shot started; `/` is the isolation proof card.
const STANDALONE = [
  { url: "/player/", selector: "#panel", out: ["player", "og.png"], label: "player", start: true },
  { url: "/", selector: ".card", out: ["og.png"], label: "root (isolation proof)", start: false },
];

// Serve the deploy dir under its own _headers (COOP/COEP/CORP + MIME) so the
// threaded-wasm module instantiates exactly as it will on Cloudflare.
const server = spawn(process.execPath,
  [join(HERE, "serve-headers.mjs"), "--dir", OUT, "--port", String(PORT)],
  { stdio: ["ignore", "ignore", "inherit"] });
await sleep(500);

const browser = await chromium.launch({
  executablePath: CHROME,
  headless: true,
  // --mute-audio: the demo builds a real AudioContext; keep it silent.
  // --autoplay-policy: __start() runs without a user gesture, so let the context
  //   actually resume (otherwise start() awaits a resume that never lands).
  args: ["--mute-audio", "--autoplay-policy=no-user-gesture-required"],
});

// The card size every unfurler is tuned for (1.91:1). Shot at deviceScaleFactor 2,
// so the PNG lands at 2400x1260.
const OG_W = 1200;
const OG_H = 630;

let failed = 0;
async function attempt(pageUrl, selector, outPath, label, start) {
  const context = await browser.newContext({
    viewport: { width: 960, height: 1200 },
    deviceScaleFactor: 2,   // crisp @2x preview cards, matching the WAM sites
  });
  const page = await context.newPage();
  page.on("pageerror", (e) => console.log(`  [pageerror ${label}]`, e.message));
  try {
    await page.goto(pageUrl, { waitUntil: "load", timeout: 30000 });
    if (start) {
      // Press the shared player's test seam and wait for the started editor.
      await page.waitForFunction(() => typeof window.__start === "function", null, { timeout: 20000 });
      await page.evaluate(() => { window.__start(); });
      await page.waitForFunction(() => window.__demo && window.__demo.started === true, null, { timeout: 25000 });
      await sleep(800);   // let the scope/meter/widgets settle into first paint
    }
    const el = await page.$(selector);
    if (!el) throw new Error(`selector ${selector} not found`);

    // Compose an OG-SHAPED card rather than shooting the element raw. Unfurlers
    // (iMessage, Slack, Twitter) want ~1.91:1 and render anything else as a small
    // square thumbnail instead of the large card — a portrait screenshot of a tall
    // plugin panel technically "has an og:image" and still looks broken. So fit the
    // element into a 1200x630 frame on the page's own background and shoot the
    // frame. Scale is capped at 1 so a small panel is never upscaled to mush.
    const fitted = await page.evaluate(({ sel, W, H, pad }) => {
      const node = document.querySelector(sel);
      const r = node.getBoundingClientRect();
      const scale = Math.min((W - pad * 2) / r.width, (H - pad * 2) / r.height, 1);
      const bg = getComputedStyle(document.body).backgroundColor || "#0d1117";
      const frame = document.createElement("div");
      frame.id = "__og_frame";
      Object.assign(frame.style, {
        position: "fixed", left: "0", top: "0", width: `${W}px`, height: `${H}px`,
        background: bg, display: "flex", alignItems: "center", justifyContent: "center",
        zIndex: "2147483647", overflow: "hidden", margin: "0",
      });
      // Move the live node into the frame — cloning would drop the canvas bitmap
      // (a <canvas> clone is blank), and the whole point is the running plugin.
      const holder = document.createElement("div");
      holder.style.transform = `scale(${scale})`;
      holder.style.transformOrigin = "center center";
      holder.style.flex = "0 0 auto";
      frame.appendChild(holder);
      document.body.appendChild(frame);
      holder.appendChild(node);
      return { scale, w: Math.round(r.width), h: Math.round(r.height) };
    }, { sel: selector, W: OG_W, H: OG_H, pad: 24 });

    await page.setViewportSize({ width: OG_W, height: OG_H });
    await sleep(250);   // one frame for the reflow + any canvas repaint
    await page.screenshot({ path: outPath, clip: { x: 0, y: 0, width: OG_W, height: OG_H } });
    console.log(`  ok   ${label} -> ${outPath} ` +
                `(${OG_W * 2}x${OG_H * 2} card; panel ${fitted.w}x${fitted.h} @ ${fitted.scale.toFixed(2)}x)`);
  } finally {
    await context.close();
  }
}

// One retry absorbs transient headless worklet-load timing without weakening the
// "every page must produce an image" guarantee.
async function shoot(pageUrl, selector, outPath, label, start) {
  for (let tryNo = 1; tryNo <= 2; tryNo++) {
    try { await attempt(pageUrl, selector, outPath, label, start); return; }
    catch (e) {
      if (tryNo === 2) { failed++; console.log(`  FAIL ${label}: ${e && e.message ? e.message : e}`); }
      else { console.log(`  retry ${label} (${e && e.message ? e.message : e})`); await sleep(600); }
    }
  }
}

try {
  for (const slug of SECTIONS) {
    const galleryFile = join(OUT, slug, "index.html");
    if (!existsSync(galleryFile)) { console.log(`  skip ${slug} (no gallery)`); continue; }
    const html = await readFile(galleryFile, "utf8");
    const arrText = (html.match(/const plugins = (\[[\s\S]*?\]);/) || [])[1];
    if (!arrText) { console.log(`  WARN ${slug}: no plugins array`); failed++; continue; }
    // eslint-disable-next-line no-eval — trusted, our own assembled gallery file.
    const plugins = eval(arrText);
    // Per-plugin card: the started editor panel.
    for (const p of plugins) {
      await shoot(`${BASE}/${slug}/${p.dir}/`, "#panel",
        join(OUT, slug, p.dir, "og.png"), `${slug}/${p.dir}`, true);
    }
    // Gallery card: the card grid itself.
    await shoot(`${BASE}/${slug}/`, ".wrap", join(OUT, slug, "og.png"), `${slug} gallery`, false);
  }

  if (existsSync(join(OUT, SUPER_CONVOLVER.slug, "index.html"))) {
    const { slug, dirs } = SUPER_CONVOLVER;
    for (const dir of dirs) {
      await shoot(`${BASE}/${slug}/${dir}/`, "#panel",
                  join(OUT, slug, dir, "og.png"), `${slug}/${dir}`, true);
    }
    await shoot(`${BASE}/${slug}/`, ".wrap", join(OUT, slug, "og.png"), `${slug} gallery`, false);
  } else {
    console.log(`  skip ${SUPER_CONVOLVER.slug} (not assembled)`);
  }

  for (const s of STANDALONE) {
    const page = join(OUT, ...s.out.slice(0, -1), "index.html");
    if (!existsSync(page)) { console.log(`  skip ${s.label} (not assembled)`); continue; }
    await shoot(`${BASE}${s.url}`, s.selector, join(OUT, ...s.out), s.label, s.start);
  }
} finally {
  await browser.close();
  server.kill("SIGTERM");
}

if (failed) { console.error(`\ngen-og-images: ${failed} page(s) failed to render.`); process.exit(1); }
console.log(`\ngen-og-images: rendered OG preview images into ${OUT}/`);
