// Assemble the WCLAP site's ROOT LANDING PAGE into the Cloudflare Pages deploy dir.
//
// The deploy root (`/`) is the front door of the WCLAP demo site. It should point
// visitors at the two WCLAP demo galleries first, then at the equivalent WAM
// galleries, and link the Pulp web-plugin docs — NOT embed a single live plugin.
//
// So this script (run LAST, after assemble.mjs / assemble-player.mjs /
// assemble-gallery.mjs) does two things:
//
//   1. Relocates the single-plugin isolation proof from `/` to
//      `/isolation-proof.html`. The proof's sibling assets (main.js,
//      wclap-host.mjs, wclap-wasi.mjs, PulpGain.wasm) stay at the deploy ROOT and
//      are referenced via `./` against the document base URL (still `/`), so the
//      relocated page keeps working with zero path rewrites. It is regenerated
//      from the committed browser-host/index.html (favicon injected) so it is a
//      clean proof, not the banner-injected variant.
//
//   2. Writes a clean landing page to `/index.html` that promotes the two WCLAP
//      galleries, the two WAM galleries (on generouscorp.com), the doc pages, and
//      keeps the isolation proof + shared-player demo reachable via small links.
//
// Usage:  node assemble-landing.mjs [--out <dir>]
// Default: --out ./public
import { readFile, writeFile, rename } from "node:fs/promises";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const BROWSER_HOST = resolve(HERE, "../browser-host");

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const die = (m) => { console.error("assemble-landing: FAIL: " + m); process.exit(1); };
const OUT = resolve(HERE, arg("--out", "./public"));

if (!existsSync(OUT)) die(`deploy dir not found at ${OUT} (run assemble.mjs first)`);

// ── 1. Relocate the isolation proof to /isolation-proof.html. ────────────────
// Regenerate it cleanly from the committed source (same favicon injection as
// assemble.mjs) so it carries no root-page banners.
const srcHtml = await readFile(join(BROWSER_HOST, "index.html"), "utf8");
if (!srcHtml.includes("</head>")) die("browser-host/index.html has no </head> to inject favicon into");
await writeFile(join(OUT, "isolation-proof.html"),
  srcHtml.replace("</head>", '  <link rel="icon" href="data:,">\n</head>'));
// Remove the old root proof if assemble.mjs left one at index.html (we overwrite
// index.html below anyway; the rename is belt-and-suspenders for clarity).
if (existsSync(join(OUT, "index.html"))) {
  try { await rename(join(OUT, "index.html"), join(OUT, "index.html.bak")); } catch { /* ignore */ }
  if (existsSync(join(OUT, "index.html.bak"))) {
    const { rm } = await import("node:fs/promises");
    await rm(join(OUT, "index.html.bak"), { force: true });
  }
}

// ── 2. Write the clean landing page. ─────────────────────────────────────────
const LANDING = `<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Pulp — Web Demos (WAM &amp; WCLAP)</title>
<meta name="description" content="Live in-browser demos of the Pulp example plugins and classic effects, built two ways from the same C++ Processor: WCLAP (threaded WebAssembly CLAP, hosted here on Cloudflare Pages) and WAM (single-threaded, on GitHub Pages).">
<meta property="og:type" content="website">
<meta property="og:site_name" content="Pulp">
<meta property="og:title" content="Pulp — Web Demos (WAM & WCLAP)">
<meta property="og:description" content="Live in-browser demos of the Pulp plugins, built two ways from the same C++ Processor: WCLAP (here on Cloudflare Pages) and WAM (on GitHub Pages).">
<link rel="icon" href="data:,">
<style>
  :root{
    --bg:#0f1116; --surface:#161a21; --surface2:#1d222c; --border:#2b313d;
    --text:#e7e9ee; --muted:#9aa3b5; --accent:#2bd4be; --accent2:#5b8cff;
  }
  :root[data-theme="light"]{
    --bg:#f6f7f9; --surface:#ffffff; --surface2:#f0f2f5; --border:#d9dee6;
    --text:#1a1d23; --muted:#5b6472; --accent:#0f9e8c; --accent2:#3a63d0;
  }
  @media (prefers-color-scheme: light){
    :root:not([data-theme="dark"]){
      --bg:#f6f7f9; --surface:#ffffff; --surface2:#f0f2f5; --border:#d9dee6;
      --text:#1a1d23; --muted:#5b6472; --accent:#0f9e8c; --accent2:#3a63d0;
    }
  }
  *{box-sizing:border-box}
  html{color-scheme:dark light}
  body{margin:0;background:var(--bg);color:var(--text);
       font:15px/1.6 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
  .wrap{max-width:880px;margin:0 auto;padding:56px 20px 80px}
  header h1{margin:0 0 10px;font-size:26px;font-weight:600;letter-spacing:-.01em}
  header p{margin:0;color:var(--muted);font-size:15px;max-width:660px}
  section{margin-top:44px}
  h2{font-size:13px;letter-spacing:.09em;text-transform:uppercase;color:var(--muted);
     margin:0 0 4px;font-weight:600}
  .lede{margin:0 0 18px;color:var(--muted);font-size:13.5px;max-width:660px}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:16px}
  a.card{display:flex;flex-direction:column;gap:6px;text-decoration:none;color:inherit;
    background:var(--surface);border:1px solid var(--border);border-radius:12px;
    padding:20px 20px 18px;transition:border-color .15s,transform .15s,background .15s}
  a.card:hover{border-color:var(--accent);transform:translateY(-2px);background:var(--surface2)}
  a.card .k{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted)}
  a.card .t{font-size:17px;font-weight:600}
  a.card .d{font-size:13px;color:var(--muted);flex:1}
  a.card .go{color:var(--accent);font-size:12.5px;letter-spacing:.05em;margin-top:2px}
  .primary a.card .t{font-size:18px}
  .docs{display:flex;flex-wrap:wrap;gap:10px}
  .docs a{text-decoration:none;color:var(--text);background:var(--surface);
    border:1px solid var(--border);border-radius:8px;padding:9px 14px;font-size:13.5px;
    transition:border-color .15s,color .15s}
  .docs a:hover{border-color:var(--accent2);color:var(--accent2)}
  footer{margin-top:52px;padding-top:20px;border-top:1px solid var(--border);
    color:var(--muted);font-size:12.5px;line-height:1.8}
  footer a{color:var(--accent);text-decoration:none}
  footer a:hover{text-decoration:underline}
  .note{color:var(--muted);font-size:12.5px;margin-top:8px}
  .note b{color:var(--text);font-weight:600}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>Pulp — Web Demos</h1>
    <p>Every Pulp example plugin runs live in your browser on the <b>same C++ <code>Processor</code></b>
      as the native VST3&nbsp;/&nbsp;AU&nbsp;/&nbsp;CLAP build — offered two ways. <b>WCLAP</b> is the
      threaded-WebAssembly CLAP build hosted here (Cloudflare Pages, cross-origin isolated);
      <b>WAM</b> is the single-threaded build on plain GitHub Pages.</p>
  </header>

  <section class="primary">
    <h2>WCLAP demos — here</h2>
    <p class="lede">A threaded WebAssembly CLAP hosted in a worklet-resident CLAP host. Runs on this
      cross-origin-isolated site (COOP/COEP).</p>
    <div class="cards">
      <a class="card" href="./example-plugins/">
        <span class="k">WCLAP</span><span class="t">Example Plugins</span>
        <span class="d">Eight small plugins — a synth, MIDI utilities, a gain/pan effect and more.</span>
        <span class="go">Open gallery (8) &rarr;</span>
      </a>
      <a class="card" href="./classic-effects/">
        <span class="k">WCLAP</span><span class="t">Classic Effects</span>
        <span class="d">Fifteen classic audio effects — chorus, delay, phaser, wah, compressor and more.</span>
        <span class="go">Open gallery (15) &rarr;</span>
      </a>
    </div>
  </section>

  <section>
    <h2>WAM demos — on GitHub Pages</h2>
    <p class="lede">The same plugins compiled with Emscripten to a single-threaded WAM v2 module —
      no cross-origin isolation needed, so any static host works.</p>
    <div class="cards">
      <a class="card" href="https://www.generouscorp.com/pulp-example-plugins/">
        <span class="k">WAM</span><span class="t">Example Plugins</span>
        <span class="d">The same eight plugins as WAM v2, on plain GitHub Pages.</span>
        <span class="go">Open gallery &rarr;</span>
      </a>
      <a class="card" href="https://www.generouscorp.com/pulp-classic-effects/">
        <span class="k">WAM</span><span class="t">Classic Effects</span>
        <span class="d">The same fifteen effects as WAM v2, on plain GitHub Pages.</span>
        <span class="go">Open gallery &rarr;</span>
      </a>
    </div>
  </section>

  <section>
    <h2>Learn more about Pulp WAM &amp; WCLAP</h2>
    <p class="lede">How the two web builds are made, what they share with the native plugin, and the
      CMake surface.</p>
    <div class="docs">
      <a href="https://www.generouscorp.com/pulp/web-plugins.html">Web plugins guide</a>
      <a href="https://www.generouscorp.com/pulp/web-plugin-support.html">Web plugin support &amp; references</a>
      <a href="https://www.generouscorp.com/pulp/cmake.html">CMake reference</a>
    </div>
  </section>

  <footer>
    <div>Under the hood: <a href="./player/">shared-player WCLAP demo</a> (PulpGain in real time behind
      the same player as the WAM demos) &middot; <a href="./isolation-proof">single-plugin isolation
      proof</a> (minimal in-browser CLAP host).</div>
    <div class="note"><b>WCLAP</b> is Pulp's build of the WebCLAP browser plugin format — a CLAP plugin
      compiled to threaded WebAssembly and hosted in the browser, which is why this site needs the
      cross-origin-isolation headers a static host like GitHub Pages can't send.</div>
  </footer>
</div>
</body>
</html>
`;
await writeFile(join(OUT, "index.html"), LANDING);

console.log("assemble-landing: wrote root landing page → " + join(OUT, "index.html"));
console.log("  index.html          (landing: WCLAP galleries + WAM galleries + docs)");
console.log("  isolation-proof.html (relocated single-plugin proof; assets stay at root)");
