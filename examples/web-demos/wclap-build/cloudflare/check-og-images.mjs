#!/usr/bin/env node
// check-og-images.mjs — fail the deploy if any demo page would unfurl bare.
//
// The unfurl image is the first thing anyone sees when a demo link is pasted into
// Messages, Slack, or a tweet, and it fails SILENTLY: a page with no og:image
// still loads fine, so nothing catches it until someone shares the link and gets
// a blank card. gen-og-images.mjs renders the images from the assembled pages on
// every deploy (so a UI change cannot leave a stale screenshot behind), and this
// script is the guard that the step actually ran and covered every page.
//
// For each demo page under the deploy dir it asserts:
//   1. the HTML carries an og:image (and a twitter card),
//   2. the image it points at EXISTS in the deploy dir,
//   3. the file is a real PNG of plausible size — not a 0-byte or truncated
//      artifact from a screenshot that raced the page's first paint.
//
//   node check-og-images.mjs [--out <deployDir>]

import { readFile, readdir, stat } from "node:fs/promises";
import { inflateSync } from "node:zlib";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const argOut = process.argv.indexOf("--out");
const OUT = resolve(HERE, argOut >= 0 ? process.argv[argOut + 1] : "./public");

// A screenshot of a rendered plugin panel is tens of KB. Anything smaller is a
// blank frame or a truncated write, which is worse than no image: it unfurls as
// an empty box.
const MIN_PNG_BYTES = 4096;
const PNG_MAGIC = Buffer.from([0x89, 0x50, 0x4e, 0x47]);

// Is the PLUGIN actually in this picture?
//
// Every other check here passes on a photograph of the LOADING SCREEN: it exists, it is a real
// PNG, it is the right shape, and it is 54 KB of flat dark panel with "Loading editor…" in the
// middle. That card shipped, and nobody saw it until it was pasted into a chat.
//
// So decode the pixels and measure the band where the controls live. A card of a running
// plugin is BUSY there — knobs, values, a scope. A card of an empty or still-loading panel is
// the background color and almost nothing else.
//
// MEASURED, which is why the threshold is where it is: the live loading-screen card scored
// 0.60% bright pixels; every real plugin card scored 4.2–7.0%. 2% sits in the gap with room on
// both sides. This is a "did anything render" test, NOT a pixel-comparison — a redesign must
// not fail it.
const MIN_BRIGHT_FRACTION = 0.02;

function unfilterPng(buf) {
  let pos = 8, w = 0, h = 0, bitDepth = 0, colorType = 0, interlace = 0;
  const idat = [];
  while (pos + 8 <= buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString("ascii", pos + 4, pos + 8);
    if (type === "IHDR") {
      w = buf.readUInt32BE(pos + 8); h = buf.readUInt32BE(pos + 12);
      bitDepth = buf[pos + 16]; colorType = buf[pos + 17]; interlace = buf[pos + 20];
    } else if (type === "IDAT") idat.push(buf.subarray(pos + 8, pos + 8 + len));
    else if (type === "IEND") break;
    pos += 12 + len;
  }
  // An encoding we do not decode must never fail the build — return null and skip the check.
  if (bitDepth !== 8 || interlace !== 0) return null;
  const ch = colorType === 6 ? 4 : colorType === 2 ? 3 : 0;
  if (!ch) return null;

  let raw;
  try { raw = inflateSync(Buffer.concat(idat)); } catch { return null; }
  const stride = w * ch;
  if (raw.length < (stride + 1) * h) return null;

  const px = Buffer.alloc(stride * h);
  let p = 0;
  for (let y = 0; y < h; y++) {
    const f = raw[p++];
    const cur = px.subarray(y * stride, (y + 1) * stride);
    raw.copy(cur, 0, p, p + stride); p += stride;
    const prev = y ? px.subarray((y - 1) * stride, y * stride) : Buffer.alloc(stride);
    for (let x = 0; x < stride; x++) {
      const a = x >= ch ? cur[x - ch] : 0, b = prev[x], c = x >= ch ? prev[x - ch] : 0;
      let v = cur[x];
      if (f === 1) v += a;
      else if (f === 2) v += b;
      else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) {
        const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
        v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      }
      cur[x] = v & 0xff;
    }
  }
  return { w, h, ch, px };
}

async function pngBrightFraction(file) {
  const { readFile } = await import("node:fs/promises");
  const img = unfilterPng(await readFile(file));
  if (!img) return null;
  const { w, h, ch, px } = img;
  const y0 = Math.floor(h * 0.35), y1 = Math.floor(h * 0.95);   // the control band
  const lum = [];
  for (let y = y0; y < y1; y += 2)
    for (let x = 0; x < w; x += 2) {
      const i = y * w * ch + x * ch;
      lum.push(px[i] * 0.30 + px[i + 1] * 0.59 + px[i + 2] * 0.11);
    }
  if (!lum.length) return null;
  const mean = lum.reduce((a, b) => a + b, 0) / lum.length;
  return lum.filter((v) => v > mean + 12).length / lum.length;   // pixels above the panel
} Buffer.from([0x89, 0x50, 0x4e, 0x47]);

// `/` is the single-plugin isolation PROOF page, and its HTML is the in-repo dev
// browser-host page (tools/browser-host/index.html) copied verbatim — it is a
// harness, not a demo we hand people, and baking deploy-specific OG tags into a
// local dev tool would leak deploy concerns into it. Every page a visitor is
// actually pointed at must have one. If `/` is ever promoted to a shared demo,
// delete this entry rather than working around it.
const EXEMPT = new Set(["/"]);

let failed = 0;
const fail = (page, why) => { console.error(`  FAIL ${page} — ${why}`); failed++; };
const ok = (page, why) => console.log(`  ok   ${page} — ${why}`);

async function* demoPages(dir, rel = "") {
  for (const e of await readdir(dir, { withFileTypes: true })) {
    if (e.isDirectory()) {
      if (e.name === "vendor-player" || e.name === "node_modules") continue;
      yield* demoPages(join(dir, e.name), `${rel}/${e.name}`);
    } else if (e.name === "index.html") {
      yield { file: join(dir, e.name), url: `${rel}/` || "/" };
    }
  }
}

// The root index.html comes from assemble.mjs, not assemble-gallery.mjs, so the
// deploy dir's existence is the precondition — not any one page.
if (!existsSync(OUT)) {
  console.error(`check-og-images: no assembled site at ${OUT} — run assemble-gallery.mjs first.`);
  process.exit(2);
}

console.log(`check-og-images: auditing ${OUT}`);
let audited = 0;
for await (const { file, url } of demoPages(OUT)) {
  if (EXEMPT.has(url)) { console.log(`  --   ${url} — exempt (dev harness, not a shared demo)`); continue; }

  const html = await readFile(file, "utf8");
  audited++;

  const og = html.match(/<meta\s+property="og:image"\s+content="([^"]+)"/i);
  if (!og) { fail(url, "no og:image — this page unfurls bare"); continue; }
  if (!/twitter:card/i.test(html)) { fail(url, "og:image present but no twitter:card"); continue; }

  // The tag is an absolute site URL; resolve it back to a file in the deploy dir.
  const path = og[1].replace(/^https?:\/\/[^/]+/, "");
  const onDisk = join(OUT, path);
  if (!existsSync(onDisk)) { fail(url, `og:image points at ${path}, which is not in the deploy dir`); continue; }

  const st = await stat(onDisk);
  if (st.size < MIN_PNG_BYTES) { fail(url, `${path} is only ${st.size}B — a blank or truncated screenshot`); continue; }

  // Read the IHDR: magic (8B) + length/type (8B) + width/height (8B).
  const head = Buffer.alloc(24);
  const fh = await (await import("node:fs/promises")).open(onDisk, "r");
  await fh.read(head, 0, 24, 0);
  await fh.close();
  if (!head.subarray(0, 4).equals(PNG_MAGIC)) { fail(url, `${path} is not a PNG`); continue; }

  // Aspect matters as much as existence: unfurlers render anything far from ~1.91:1
  // as a small square thumbnail rather than a large card, so a portrait screenshot
  // passes every other check here and STILL looks broken when the link is shared.
  const w = head.readUInt32BE(16), h = head.readUInt32BE(20);
  const ratio = w / h;
  if (ratio < 1.7 || ratio > 2.1) {
    fail(url, `${path} is ${w}x${h} (aspect ${ratio.toFixed(2)}) — unfurlers want ~1.91:1; ` +
              "it will render as a small thumbnail, not a card");
    continue;
  }

  // LOOK AT THE PIXELS. Every check above passes on a picture of the LOADING SCREEN: it
  // exists, it is a real PNG, it is the right shape, and it is 54 KB of mostly-flat dark
  // panel with a "Loading editor…" label in the middle. That card shipped, and nobody saw it
  // until it was pasted into a chat.
  //
  // A card of the running plugin has knobs, values, a scope — the lower half is BUSY. A card
  // of an empty/loading panel is nearly uniform there. So decode and measure it: sample the
  // bottom two-thirds (where the plugin lives, below the title block) and require real
  // variation. Deliberately not an exact-pixel comparison — that would break on every legit
  // redesign — just "is there anything there at all".
  const bright = await pngBrightFraction(onDisk);
  if (bright !== null && bright < MIN_BRIGHT_FRACTION) {
    fail(url, `${path} is EMPTY where the plugin should be (${(bright * 100).toFixed(2)}% of ` +
              `the control band is above the panel background, need ${MIN_BRIGHT_FRACTION * 100}%) ` +
              "— this is a picture of a blank or still-loading panel, not the plugin");
    continue;
  }

  ok(url, `${path} — ${w}x${h}, ${Math.round(st.size / 1024)} KB` +
          (bright === null ? "" : `, ${(bright * 100).toFixed(1)}% plugin`));
}

console.log(`\nchecked ${audited} page(s)`);
if (failed) {
  console.error(`check-og-images: ${failed} page(s) would unfurl bare or broken.`);
  console.error("Run: node gen-og-images.mjs && node assemble-gallery.mjs");
  process.exit(1);
}
console.log("check-og-images: every demo page has a real OG preview image.");
