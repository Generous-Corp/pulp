// Assemble the WebCLAP 1:1 demo gallery into the Cloudflare Pages deploy dir.
//
// A WebCLAP version of EVERY plugin that already has a WAM demo, so this site
// mirrors both existing WAM galleries one-for-one — same rows, same order, same
// widgets, same copy — only the ABI underneath differs (a threaded WebCLAP
// .wasm hosted in a worklet-resident CLAP host, vs a WAM DSP module). Emits:
//
//   public/vendor-player/**              — ONE shared copy of @danielraffel/web-player
//                                          (shell, adapters/wclap.js, widgets,
//                                          theme, vendor/pulp-wasm worklet).
//   public/example-plugins/index.html    — gallery mirroring the WAM example
//                                          gallery (8 cards).
//   public/example-plugins/<slug>/       — per-plugin shared-player page +
//                                          <Target>.wasm  (8 dirs)
//   public/classic-effects/index.html    — gallery mirroring the WAM classic
//                                          effects gallery (15 cards).
//   public/classic-effects/<slug>/       — per-plugin page + <Target>.wasm (15)
//   public/super-convolver/{wam,wclap}/  — the SAME plugin in BOTH ABIs, side by
//                                          side (the only section that ships its
//                                          WAM page here); needs --wam-build.
//
// The two galleries are linked from the WAM sites' "also runs as WebCLAP →"
// footer (which points at this site root); this script also injects a gallery
// nav banner into the generated root isolation-proof page so `/` surfaces both.
//
// Every per-plugin page mounts the SAME shell.js the WAM demos use, driven by
// the WebCLAP adapter instead of the WAM one, so the UI is byte-for-byte the
// same. The MIDI-effect pages audition through the MonoSynth WebCLAP module via
// the adapter's createSecondary(), exactly like the WAM pages feed mono-synth.
//
// The sibling `_headers` (COOP/COEP/CORP + MIME) applies to every nested path,
// so crossOriginIsolated holds under /example-plugins/** and /classic-effects/**
// (WebCLAP imports a shared WebAssembly.Memory → needs cross-origin isolation).
//
// Usage:  node assemble-gallery.mjs [--build <dir>] [--out <dir>]
// Defaults: --build ../build   --out ./public
import { mkdir, copyFile, readFile, writeFile, cp } from "node:fs/promises";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../../..");
const PKG_SRC = resolve(REPO, "packages/pulp-web-player/src");

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const die = (m) => { console.error("assemble-gallery: FAIL: " + m); process.exit(1); };

const BUILD = resolve(HERE, arg("--build", "../build"));
const OUT = resolve(HERE, arg("--out", "./public"));

// Absolute base URL of the deployed site — baked into every page's og:url and
// og:image so links unfurl (social crawlers fetch the raw HTML and resolve
// absolute URLs, matching the WAM sites' convention). Override with
// SITE_BASE=... or --site-base; default is the canonical Pages host.
const SITE_BASE = (arg("--site-base", process.env.SITE_BASE) || "https://pulp-wclap-demos.pages.dev")
  .replace(/\/+$/, "");

if (!existsSync(PKG_SRC)) die(`package src not found at ${PKG_SRC}`);
if (!existsSync(BUILD)) die(`wclap build dir not found at ${BUILD} (build the 23 targets first)`);

// ── The 1:1 catalog. Order + tags + card copy + player config mirror the two
//    WAM galleries exactly (web/site/index.html + each web/site/<slug>/index.html
//    in pulp-example-plugins-web-demos and pulp-classic-effects-web).
const SRC_EXAMPLE = "https://github.com/danielraffel/pulp-example-plugins/tree/main";
const SRC_CLASSIC = "https://github.com/danielraffel/pulp-classic-effects/tree/main";
const WAM_EXAMPLE = "https://www.generouscorp.com/pulp-example-plugins";
const WAM_CLASSIC = "https://www.generouscorp.com/pulp-classic-effects";

// synthUrls for the MIDI-effect pages: audition through the MonoSynth WebCLAP
// module (sibling section dir), driven by the same worklet host.
const MONOSYNTH_DSP = "../../example-plugins/mono-synth/MonoSynth.wasm";
const WORKLET = "../../vendor-player/vendor/pulp-wasm/wclap-processor.js";
const SYNTH_URLS = { dsp: MONOSYNTH_DSP, processor: WORKLET };

const SECTIONS = [
  {
    slug: "example-plugins",
    galleryTitle: "Pulp — Example Plugins",
    galleryHead: "Pulp — Example Plugin Gallery",
    galleryIntro:
      "Eight small plugins running right here in your browser, as WebCLAP — the same " +
      "plugins as the WAM gallery, compiled to a threaded WebAssembly CLAP and hosted " +
      "in a worklet-resident CLAP host. Instruments give you a keyboard to play; sound " +
      "effects run a short backing loop through themselves so you can hear the change; " +
      "note effects show you what they're doing. Nothing is uploaded — it all runs on your machine.",
    galleryFoot:
      "Pick a card to open it, then click <b>Start</b> to make sound. Note effects can feed " +
      "their output into a synth so you can hear the result.",
    src: SRC_EXAMPLE, wam: WAM_EXAMPLE,
    otherHref: "../classic-effects/index.html", otherName: "Classic Effects",
    wamGallery: "https://www.generouscorp.com/pulp-example-plugins/",
    plugins: [
      { dir: "mono-synth", target: "MonoSynth", tag: "Instrument", name: "Mono Synth",
        card: "Play one note at a time. Shape each note with attack, decay, sustain, release and volume.",
        title: "Mono Synth",
        subtitle: "Play notes on the keyboard. Shape each one with attack, decay, sustain, release and volume.",
        cfg: { mode: "instrument", paramRows: 2 } },
      { dir: "synth-with-presets", target: "SynthWithPresets", tag: "Instrument", name: "Synth With Presets",
        card: "A small synth with three ready-made sounds — Pluck, Pad and Sine. Pick one and the knobs move to match.",
        title: "Synth With Presets",
        subtitle: "A small synth with three factory presets — Pluck, Pad and Sine. Pick one and the knobs move.",
        cfg: { mode: "instrument", paramRows: 2, controllers: true } },
      { dir: "gain", target: "Gain", tag: "Sound effect", name: "Gain & Pan",
        card: "Make the sound louder or quieter and move it left or right. A looping backing track runs through it.",
        title: "Gain & Pan",
        subtitle: "Make the sound louder or quieter and move it left or right. A looping backing track runs through it — watch the meter.",
        cfg: { mode: "audio-effect", paramRows: 1, widgets: { Gain: "fader" } } },
      { dir: "state-memo", target: "StateMemo", tag: "Sound effect", name: "State Memo",
        card: "A volume control plus a text note that saves and reloads with the plugin.",
        title: "State Memo",
        subtitle: "A volume control plus a text note that saves and reloads with the plugin.",
        cfg: { mode: "audio-effect", paramRows: 1, stateMemo: true } },
      { dir: "midi-transpose", target: "MidiTranspose", tag: "Note effect", name: "MIDI Transpose",
        card: "Shifts every note you play up or down by a set number of semitones and octaves.",
        title: "MIDI Transpose",
        subtitle: "Shifts every note you play up or down by a set number of semitones and octaves.",
        cfg: { paramRows: 1, midiViz: "transpose", synthUrls: SYNTH_URLS } },
      { dir: "mpe-spreader", target: "MpeSpreader", tag: "Note effect", name: "MPE Spreader",
        card: "Gives each note you hold its own channel, so a chord spreads out across the lanes.",
        title: "MPE Spreader",
        subtitle: "Gives each note you hold its own channel, so a chord spreads out across the lanes.",
        cfg: { paramRows: 1, midiViz: "mpe", synthUrls: SYNTH_URLS } },
      { dir: "midi-inspector", target: "MidiInspector", tag: "Note effect", name: "MIDI Inspector",
        card: "Shows every MIDI message going through, live — name, channel and raw bytes.",
        title: "MIDI Inspector",
        subtitle: "Shows every MIDI message going through, live — name, channel and raw bytes.",
        cfg: { paramRows: 1, midiViz: "inspector", synthUrls: SYNTH_URLS } },
      { dir: "sysex-echo", target: "SysexEcho", tag: "Note effect", name: "SysEx Echo",
        card: "Send it a SysEx message and it sends the very same bytes right back.",
        title: "SysEx Echo",
        subtitle: "Send it a SysEx message and it sends the same bytes right back.",
        cfg: { paramRows: 1, midiViz: "sysex" } },
    ],
  },
  {
    slug: "classic-effects",
    galleryTitle: "Pulp Classic Effects",
    galleryHead: "Pulp — Classic Effects Gallery",
    galleryIntro:
      "Fifteen classic audio effects running right here in your browser, as WebCLAP — the " +
      "same plugins as the WAM gallery, compiled to a threaded WebAssembly CLAP and hosted " +
      "in a worklet-resident CLAP host. Each one runs a short backing loop through itself so " +
      "you can hear what it does — or feed it your microphone. Nothing is uploaded; it all " +
      "runs on your machine.",
    galleryFoot:
      "Pick a card to open it, then click <b>Start</b> to make sound. Switch the audio source " +
      "between the built-in loop and your microphone.",
    src: SRC_CLASSIC, wam: WAM_CLASSIC,
    otherHref: "../example-plugins/index.html", otherName: "Example Plugins",
    wamGallery: "https://www.generouscorp.com/pulp-classic-effects/",
    plugins: [
      ["chorus", "Chorus", "Chorus", "Layers slightly detuned copies of the sound to make it thicker and shimmering.", 2],
      ["compressor-expander", "CompressorExpander", "Compressor / Expander", "Evens out loud and quiet parts — or exaggerates the difference. Watch the meter.", 2],
      ["delay", "Delay", "Delay", "Repeats the sound after a short gap, each echo quieter than the last.", 1],
      ["distortion", "Distortion", "Distortion", "Pushes the signal until it clips, adding grit and harmonics.", 1],
      ["flanger", "Flanger", "Flanger", "A sweeping, jet-plane whoosh, made by mixing the sound with a tiny moving delay.", 3],
      ["panning", "Panning", "Panning", "Moves the sound between the left and right speakers.", 1],
      ["parametric-eq", "ParametricEq", "Parametric EQ", "Boosts or cuts one band of frequencies. Pick the frequency, width and amount.", 1],
      ["phaser", "Phaser", "Phaser", "A swirling sweep created by shifting the phase of the sound in stages.", 2],
      ["ping-pong", "PingPong", "Ping-Pong Delay", "Echoes that bounce back and forth between the left and right speakers.", 1],
      ["pitch-shift", "PitchShift", "Pitch Shift", "Moves the sound up or down in pitch without changing its speed.", 1],
      ["ring-mod", "RingMod", "Ring Modulator", "Multiplies the sound by a tone, giving it a metallic, bell-like edge.", 1],
      ["robotization", "Robotization", "Robotization", "Flattens the pitch to a monotone, so anything sounds like a robot.", 1],
      ["tremolo", "Tremolo", "Tremolo", "Turns the volume up and down over and over, giving a pulsing wobble.", 1],
      ["vibrato", "Vibrato", "Vibrato", "Wavers the pitch up and down, like a singer holding a note.", 1],
      ["wah", "Wah", "Wah", "A sweeping filter that opens and closes — the classic “wah” sound.", 3, { inputGain: 0.08 }],
    ].map(([dir, target, name, card, paramRows, extra]) => ({
      dir, target, tag: "Sound effect", name, card,
      title: name, subtitle: card,
      cfg: { mode: "audio-effect", paramRows, ...(extra || {}) },
    })),
  },
];

const esc = (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const attr = (s) => esc(s).replace(/"/g, "&quot;");

// og:url plus the og:image / twitter block. Social crawlers fetch the raw HTML
// and do NOT run JS, so these must be present in the <head> at rest. Mirrors the
// WAM sites' gen-og.mjs: og:url is always emitted; the image + Twitter
// large-image card are emitted only once a sibling og.png exists (gen-og-images.mjs
// produces those), so a page whose shot failed still unfurls with its title +
// description instead of a broken preview. `pageUrl` ends in a slash.
function ogUrlAndImage(pageUrl, hasImage, indent = "  ") {
  const lines = [`<meta property="og:url" content="${attr(pageUrl)}">`];
  if (hasImage) {
    const img = attr(pageUrl + "og.png");
    lines.push(`<meta property="og:image" content="${img}">`);
    lines.push(`<meta name="twitter:card" content="summary_large_image">`);
    lines.push(`<meta name="twitter:image" content="${img}">`);
  }
  return lines.map((l) => indent + l).join("\n");
}

// A per-plugin shared-player page: the SAME shell as the WAM demos, WebCLAP adapter.
function pluginPage(section, p, pageUrl, hasOgImage) {
  // Serialize each per-plugin config key as its own `key: value,` line so it
  // drops straight into the mountDemo({...}) object (values are JSON-safe:
  // strings, numbers, booleans, and the widgets / synthUrls plain objects).
  const cfgProps = Object.entries(p.cfg)
    .map(([k, v]) => `    ${k}: ${JSON.stringify(v)},`)
    .join("\n");
  return `<!doctype html>
<html lang="en" data-theme="dark">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <title>${esc(p.title)} — Pulp WebCLAP demo</title>
  <meta name="description" content="${attr(p.subtitle)}">
  <meta name="pulp:source" content="${attr(section.src + "/" + p.dir)}">
  <meta property="og:type" content="website">
  <meta property="og:site_name" content="Pulp">
  <meta property="og:title" content="${attr(p.title + " — Pulp WebCLAP demo")}">
  <meta property="og:description" content="${attr(p.subtitle)}">
${ogUrlAndImage(pageUrl, hasOgImage)}
  <link rel="icon" href="data:,">
</head>
<body>
<div id="app"></div>
<script type="module">
  // The SAME shared player (@danielraffel/web-player) the WAM demos mount — imported
  // host-agnostically (shell.js, NOT index.js, so the WAM backend is never
  // pulled in) and driven by the WebCLAP adapter. Byte-for-byte the same UI as
  // the WAM page for this plugin; only the backend (a worklet-resident CLAP host
  // over a threaded .wasm) differs. Needs cross-origin isolation (the sibling
  // _headers) because the module imports a shared WebAssembly.Memory.
  import { mountDemo } from "../../vendor-player/shell.js";
  import { createWclapAdapter } from "../../vendor-player/adapters/wclap.js";

  // Diagnostics surface for headless proofs (real-time quanta clock + RMS). Not
  // part of the UI; the shell renders its own scope/meter from the analyser.
  window.__wclap = { meters: [], lastMeter: null };

  mountDemo({
    root: document.getElementById("app"),
    title: "${esc(p.title)}",
    subtitle: "${attr(p.subtitle)}",
    hostLabel: "WebCLAP",
    hostDocsHref: "https://github.com/free-audio/clap",
    galleryHref: "../index.html",
    sourceHref: "${attr(section.src + "/" + p.dir)}",
    dspUrl: "./${p.target}.wasm",
    processorUrl: "${WORKLET}",
${cfgProps}
    createAdapter: (ctx, urls) => createWclapAdapter(ctx, urls, {
      diag: true,
      onDiag: (m) => {
        window.__wclap.lastMeter = m;
        window.__wclap.meters.push(m);
        if (window.__wclap.meters.length > 600) window.__wclap.meters.shift();
      },
    }),
  });
</script>
</body>
</html>
`;
}

// A gallery index page mirroring the WAM gallery (same layout + card markup),
// cross-linking WAM ↔ WebCLAP.
function galleryPage(section, pageUrl, hasOgImage) {
  const cards = JSON.stringify(
    section.plugins.map((p) => ({ dir: p.dir, tag: p.tag, name: p.name, desc: p.card })),
    null, 2);
  return `<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(section.galleryHead)} (WebCLAP)</title>
<meta name="description" content="${attr(section.galleryIntro)}">
<meta property="og:type" content="website">
<meta property="og:site_name" content="Pulp">
<meta property="og:title" content="${attr(section.galleryHead + " (WebCLAP)")}">
<meta property="og:description" content="${attr(section.galleryIntro)}">
${ogUrlAndImage(pageUrl, hasOgImage, "")}
<link rel="stylesheet" href="../vendor-player/theme/tokens.css">
<link rel="stylesheet" href="../vendor-player/theme/fonts.css">
<style>
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg-surface);color:var(--text-primary);
       font:14px/1.5 var(--font-family-native)}
  .wrap{max-width:860px;margin:0 auto;padding:48px 20px 80px}
  header h1{margin:0 0 6px;font-size:22px;font-weight:400}
  header p{margin:0 0 32px;color:var(--text-secondary);font-size:13px;max-width:620px;line-height:1.6}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:16px}
  .card{background:var(--bg-primary);border:1px solid var(--control-border);border-radius:10px;
        padding:18px 18px 16px;display:flex;flex-direction:column;gap:8px;
        text-decoration:none;color:inherit;transition:border-color .15s,transform .15s}
  .card:hover{border-color:var(--accent-primary);transform:translateY(-2px)}
  .card .tag{font-size:10px;letter-spacing:.1em;text-transform:uppercase;color:var(--text-secondary)}
  .card h2{margin:0;font-size:15px}
  .card p{margin:0;color:var(--text-secondary);font-size:12.5px;flex:1;line-height:1.5}
  .card .go{color:var(--accent-primary);font-size:12px;letter-spacing:.06em;margin-top:4px}
  footer{margin-top:40px;color:var(--text-secondary);font-size:12px;line-height:1.7}
  footer a{color:var(--accent-primary);text-decoration:none}
  footer a:visited{color:var(--accent-primary)}
  footer a:hover{text-decoration:underline}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>${esc(section.galleryTitle)} <span style="color:var(--text-secondary);font-size:14px">— WebCLAP</span></h1>
    <p>${section.galleryIntro}</p>
  </header>
  <div class="grid" id="grid"></div>
  <footer>
    ${section.galleryFoot}
    <div style="margin-top:16px">These demos run as <b>WebCLAP</b> — a threaded WebAssembly
      CLAP (SharedArrayBuffer + shared memory), which needs a header-capable host
      (Cloudflare, with COOP/COEP). The same Pulp plugins also run as <b>WAM</b> on plain
      GitHub Pages: <a href="${section.wamGallery}">also runs as WAM &rarr;</a></div>
    <div style="margin-top:16px">Other Pulp WebCLAP demos:
      <a href="${section.otherHref}">${section.otherName} &rarr;</a></div>
    <div style="margin-top:6px"><a href="${section.src.replace("/tree/main", "")}">Source on GitHub &nearr;</a></div>
  </footer>
</div>
<script>
const plugins = ${cards};
const grid = document.getElementById("grid");
for (const p of plugins) {
  const a = document.createElement("a");
  a.className = "card";
  a.href = p.dir + "/index.html";
  a.innerHTML =
    '<span class="tag">' + p.tag + '</span>' +
    '<h2>' + p.name + '</h2>' +
    '<p>' + p.desc + '</p>' +
    '<span class="go">▶ Open demo</span>';
  grid.appendChild(a);
}
</script>
</body>
</html>
`;
}

// ── SuperConvolver: the SAME plugin, both ABIs, side by side. ───────────────
// Every other plugin on this site is WebCLAP-only here (its WAM twin lives on
// the WAM sites). SuperConvolver ships BOTH pages under one section so the two
// can be opened back to back: identical player, identical UI, identical audio —
// the only difference is the module underneath. Anything a viewer can see or
// hear differ between them is a shared-player bug, not a per-demo tweak.
//
// The WAM page needs the SINGLE_FILE worklet build (the wasm embedded in the
// .js — an AudioWorkletGlobalScope cannot fetch), which comes from the OTHER
// build tree (examples/web-demos/wasm-build/build, --wam-build). When that tree
// is absent the section is skipped with a warning rather than failing the site
// build, since the 23 WebCLAP gallery pages do not depend on it.
const WAM_BUILD = resolve(HERE, arg("--wam-build", "../../wasm-build/build"));
// Pulp's own View tree compiled to wasm (Skia Ganesh on WebGL2) — DSP-free, and
// mounted through the player's customUi seam, so the SAME module serves both
// ABIs. Optional: without it the pages are built with no customUi at all and the
// player renders its generated parameter grid. A browser that HAS the module but
// no WebGL2 context also ends up on that grid — but only because the factory
// below hands its mount promise to the player as `ready`, which the customUi seam
// falls back on when it rejects (a UI module that only fails asynchronously and
// reports nothing would leave an empty panel).
const UI_BUILD = resolve(HERE, arg("--ui-build", "../../super-convolver-ui/build-webui"));
const UI_SRC = resolve(REPO, "examples/web-demos/super-convolver-ui");
const UI_MODULE = "PulpSuperConvolverUi";

const SC_SRC = "https://github.com/danielraffel/pulp/tree/main/examples/super-convolver";
const SC_TITLE = "SuperConvolver";
const SC_SUBTITLE =
  "A convolution reverb running in your browser. Size sets how long the space rings; " +
  "Mix balances the dry sound against the reverb. Nothing is uploaded — it all runs on your machine.";

// What the two cards are actually showing, stated plainly. The interesting part is
// not that a plugin runs on a web page; it is that this is the SAME processor that
// builds as a native VST3/AU/CLAP, and that the WebCLAP page is a real compiled
// CLAP plugin — the browser is just another host calling clap_entry. Say that once,
// without adjectives, and let the demo carry the rest.
const SC_LEDE =
  "The same C++ processor that builds as a native VST3, AU and CLAP plugin, compiled for the web. " +
  "The editor is not HTML: it is Pulp's own view tree — the widgets, layout and text shaping the " +
  "desktop editor uses — drawn by Skia straight onto the page's canvas.";
const SC_CFG = { mode: "audio-effect", paramRows: 2 };

// Cache-bust the MAIN-THREAD player entry only. The worklet processor and the
// DSP module are deliberately NOT busted: the processor's registered name is
// derived from its URL, so the main thread and the worklet must agree on one
// URL — a query string on either forks the name and the node never constructs.
// Every module the ?v= is stamped on must be part of the hash, or a change to one
// of them ships behind a stale cache entry.
async function playerVersion() {
  const { createHash } = await import("node:crypto");
  const h = createHash("sha256");
  for (const f of ["shell.js", "widgets/index.js", "adapters/wam.js", "adapters/wclap.js"])
    h.update(await readFile(join(PKG_SRC, f)));
  h.update(await readFile(join(UI_SRC, "pulp-ui.js")));
  return h.digest("hex").slice(0, 8);
}

function superConvolverPage(abi, pageUrl, hasOgImage, v, withUi) {
  const isWam = abi === "wam";
  const adapterMod = isWam ? "wam.js" : "wclap.js";
  const adapterFn = isWam ? "createWamAdapter" : "createWclapAdapter";
  const dspUrl = isWam ? "./wam-dsp.js" : "./SuperConvolver.wasm";
  // WAM's processor module STATICALLY imports its SINGLE_FILE DSP factory as the
  // sibling `./wam-dsp.js` (wam-processor.js:22), and that one addModule() is the
  // whole load — dspUrl is never addModule'd. So the processor cannot be shared out
  // of vendor-player/: it must sit in this plugin's own directory, next to that
  // plugin's DSP. Pointing at the shared copy resolves ./wam-dsp.js to a 404 and
  // the module graph fails as an opaque "Unable to load a worklet's module".
  const processorUrl = isWam ? "./wam-processor.js" : WORKLET;
  const hostLabel = isWam ? "WAM" : "WebCLAP";
  const hostDocsHref = isWam
    ? "https://www.webaudiomodules.com/docs/intro/"
    : "https://github.com/free-audio/clap";
  const other = isWam ? "../wclap/index.html" : "../wam/index.html";
  const otherLabel = isWam ? "WebCLAP" : "WAM";
  // Pulp's real View tree, painted by Skia Ganesh on WebGL2, in place of the
  // generated parameter grid. The factory must return its handle SYNCHRONOUSLY
  // (the shell's seam is sync), so it appends the canvas, starts the async mount,
  // and hands back BOTH a destroy() that awaits it and the mount promise as
  // `ready`. `ready` is what makes the fallback real: mountPulpUi rejects
  // asynchronously when the browser has no WebGL2 context, and the seam restores
  // the generated parameter grid on that rejection instead of leaving an empty
  // panel. Both ABI pages get this block verbatim — the module is DSP-free and
  // talks only to the HostAdapter, so any visible difference between the two pages
  // is a shared-player bug.
  const uiImport = withUi ? `\n  import { mountPulpUi } from "./pulp-ui.js?v=${v}";` : "";
  const uiProp = withUi ? `
    customUi: (container, adapter) => {
      const canvas = document.createElement("canvas");
      canvas.id = "pulp-ui-canvas";
      canvas.style.cssText = "width:100%;aspect-ratio:8/5;display:block";
      container.appendChild(canvas);
      const pending = mountPulpUi(canvas, adapter, { moduleUrl: "./${UI_MODULE}.js" });
      // The shell handles the rejection (it restores the grid); this keeps the
      // promise from also surfacing as an unhandled rejection, and logs it once.
      pending.catch((err) => { console.warn("Pulp UI failed to mount:", err); });
      return {
        ready: pending,
        destroy: () => { pending.then((ui) => ui && ui.destroy(), () => {}); },
      };
    },` : "";
  return `<!doctype html>
<html lang="en" data-theme="dark">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <title>${esc(SC_TITLE)} — Pulp ${hostLabel} demo</title>
  <meta name="description" content="${attr(SC_SUBTITLE)}">
  <meta name="pulp:source" content="${attr(SC_SRC)}">
  <meta property="og:type" content="website">
  <meta property="og:site_name" content="Pulp">
  <meta property="og:title" content="${attr(SC_TITLE + " — Pulp " + hostLabel + " demo")}">
  <meta property="og:description" content="${attr(SC_SUBTITLE)}">
${ogUrlAndImage(pageUrl, hasOgImage)}
  <link rel="icon" href="data:,">
</head>
<body>
<div id="app"></div>
<script type="module">
  // The SAME shared player every other demo on this site mounts, driven by the
  // ${hostLabel} adapter.
  import { mountDemo } from "../../vendor-player/shell.js?v=${v}";
  import { ${adapterFn} } from "../../vendor-player/adapters/${adapterMod}?v=${v}";${uiImport}

  mountDemo({
    root: document.getElementById("app"),
    title: "${esc(SC_TITLE)}",
    subtitle: "${attr(SC_SUBTITLE)}",
    hostLabel: "${hostLabel}",
    hostDocsHref: "${hostDocsHref}",
    galleryHref: "../index.html",
    sourceHref: "${attr(SC_SRC)}",
    dspUrl: "${dspUrl}",
    processorUrl: "${processorUrl}",
${Object.entries(SC_CFG).map(([k, val]) => `    ${k}: ${JSON.stringify(val)},`).join("\n")}${uiProp}
    createAdapter: (ctx, urls) => ${adapterFn}(ctx, urls),
  });
</script>
<p style="max-width:860px;margin:0 auto;padding:0 20px 40px;
          font:13px/1.6 system-ui;color:#8b96a3">
  This is the <b>${hostLabel}</b> build. The same plugin also runs as
  <a href="${other}" style="color:#2bd4be">${otherLabel} &rarr;</a> — same player, same UI, same audio.
</p>
</body>
</html>
`;
}

function superConvolverIndex(pageUrl, hasOgImage) {
  return `<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Pulp — SuperConvolver (WAM &amp; WebCLAP)</title>
<meta name="description" content="${attr(SC_SUBTITLE)}">
<meta name="pulp:source" content="${attr(SC_SRC)}">
<meta property="og:type" content="website">
<meta property="og:site_name" content="Pulp">
<meta property="og:title" content="Pulp — SuperConvolver (WAM &amp; WebCLAP)">
<meta property="og:description" content="${attr(SC_SUBTITLE)}">
${ogUrlAndImage(pageUrl, hasOgImage, "")}
<link rel="stylesheet" href="../vendor-player/theme/tokens.css">
<link rel="stylesheet" href="../vendor-player/theme/fonts.css">
<style>
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg-surface);color:var(--text-primary);
       font:14px/1.5 var(--font-family-native)}
  .wrap{max-width:860px;margin:0 auto;padding:48px 20px 80px}
  header h1{margin:0 0 6px;font-size:22px;font-weight:400}
  header p{margin:0 0 32px;color:var(--text-secondary);font-size:13px;max-width:620px;line-height:1.6}
  .abis{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px}
  .abi{background:var(--bg-primary);border:1px solid var(--control-border);border-radius:10px;
       padding:18px;display:flex;flex-direction:column;gap:8px;text-decoration:none;color:inherit;
       border-top-width:3px;transition:border-color .15s,transform .15s}
  .abi:hover{border-color:var(--accent-primary);transform:translateY(-2px)}
  .abi.wam{border-top-color:#2bd4be}
  .abi.wclap{border-top-color:#c58af9}
  .abi .tag{font-size:10px;letter-spacing:.1em;text-transform:uppercase;color:var(--text-secondary)}
  .abi h2{margin:0;font-size:15px}
  .abi p{margin:0;color:var(--text-secondary);font-size:12.5px;flex:1;line-height:1.5}
  .abi .go{color:var(--accent-primary);font-size:12px;letter-spacing:.06em;margin-top:4px}
  footer{margin-top:40px;color:var(--text-secondary);font-size:12px;line-height:1.7}
  footer a{color:var(--accent-primary);text-decoration:none}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>SuperConvolver <span style="color:var(--text-secondary);font-size:14px">— one plugin, two web ABIs</span></h1>
    <p>${esc(SC_SUBTITLE)}</p>
    <p>${esc(SC_LEDE)}</p>
  </header>
  <div class="abis">
    <a class="abi wam" href="./wam/index.html">
      <span class="tag">Web Audio Module</span>
      <h2>SuperConvolver — WAM</h2>
      <p>The processor compiled to a headless DSP module and driven directly by an AudioWorklet.
         Runs on any static host — no special headers.</p>
      <span class="go">▶ Open the WAM demo</span>
    </a>
    <a class="abi wclap" href="./wclap/index.html">
      <span class="tag">WebCLAP</span>
      <h2>SuperConvolver — WebCLAP</h2>
      <p>Not a port: the CLAP plugin itself, compiled to wasm and loaded by a CLAP host that lives
         inside the audio worklet. It calls the same <code>clap_entry</code> a desktop DAW calls.
         Needs COOP/COEP.</p>
      <span class="go">▶ Open the WebCLAP demo</span>
    </a>
  </div>
  <footer>
    Both pages mount the same player, so the UI, the keyboard, the scope and the meter are identical —
    only the ABI underneath differs. Open them side by side; if you can tell which is which without
    reading the badge, that is a bug.
    <div style="margin-top:6px">
      The DSP is the CPU convolution engine, which is also the desktop default. There is no GPU audio
      in the browser — WebGL2 has no compute shaders — though the editor above is GPU-rendered.
    </div>
    <div style="margin-top:6px"><a href="${SC_SRC}">Source on GitHub &nearr;</a></div>
  </footer>
</div>
</body>
</html>
`;
}

async function emitSuperConvolver() {
  const wclapWasm = [join(BUILD, "SuperConvolver.wclap", "module.wasm"), join(BUILD, "SuperConvolver.wasm")]
    .find((p) => existsSync(p));
  const wamJs = join(WAM_BUILD, "SuperConvolverWorklet.js");
  if (!wclapWasm || !existsSync(wamJs)) {
    console.warn("assemble-gallery: skipping super-convolver — needs the WebCLAP module in " +
                 `${BUILD} AND SuperConvolverWorklet.js in ${WAM_BUILD} (--wam-build).`);
    return 0;
  }
  // The Pulp UI module: <module>.js + .wasm + the .data MEMFS image (it carries
  // icudtl.dat, which SkUnicode needs for text shaping — a page without it draws
  // no text). All three, or the pages fall back to the generated grid.
  const uiFiles = [`${UI_MODULE}.js`, `${UI_MODULE}.wasm`, `${UI_MODULE}.data`];
  const withUi = uiFiles.every((f) => existsSync(join(UI_BUILD, f)));
  if (!withUi)
    console.warn(`assemble-gallery: super-convolver — no Pulp UI module in ${UI_BUILD} ` +
                 "(--ui-build); the pages will use the generated parameter grid.");

  const v = await playerVersion();
  const dir = join(OUT, "super-convolver");
  await mkdir(dir, { recursive: true });
  await writeFile(join(dir, "index.html"),
                  superConvolverIndex(`${SITE_BASE}/super-convolver/`,
                                      existsSync(join(dir, "og.png"))));
  for (const abi of ["wam", "wclap"]) {
    const pdir = join(dir, abi);
    await mkdir(pdir, { recursive: true });
    await writeFile(join(pdir, "index.html"),
                    superConvolverPage(abi, `${SITE_BASE}/super-convolver/${abi}/`,
                                       existsSync(join(pdir, "og.png")), v, withUi));
    if (withUi) {
      for (const f of uiFiles) await copyFile(join(UI_BUILD, f), join(pdir, f));
      await copyFile(join(UI_SRC, "pulp-ui.js"), join(pdir, "pulp-ui.js"));
    }
  }
  // Self-contained WAM dir: the processor + the runtime it imports, and the DSP
  // under the sibling name the processor statically imports (see processorUrl).
  const wamVendor = resolve(REPO, "core/format/src/wasm");
  await copyFile(wamJs, join(dir, "wam", "wam-dsp.js"));
  await copyFile(join(wamVendor, "wam-processor.js"), join(dir, "wam", "wam-processor.js"));
  await copyFile(join(wamVendor, "wam-runtime.mjs"), join(dir, "wam", "wam-runtime.mjs"));
  await copyFile(wclapWasm, join(dir, "wclap", "SuperConvolver.wasm"));
  console.log(`  super-convolver/**         (the same plugin as WAM and as WebCLAP` +
              `${withUi ? ", with Pulp's own UI" : ""})`);
  return 3;
}

// ── 1. shared @danielraffel/web-player copy (once). ─────────────────────────────────
await mkdir(OUT, { recursive: true });
await cp(PKG_SRC, join(OUT, "vendor-player"), { recursive: true });

// _headers if run standalone.
if (!existsSync(join(OUT, "_headers"))) await copyFile(join(HERE, "_headers"), join(OUT, "_headers"));

// ── 2. sections: gallery index + per-plugin pages + wasm. ───────────────────
let wrote = 0, wasmCopied = 0;
for (const section of SECTIONS) {
  const sectionDir = join(OUT, section.slug);
  await mkdir(sectionDir, { recursive: true });
  // og.png is produced by gen-og-images.mjs AFTER this assemble pass; on the
  // second pass (post-render) the file exists and the og:image block is baked
  // in. Absolute URLs end in a slash so `${url}og.png` resolves colocated.
  const galleryUrl = `${SITE_BASE}/${section.slug}/`;
  await writeFile(
    join(sectionDir, "index.html"),
    galleryPage(section, galleryUrl, existsSync(join(sectionDir, "og.png"))));
  wrote++;
  for (const p of section.plugins) {
    const pdir = join(sectionDir, p.dir);
    await mkdir(pdir, { recursive: true });
    const pageUrl = `${SITE_BASE}/${section.slug}/${p.dir}/`;
    await writeFile(
      join(pdir, "index.html"),
      pluginPage(section, p, pageUrl, existsSync(join(pdir, "og.png"))));
    wrote++;
    const wasm = join(BUILD, `${p.target}.wclap`, "module.wasm");
    const wasmFlat = join(BUILD, `${p.target}.wasm`);
    const wasmSrc = existsSync(wasm) ? wasm : wasmFlat;
    if (!existsSync(wasmSrc)) die(`missing wasm for ${section.slug}/${p.dir} (${p.target}); build it first`);
    await copyFile(wasmSrc, join(pdir, `${p.target}.wasm`));
    wasmCopied++;
  }
}

wrote += await emitSuperConvolver();

// ── 3. surface both galleries from the generated root isolation-proof page. ──
const rootIndex = join(OUT, "index.html");
if (existsSync(rootIndex)) {
  let html = await readFile(rootIndex, "utf8");
  if (!html.includes('href="./example-plugins/"') && html.includes("<body>")) {
    const banner = `<body>\n  <div style="max-width:900px;margin:12px auto;padding:10px 14px;` +
      `border:1px solid #2bd4be;border-radius:8px;font:14px/1.5 system-ui;background:#0c1116;color:#cde">` +
      `<strong>WebCLAP demo gallery →</strong> ` +
      `<a href="./example-plugins/" style="color:#2bd4be">Example Plugins (8)</a> &middot; ` +
      `<a href="./classic-effects/" style="color:#2bd4be">Classic Effects (15)</a> &middot; ` +
      `<a href="./super-convolver/" style="color:#2bd4be">SuperConvolver (WAM &amp; WebCLAP)</a> ` +
      `— every WAM demo, rebuilt as a threaded WebCLAP module behind the same player.</div>`;
    html = html.replace("<body>", banner);
    await writeFile(rootIndex, html);
    console.log("  index.html                 (+ gallery banner → example-plugins / classic-effects)");
  }
}

const rel = (p) => p.replace(REPO + "/", "");
console.log("assemble-gallery: wrote WebCLAP 1:1 demo gallery → " + rel(OUT));
console.log(`  vendor-player/**           (shared shell + wclap adapter + worklet)`);
console.log(`  ${wrote} HTML pages          (2 galleries + 23 per-plugin shared-player pages` +
            `, + 3 for super-convolver when both build trees are present)`);
console.log(`  ${wasmCopied} WebCLAP modules     (one .wasm per plugin)`);
