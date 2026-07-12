#!/usr/bin/env node
// check-unfurl.mjs — prove the LIVE site unfurls, by behaving like a crawler.
//
// check-og-images.mjs audits the files on disk. It cannot catch the failure that
// actually shipped: the deploy landed on a PREVIEW alias while og:image is an
// absolute production URL, so the tags pointed at a host that had no images. The
// files were perfect; every shared link still unfurled blank.
//
// It is invisible to a status-code check, too — Cloudflare serves a missing asset
// as its 404 HTML page with a **200**. So this script asserts on BYTES: fetch the
// page, parse its og:image, fetch that URL, and require a real PNG of card shape.
//
//   node check-unfurl.mjs --base https://pulp-wclap-demos.pages.dev [--page /x/ ...]

const arg = (f, d) => {
  const i = process.argv.indexOf(f);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : d;
};
const BASE = arg("--base", "https://pulp-wclap-demos.pages.dev").replace(/\/$/, "");
const explicit = process.argv.reduce((acc, a, i) =>
  a === "--page" ? [...acc, process.argv[i + 1]] : acc, []);

// Every page a visitor is handed a link to. `/` is the dev harness (see
// check-og-images.mjs) and is deliberately absent.
const PAGES = explicit.length ? explicit : [
  "/super-convolver/", "/super-convolver/wam/", "/super-convolver/wclap/",
  "/classic-effects/", "/example-plugins/", "/player/",
];

// A crawler UA: some hosts vary their response on it, so check what the unfurler sees.
const UA = { "User-Agent": "facebookexternalhit/1.1 (+http://www.facebook.com/externalhit_uatext.php)" };
const PNG = Buffer.from([0x89, 0x50, 0x4e, 0x47]);

let failed = 0;
const fail = (p, why) => { console.error(`  FAIL ${p} — ${why}`); failed++; };

for (const p of PAGES) {
  let html;
  try {
    const r = await fetch(BASE + p, { headers: UA });
    if (!r.ok) { fail(p, `page returned ${r.status}`); continue; }
    html = await r.text();
  } catch (e) { fail(p, `page unreachable: ${e.message}`); continue; }

  const m = html.match(/<meta\s+property="og:image"\s+content="([^"]+)"/i);
  if (!m) { fail(p, "no og:image — unfurls bare"); continue; }
  if (!/twitter:card/i.test(html)) { fail(p, "no twitter:card"); continue; }

  const img = m[1];
  let buf, ct;
  try {
    const r = await fetch(img, { headers: UA });
    if (!r.ok) { fail(p, `og:image ${img} returned ${r.status}`); continue; }
    ct = r.headers.get("content-type") || "";
    buf = Buffer.from(await r.arrayBuffer());
  } catch (e) { fail(p, `og:image unreachable: ${e.message}`); continue; }

  // The load-bearing assertion. A 404 page arrives here as 200 + text/html.
  if (!buf.subarray(0, 4).equals(PNG)) {
    fail(p, `og:image is not a PNG (content-type=${ct}, ${buf.length}B) — ` +
            `${img} is almost certainly missing on this host`);
    continue;
  }

  const w = buf.readUInt32BE(16), h = buf.readUInt32BE(20);
  const ratio = w / h;
  if (ratio < 1.7 || ratio > 2.1) {
    fail(p, `og:image is ${w}x${h} (aspect ${ratio.toFixed(2)}) — unfurlers want ~1.91:1; ` +
            "renders as a thumbnail, not a card");
    continue;
  }
  console.log(`  ok   ${p.padEnd(28)} ${w}x${h}  ${Math.round(buf.length / 1024)} KB`);
}

if (failed) {
  console.error(`\ncheck-unfurl: ${failed} page(s) do not unfurl on ${BASE}.`);
  console.error("If the files look right locally, check the DEPLOY TARGET: " +
                "`wrangler pages deploy` publishes a preview alias unless you pass --branch main.");
  process.exit(1);
}
console.log(`\ncheck-unfurl: every page unfurls with a real card on ${BASE}.`);
