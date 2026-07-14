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
import { readFile, readdir, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";
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

// /super-convolver-gpu/ is the WebGPU-compute engine page. It is WebCLAP-ONLY (a WAM
// module lives inside an AudioWorklet, which can neither reach navigator.gpu nor spawn
// the worker that would), so unlike the pair above it has NO per-ABI sub-dirs — the
// plugin is on the index itself, and the page IS the shot.
//
// It is listed here because assemble-gallery.mjs emits it CONDITIONALLY (only when the
// GPU DSP build tree is present). A page that appears without an entry here would ship
// with a baked og:image pointing at a PNG nobody generated — and Cloudflare serves a
// missing asset as its 404 page with a 200 status, so it fails as a BLANK UNFURL rather
// than as a broken link. check-og-images.mjs is the backstop that turns that into a
// build failure; this is what keeps it from happening.
const SUPER_CONVOLVER_GPU = { slug: "super-convolver-gpu" };

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

// The shape every unfurler is tuned for (1200x630). We do not render AT that size —
// we crop a window of this RATIO out of the page at native resolution, so the card is
// a tight, unscaled view of the plugin rather than a shrunken one.
const OG_RATIO = 1200 / 630;   // 1.905

let failed = 0;
let skipped = 0;
async function attempt(pageUrl, selector, outPath, label, start) {
  const context = await browser.newContext({
    viewport: { width: 960, height: 1200 },
    // 3x: a ~660px-wide crop lands near 2000px, comfortably above the 1200px
    // unfurlers want, with no upscaling. It also makes Pulp's canvas render its
    // backing store at 3x (it sizes from devicePixelRatio), so the knobs are crisp —
    // CSS-scaling the canvas instead would just resample a 1x bitmap.
    deviceScaleFactor: 3,
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

      // "started" is the SHELL's state, not the PLUGIN's — and neither is "the canvas
      // exists". A wasm editor's canvas element is created IMMEDIATELY and only then does the
      // module get fetched and instantiated, so waiting for the element waits for nothing:
      // the card came out reading "Loading editor…", which is what the player paints INTO
      // that canvas's slot while the plugin is still coming up. It shipped that way, twice —
      // first as a black rectangle, then as a loading message.
      //
      // The player already publishes the correct signal. It shows `.pw-customui-loading`
      // while the editor is mounting and REMOVES it when the editor has settled. Wait for
      // that to be gone, not for a box to exist.
      await page.waitForFunction(() => {
        if (document.querySelector(".pw-customui-loading")) return false;   // still mounting
        const c = document.querySelector("#pulp-ui-canvas, #custom-ui canvas");
        if (c) return c.width > 0 && c.height > 0;
        return !!document.querySelector("#params .pw-cell");                // no custom UI
      }, null, { timeout: 45000 }).catch(() => {});
      await sleep(2200);  // let the editor, scope, meter and live values settle into paint

      // AND REFUSE TO SHIP A CARD OF THE LOADING SCREEN. Every wait above is a claim about
      // when the plugin is ready; this is the check that the claim held. Cheap, and it is the
      // only thing standing between a wrong wait and a wrong picture on every share link.
      const stillLoading = await page.evaluate(() =>
        !!document.querySelector(".pw-customui-loading") ||
        /Loading editor/i.test(document.body.innerText));
      if (stillLoading) throw new Error("the editor had not finished mounting — refusing to shoot the loading screen");
    }
    const el = await page.$(selector);
    if (!el) throw new Error(`selector ${selector} not found`);

    // TIGHT CROP, not fit-and-letterbox. Plugin panels are PORTRAIT (a 640x682 panel
    // is typical) and an OG card is LANDSCAPE, so scaling the whole panel to fit
    // strands ~600px of dead margin either side and the plugin reads tiny — the
    // opposite of what a preview card is for. Instead crop a card-shaped window from
    // the panel itself: full width, anchored at the TOP, where the plugin's identity
    // lives (title, knobs, live values). Nothing is scaled, so the canvas bitmap is
    // never resampled — CSS-scaling a <canvas> just blurs it.
    await page.evaluate((sel) => {
      const node = document.querySelector(sel);
      node.scrollIntoView({ block: "start", inline: "nearest" });
    }, selector);
    await sleep(150);

    const box = await el.boundingBox();
    if (!box) throw new Error("element has no box");

    // A hair of padding so the panel's rounded border is not shaved off. The crop
    // height is ALWAYS width/ratio — never clamped to the panel — so the card keeps an
    // exact 1.91:1 even for a short panel (the remainder is the page's own background,
    // which is preferable to a ratio the unfurler will thumbnail).
    const PAD = 10;
    const cw = box.width + PAD * 2;
    const ch = cw / OG_RATIO;
    const x = Math.max(0, box.x - PAD);
    let y = Math.max(0, box.y - PAD);

    // Keep the window inside the page, growing the document if the panel sits near the
    // bottom — clipping past the end of the page is an error in Playwright.
    await page.evaluate((need) => {
      document.body.style.minHeight = `${need}px`;
    }, Math.ceil(y + ch + 40));
    await sleep(80);

    await page.screenshot({ path: outPath, clip: { x, y, width: cw, height: ch } });
    const dsf = await page.evaluate(() => window.devicePixelRatio);
    console.log(`  ok   ${label} -> ${outPath} (${Math.round(cw * dsf)}x${Math.round(ch * dsf)}` +
                `, tight crop of a ${Math.round(box.width)}x${Math.round(box.height)} panel)`);
  } finally {
    await context.close();
  }
}

// One retry absorbs transient headless worklet-load timing without weakening the
// "every page must produce an image" guarantee.
// ── Don't re-shoot a page that has not changed ──────────────────────────────────
//
// A shot is EXPENSIVE: launch a context, load the page, instantiate a multi-megabyte wasm
// plugin, wait for it to settle, screenshot. Thirty of those on every deploy — including the
// twenty-odd pages nobody touched — is minutes of CI, and battery, spent redrawing pictures
// that are already correct.
//
// So each og.png is keyed to the things that can actually change it: the page's own HTML,
// and every asset that page loads from this site (its wasm module, the player, pulp-ui.js).
// Same inputs -> same picture -> skip. The key is stored NEXT TO the image, so a missing or
// hand-deleted png always re-shoots, and a changed plugin always re-shoots.
//
// Deliberately keyed on CONTENT, not mtimes: a rebuild touches every file, and an mtime
// cache would then be a cache that never hits.
const KEY_SUFFIX = ".key";

async function shotKey(pageDir) {
  const h = createHash("sha256");
  const walk = async (dir) => {
    for (const e of (await readdir(dir, { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
      if (e.name.endsWith(".png") || e.name.endsWith(KEY_SUFFIX)) continue;   // the output, not an input
      const full = join(dir, e.name);
      if (e.isDirectory()) { await walk(full); continue; }
      h.update(e.name);
      h.update(await readFile(full));
    }
  };
  try { await walk(pageDir); } catch { return null; }
  // The shared player is loaded by every page and is NOT in the page's own directory.
  try { await walk(join(OUT, "vendor-player")); } catch { /* the standalone pages have none */ }
  return h.digest("hex");
}

async function shoot(pageUrl, selector, outPath, label, start) {
  const pageDir = dirname(outPath);
  const keyPath = outPath + KEY_SUFFIX;
  const key = await shotKey(pageDir);
  if (key && existsSync(outPath)) {
    const prev = await readFile(keyPath, "utf8").catch(() => null);
    if (prev === key) { skipped++; console.log(`  skip ${label} (unchanged)`); return; }
  }

  for (let tryNo = 1; tryNo <= 2; tryNo++) {
    try {
      await attempt(pageUrl, selector, outPath, label, start);
      if (key) await writeFile(keyPath, key);
      return;
    }
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

  // The GPU page: shot STARTED, like every other demo page, so the preview is the
  // plugin actually running its WebGPU engine rather than a click-to-start overlay.
  if (existsSync(join(OUT, SUPER_CONVOLVER_GPU.slug, "index.html"))) {
    const { slug } = SUPER_CONVOLVER_GPU;
    await shoot(`${BASE}/${slug}/`, "#panel", join(OUT, slug, "og.png"), slug, true);
  } else {
    console.log(`  skip ${SUPER_CONVOLVER_GPU.slug} (not assembled)`);
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
