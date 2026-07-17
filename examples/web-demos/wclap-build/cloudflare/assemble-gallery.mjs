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
import { brotliCompressSync, constants as zlibConstants } from "node:zlib";
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

// Two builds of the same plugin, each in both ABIs. v1 is the plain Ink & Signal
// editor over the CPU convolver — the default a Pulp plugin gets for free. v2 is
// the full desktop editor (the animated field renderer) over WebGPU-compute DSP.
//
// v2's cards are emitted ONLY when its build tree is on disk (see scVariants
// below), so this page can never advertise a link that 404s: when the v2 build
// lands, its row appears; until then the page simply shows v1. Same discipline as
// the optional Pulp-UI module above.
const SC_V2_BUILD = resolve(HERE, arg("--v2-build", "../../super-convolver-gpu/build-webgpu"));
const SC_VARIANTS = [
  {
    id: "v1",
    dir: "",                       // v1 lives at /super-convolver/{wam,wclap}/ (stable URLs)
    name: "Ink & Signal",
    blurb: "The stock Pulp editor over the CPU convolution engine — what a Pulp plugin gets " +
           "for free, with no GPU work of its own.",
    always: true,
  },
  {
    id: "v2",
    dir: "gpu",                    // v2 lives at /super-convolver/gpu/{wam,wclap}/
    name: "GPU editor + GPU DSP",
    blurb: "The desktop editor itself — including the animated acoustic field — over convolution " +
           "run as a WebGPU compute shader in a worker, with the CPU engine as the fallback.",
    always: false,
    buildDir: SC_V2_BUILD,
  },
];
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
  // The shell's ?v= must move when ANYTHING in its import closure moves, not just
  // the shell itself: its own static imports (./ui/*.js, ./state/*.js) carry no
  // query, so they are the modules a stale cache holds onto. The drop zone
  // (ui/file-upload.js) is in that closure — leave it out and a fix to it ships
  // behind a cache entry nobody can see.
  for (const f of ["shell.js", "widgets/index.js", "adapters/wam.js", "adapters/wclap.js",
                   "state/plugin-state.js", "ui/file-upload.js", "ui/custom-ui.js"])
    h.update(await readFile(join(PKG_SRC, f)));
  for (const f of ["pulp-ui.js", "ir-source.js"])
    h.update(await readFile(join(UI_SRC, f)));
  return h.digest("hex").slice(0, 8);
}

// ── The plugin canvas box ───────────────────────────────────────────────────────
//
// 8:3, and it is a FLOOR, not a preference. The editor's content — title, one row of
// knobs, their labels and values — ends around 190px at a 590px width, so an 8:5 box
// left a third of the panel as dead space under the controls. But do NOT keep shrinking
// it to close the gap: the editor lays out PROPORTIONALLY to the canvas, so a shorter box
// does not crop the empty bottom — it squeezes every row together, and the status line
// lands ON TOP of the knob labels. Measured at 8/2.75 and 8/2.4; both collide. The gap is
// closed the right way instead, by putting the file-upload slot ABOVE the scope+meter
// (shell.js) so the loader sits directly under the controls.
//
// THIS NOTE LIVES HERE, NOT IN THE EMITTED PAGE. Comments written inside the page template
// are shipped to users inside index.html — and gallery-smoke.mjs asserts that the two
// SuperConvolver pages carry no GPU/Engine surface at all, by grepping the shipped HTML.
// A comment mentioning the engine status line trips that check: the page does not advertise
// a GPU engine, but the grep cannot tell prose from product, and it should not have to. Keep
// the reasoning in the assembler; keep the artifact clean.
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
  // "Load impulse response…" — the browser equivalent of the desktop plugin's file
  // dialog. The INTERACTION is the shared player's: `fileUpload` gives both ABIs the
  // drop zone, the dialog button (the only path that exists on a phone) and the
  // document-level guard that keeps a near miss from navigating away and destroying
  // the running demo. Declaring it here rather than hand-rolling a drop zone in this
  // page is the whole point — a per-page copy is a thing the WAM and WebCLAP builds
  // would eventually disagree about.
  //
  // What the page supplies is the ENCODING, because only SuperConvolver knows how its
  // bytes want to look: irFileUpload()'s onFile decodes with the demo's AudioContext
  // (so the PCM already arrives at the session rate) and hands the plugin the samples
  // through the SDK's plugin-state container — getState -> swap the plugin-owned blob
  // for an SCv2 "Pcm" record -> setState, which preserves every parameter, so loading
  // an IR never resets a knob. That path is IDENTICAL on both ABIs, and the loaded IR
  // survives a state save/restore because it IS the state. The zone lives in the
  // panel's own #fileup slot, so it is present on the Pulp-UI page AND on the
  // generated-grid fallback.
  const irImport = `\n  import { irFileUpload } from "./ir-source.js?v=${v}";`;
  const uiImport = (withUi ? `\n  import { mountPulpUi } from "./pulp-ui.js?v=${v}";` : "") + irImport;
  const uiProp = withUi ? `
    customUi: (container, adapter) => {
      const canvas = document.createElement("canvas");
      canvas.id = "pulp-ui-canvas";
      // Canvas box: see CANVAS_ASPECT in this assembler. Do not tighten it here.
      // A FIXED height, not an aspect ratio.
      //
      // The plugin's content height does not depend on its width: 20px of padding, a 26px
      // title, a 16px gap, and one 128px row of controls. 210px, at every width. An
      // aspect-ratio makes the box a function of the WIDTH instead — so it left a dead band
      // under the knobs on a desktop and collapsed to ~100px on a phone (where the view
      // then sheared its own labels through each other). Neither was a tuning problem; the
      // measurement was simply of the wrong thing.
      canvas.style.cssText = "width:100%;height:clamp(420px,60vh,680px);display:block";
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
  <style>
    /* The height the shell reserves for the editor while its multi-MB wasm loads.
       Keep it in step with the canvas's 8:3 box (see the customUi factory above):
       reserve MORE and a gap opens under the placeholder, reserve LESS and the
       panel still jumps when the editor arrives. */
    :root{--pw-customui-h:210px}
  </style>
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
    // The player renders this INSIDE the panel, directly under the controls: loading
    // an impulse response is part of the plugin, not page furniture — mounted after
    // the panel it read as unrelated and was easy to miss entirely.
    fileUpload: irFileUpload(),
  });
</script>
<p style="max-width:640px;margin:0 auto;padding:0 20px 40px;
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
    Both pages mount the same player; only the ABI underneath differs.
    <div style="margin-top:6px">
      The editor is GPU-rendered. The DSP is not — this build runs the CPU convolution engine, the
      same one the desktop uses by default.
    </div>
    <div style="margin-top:6px"><a href="${SC_SRC}">Source on GitHub &nearr;</a></div>
  </footer>
</div>
</body>
</html>
`;
}

// ── SuperConvolver GPU: a SEPARATE page, deliberately. ─────────────────────
// /super-convolver/{wam,wclap}/ are the shipped demo and stay byte-identical —
// the GPU engine ships at /super-convolver-gpu/ (WebCLAP only; a WAM module has
// no worker seam to reach WebGPU through) so a browser that cannot run WebGPU
// compute can never regress the demo that works everywhere.
//
// The page runs the capability handshake BEFORE it creates the adapter, and
// loads the GPU-capable module only when the handshake says ok. When it fails
// the page loads the ORDINARY CPU module, names the reason, and does not render
// an Engine toggle at all: an inert toggle is exactly the "inert and
// misleading" control the PULP_WASM compile-out already removed once.
const GPU_SRC = resolve(REPO, "examples/web-demos/gpu-audio/js");
// The browser seam (examples/web-demos/gpu-audio/js): gpu-bridge.mjs runs on the
// page (probe + bring-up + stats), gpu-worker.mjs is the DedicatedWorker that
// owns the WebGPU device — an AudioWorkletGlobalScope can neither touch
// navigator.gpu nor spawn a Worker, so the worker is not optional — and
// gpu-ring.mjs is the SharedArrayBuffer both of them (and the worklet) map. All
// three must be same-origin, hence the copy into the page dir.
const GPU_BRIDGE_FILES = ["gpu-bridge.mjs", "gpu-worker.mjs", "gpu-ring.mjs"];
// The Skia-free WebGPU DSP module the worker instantiates (examples/web-demos/
// gpu-audio, target pulp-gpu-dsp). Built with emcmake into its own tree.
const GPU_DSP_BUILD = resolve(HERE, arg("--gpu-build", "../../gpu-audio/build-gpu-dsp"));
const GPU_DSP_FILES = ["pulp-gpu-dsp.js", "pulp-gpu-dsp.wasm"];
const GPU_TARGET = "SuperConvolverGpu";       // super_convolver_gpu_wclap

// *** THE PAGE IS NOT EMITTED WHILE THIS IS FALSE. ***
//
// The GPU worker convolves with the IR the PAGE hands it, and that IR must be the one the
// PLUGIN is actually using — post-normalize, post-window — or the two engines are
// different reverbs and the CPU convolver stops being a sample-for-sample substitute for
// a block the GPU missed. "The safety net" would really be a second effect cutting in.
//
// That handoff is WIRED (it was not, and this page was withheld until it was): the WebCLAP
// module exports its live IR and a generation counter (pulp_ir_* in
// wclap-build/plugins/super_convolver_gpu_wclap.cpp), the worklet polls it on the
// non-realtime tick, the adapter latches and re-emits it (onIrChanged), and the page
// forwards it with lane.setIr(). It re-fires on every rebuild — a Size move, an uploaded
// impulse — so the engines cannot drift apart.
//
// This flag stays as the switch it always was. Set it back to false the moment the page
// could no longer honestly claim a running WebGPU shader: the title, <meta description>,
// OG card and gallery link all assert that in the PRESENT TENSE, and a page that serves
// 100 % CPU to every visitor while saying so is a capability lie — one displaced from the
// toggle (which correctly refuses to render) onto the chrome and onto a link someone
// pastes into Slack. browser-test/gallery-smoke.mjs holds the static half of that
// contract; browser-test/page-gpu.mjs drives the assembled page in a real browser and
// asserts the GPU is producing blocks on it.
const GPU_IR_HANDOFF_WIRED = true;

const GPU_TITLE = "SuperConvolver — GPU engine";
// Short on purpose. "Not faster than the CPU" is NOT optional — it is the one claim this
// page must not overstate — but it fits in a clause, not a paragraph. The long version
// pushed the plugin below the fold on a phone.
// Say what the page IS. "The same reverb" meant nothing to anyone who had not just read the
// other page — it is a comparison against something they cannot see. The point is: one
// plugin, two engines, pick either and hear that they match. And "not faster than the CPU"
// stays, because it is the one claim this page must not overstate.
const GPU_SUBTITLE =
  "A convolution reverb that can run its maths on your CPU or, as a WebGPU compute shader, " +
  "on your GPU. Switch between them and hear that they sound the same. The GPU is not " +
  "faster — it is just a different place to run it.";

function superConvolverGpuPage(pageUrl, hasOgImage, v) {
  return `<!doctype html>
<html lang="en" data-theme="dark">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <title>${esc(GPU_TITLE)} — Pulp WebCLAP demo</title>
  <meta name="description" content="${attr(GPU_SUBTITLE)}">
  <meta name="pulp:source" content="${attr(SC_SRC)}">
  <meta property="og:type" content="website">
  <meta property="og:site_name" content="Pulp">
  <meta property="og:title" content="${attr(GPU_TITLE + " — Pulp WebCLAP demo")}">
  <meta property="og:description" content="${attr(GPU_SUBTITLE)}">
${ogUrlAndImage(pageUrl, hasOgImage)}
  <link rel="icon" href="data:,">
  <style>
    .engine-row{max-width:640px;margin:0 auto 10px;display:flex;align-items:center;gap:10px;
                font:13px/1.5 system-ui;color:#cdd6e3}
    .engine-row select{background:#0c1116;color:#cdd6e3;border:1px solid #2a3340;border-radius:6px;
                       padding:4px 8px;font:13px system-ui}
    .engine-note{max-width:640px;margin:0 auto 14px;font:12.5px/1.6 system-ui;color:#8b96a3}
    /* Fixed-width slots + tabular figures: the labels and separators never move, only the
       digits inside each box change. A concatenated sentence re-flows on every tick. */
    .engine-stats{max-width:640px;margin:0 auto 10px;font:12.5px/1.7 system-ui;color:#8b96a3}
    .engine-stats .es-num{display:inline-block;text-align:right;color:#c9d1d9;
                          font-variant-numeric:tabular-nums;min-width:5.5ch}
    .engine-stats .es-pct{min-width:5ch}
    .engine-stats .es-eng{display:inline-block;min-width:3.5ch;color:#2bd4be;font-weight:600}
    .engine-stats .es-sep{opacity:.55}
    .engine-off{max-width:960px;margin:0 auto 14px;font:13px/1.6 system-ui;color:#e0b070}
    /* ── Hero layout ──────────────────────────────────────────────────────────────────
       The editor is a beautiful thing and demands the room; the default 640px player panel
       boxes it in. Widen the player's whole column (and the stats that sit under the editor)
       to a wide, centered, padded band, and give the editor canvas a tall aspect. Everything
       stays centered with breathing room — full-bleed would touch the window edges. */
    /* #panel.pulp is the player's centered column, capped at 640px by default. Widen it and
       everything inside (the editor canvas fills its width) to a wide, padded band. */
    #panel.pulp{max-width:min(1200px, 94vw) !important}
    /* The live readout sits in a FIXED slot between the editor and the scope.
       It ALWAYS reserves two lines' worth of height and centres its content, so
       nothing bumps: not when the metrics wrap to a second line on a narrow phone,
       and not when the engine flips CPU<->GPU (short CPU line vs. long GPU line).
       The text WRAPS — never truncates — so a phone that can't fit the sentence on
       one line shows all of it across two, inside the already-reserved box. */
    .engine-stats{max-width:min(1200px, 94vw) !important; margin:12px auto 2px;
                  font-size:13px; line-height:1.5; text-align:center;
                  min-height:3.2em; display:grid; place-items:center}
    .engine-stats #es-cpu{color:#8b96a3}
    #pulp-ui-canvas{height:clamp(420px, 60vh, 680px) !important}
  </style>
</head>
<body>
<div id="app"></div>
<script type="module">
  import { mountDemo } from "../vendor-player/shell.js?v=${v}";
  import { createWclapAdapter } from "../vendor-player/adapters/wclap.js?v=${v}";
  import { mountPulpUi } from "./pulp-ui.js?v=${v}";
  import { probe, startGpuLane } from "./gpu-bridge.mjs";
  import { irFileUpload } from "./ir-source.js?v=${v}";
  // The plugin-state container is how the editor's own IR loader (the "SOURCE" chip) writes a
  // chosen impulse response WITHOUT resetting the knobs: it swaps only the plugin-owned IR blob
  // inside the state, leaving the parameter bytes intact. Passing it to mountPulpUi is what
  // wires the chip's file dialog at all — without it Module.onRequestIr is never set.
  import * as pluginState from "../vendor-player/state/plugin-state.js?v=${v}";

  // Both constants are the plugin's, restated here because the ring is allocated
  // on this side: kInternalBlock and kWebGpuLatencyBlocks in
  // examples/super-convolver/super_convolver.hpp. One contract, two languages —
  // a mismatch is a latency bug, not a rounding error.
  const INTERNAL_BLOCK = 512;
  const GPU_LATENCY_BLOCKS = 2;

  // ── The handshake, BEFORE the adapter is created. ───────────────────────
  // Cheap main-thread preconditions first (SAB + cross-origin isolation), then
  // the REAL bring-up in the worker: adapter -> device -> shaders -> pipelines ->
  // a unit-impulse self-test. Only an "ok" from all of that loads the GPU-capable
  // module. A page that loaded it and then missed every block would just be a
  // worse CPU demo with a lying toggle on top.
  let lane = null;
  let handshake = probe();

  // The IR is the PLUGIN's, and the worker is what convolves with it. Without it the
  // GPU lane would still be convolving with the unit impulse it was self-tested with
  // — i.e. passing the dry signal through — which is a GPU engine that appears to work
  // and does nothing.
  //
  // The IR cannot be supplied HERE, though, and that is not a defect: the plugin does
  // not exist yet. It is instantiated inside the worklet by createAdapter() below, and
  // only then does it build its IR. So the lane starts with no kernel, the plugin
  // publishes its IR when it has one (pulp_ir_* exports → worklet → adapter), and the
  // page forwards it with lane.setIr(). Until that lands, the Engine toggle is not
  // rendered — see irReady.
  if (handshake.ok) {
    lane = await startGpuLane({
      workerUrl: "./gpu-worker.mjs",
      moduleUrl: "./pulp-gpu-dsp.js",
      ir: null,
      sampleRate: 48000,
      blockSize: INTERNAL_BLOCK,
      latencyBlocks: GPU_LATENCY_BLOCKS,
      onDeviceLost: (info) => {
        // Normal behavior, not a fatal error: the worklet misses from here on
        // and the plugin's CPU convolver covers every block.
        console.warn("WebGPU device lost:", info && info.reason);
      },
    });
    handshake = lane.ok ? lane : { ok: false, reason: lane.reason };
  }
  const gpuOk = !!(handshake && handshake.ok);
  window.__gpuProbe = gpuOk
    ? { ok: true, adapterInfo: lane.adapterInfo, features: lane.features,
        limits: lane.limits, timestampQuery: lane.timestampQuery }
    : { ok: false, reason: handshake.reason };

  const info = gpuOk ? (lane.adapterInfo || {}) : {};
  const backend = [info.vendor, info.architecture].filter(Boolean).join(" / ");

  let audioContext = null;
  let engineIsGpu = false;   // is the GPU the engine that is EMITTING? (see the status poll)

  // For the READOUT only. Whether the GPU does any work is decided by the plugin's Engine
  // parameter, on the audio thread, inside the worklet — deliberately NOT here. A page that
  // forgot to tell the worker would leave the GPU engine silent, and the first version of this
  // was wired page-side and did exactly that to the engine-proof fixture.
  const setEngineGpu = (on) => { engineIsGpu = !!on; };

  mountDemo({
    root: document.getElementById("app"),
    title: "${esc(GPU_TITLE)}",
    subtitle: "${attr(GPU_SUBTITLE)}",
    hostLabel: "WebCLAP",
    hostDocsHref: "https://github.com/free-audio/clap",
    galleryHref: "../super-convolver/index.html",
    sourceHref: "${attr(SC_SRC)}",
    dspUrl: gpuOk ? "./${GPU_TARGET}.wasm" : "./SuperConvolver.wasm",
    processorUrl: "${WORKLET}",
${Object.entries(SC_CFG).map(([k, val]) => `    ${k}: ${JSON.stringify(val)},`).join("\n")}
    customUi: (container, adapter) => {
      // The Engine control is DOM, not a knob: it is a page-level engine switch.
      // Where the handshake failed it is NOT RENDERED AT ALL — an inert control
      // is exactly the "inert and misleading" failure the PULP_WASM compile-out
      // already fixed once. It is also gated on descriptor.gpuLane, which is true
      // only when the worklet really attached the ring.
      // The plugin's live IR, forwarded to the worker. Assigning the handler also
      // REPLAYS the latest IR if the plugin already published one (the adapter latches
      // it) — which it has, because the first IR is built while the plugin activates,
      // before this custom UI is ever mounted. Without that replay the GPU would sit
      // with no kernel until the user happened to move Size.
      //
      // This fires again on every rebuild — a Size move, an uploaded impulse — so the
      // two engines never drift apart onto different reverbs.
      const ringAttached = gpuOk && !!(adapter.descriptor && adapter.descriptor.gpuLane);
      let laneAttached = false;   // ring attached AND the worker has the plugin's kernel

      // Built when the FIRST IR reaches the worker — not before. Two reasons it cannot
      // just be rendered up front and gated later:
      //
      //   • A toggle offered before the worker has a kernel would switch the audio to a
      //     GPU still convolving with the unit impulse it was self-tested with. That is
      //     not a degraded reverb, it is NO reverb: the effect would audibly vanish.
      //   • An inert-but-present control is the "inert and misleading" failure the
      //     PULP_WASM compile-out already fixed once. So it does not exist until it works.
      //
      // WHEN the IR arrives is not knowable here. The plugin builds it on its first
      // non-realtime tick, which needs process() to have run, which needs the user to
      // have started audio — the adapter may already have it (it latches, and assigning
      // the handler below replays it synchronously) or it may be seconds away. Both
      // paths land here.
      const mountEngineToggle = () => {
        if (laneAttached) return;      // already up
        laneAttached = true;
        // The GPU lane is live. Swap the transient "starting…" line for the steady CPU
        // note; the poll loop replaces it with the GPU metrics whenever the shader is the
        // emitting engine. NOTHING is added or removed here — the stats slot was reserved
        // at mount below — so the scope below the editor never jumps on the flip.
        const cpu = document.getElementById("es-cpu");
        if (cpu) cpu.textContent = "Convolving on the CPU.";
      };

      // The CPU-side status text (rendered into the reserved #es-cpu slot below the
      // editor). If the GPU lane never arrives this is what the line keeps saying — a
      // named reason, never a generic "unavailable". Once the lane attaches,
      // mountEngineToggle() swaps it for the steady CPU note.
      const cpuStatusText = !gpuOk
        ? "GPU unavailable (" + handshake.reason + ") — convolving on the CPU."
        : !ringAttached
          ? "GPU unavailable (no-gpu-lane-in-worklet) — convolving on the CPU."
          : "Starting the GPU engine — convolving on the CPU meanwhile.";

      // The plugin can move Engine without going through the control above (a preset, a
      // host automation lane). Follow it, or the status line and the audio disagree — which
      // is the same lie, just arrived at from the other side.
      const prevParamsChanged = adapter.onParamsChanged;
      adapter.onParamsChanged = (values, infos) => {
        try {
          const list = infos || [];
          // POSITIONAL, not keyed by id. The adapter builds this array by mapping over
          // paramInfo, so values[i] belongs to list[i] — indexing it by the parameter's ID
          // reads whatever parameter happens to sit at that position (Engine's id is 5, so
          // values[e.id] was the SIXTH parameter's value). It looked like it worked only
          // because the select path also sets this directly.
          const i = list.findIndex((x) => /^engine$/i.test(x.label || ""));
          if (i >= 0 && values && typeof values[i] === "number") {
            setEngineGpu(values[i] >= 0.5);
          }
        } catch (err) { /* a readout must never take the audio down */ }
        if (prevParamsChanged) prevParamsChanged(values, infos);
      };

      // The plugin's live IR → the worker. Assigning this handler REPLAYS the latest IR
      // if the plugin already published one (the adapter latches it), so a handler that
      // arrives late is not punished for it. It fires again on every rebuild — a Size
      // move, an uploaded impulse — so the two engines can never drift onto different
      // reverbs, which is the one thing that would stop the CPU net from being a
      // sample-for-sample substitute for a missed GPU block.
      if (ringAttached) {
        adapter.onIrChanged = (ir) => {
          if (!lane || !lane.ok) return;
          if (lane.setIr(ir)) mountEngineToggle();
        };
      }

      const canvas = document.createElement("canvas");
      canvas.id = "pulp-ui-canvas";
      // Same box as the CPU pages (see CANVAS_ASPECT). 8/5 left a third of the panel as
      // dead space between the last knob and the Engine control below it.
      // A FIXED height, not an aspect ratio.
      //
      // The plugin's content height does not depend on its width: 20px of padding, a 26px
      // title, a 16px gap, and one 128px row of controls. 210px, at every width. An
      // aspect-ratio makes the box a function of the WIDTH instead — so it left a dead band
      // under the knobs on a desktop and collapsed to ~100px on a phone (where the view
      // then sheared its own labels through each other). Neither was a tuning problem; the
      // measurement was simply of the wrong thing.
      canvas.style.cssText = "width:100%;height:clamp(420px,60vh,680px);display:block";
      container.appendChild(canvas);

      // The live readout, in its OWN fixed-height slot directly below the editor and
      // above the shell's waveform scope. Built once and ALWAYS present, so flipping
      // CPU<->GPU only swaps which child span is visible — the slot's height never
      // changes and the scope below never moves. Spans, not a sentence: each number
      // has a fixed-width tabular slot, so only the digits change (a concatenated
      // sentence re-flows every tick and, at phone width, rewraps several times a
      // second). It proves the GPU is really carrying the audio — the one thing the
      // editor's own chrome cannot show.
      const stats = document.createElement("div");
      stats.className = "engine-stats";
      stats.innerHTML =
        '<span id="es-cpu"></span>' +
        '<span id="es-gpu" hidden>' +
          '<span class="es-num" id="es-blocks">—</span><span class="es-lab"> blocks on the GPU</span>' +
          '<span class="es-sep"> · </span>' +
          '<span class="es-num" id="es-covered">—</span><span class="es-lab"> covered by the CPU</span>' +
          '<span class="es-sep"> · </span>' +
          '<span class="es-num" id="es-us">—</span><span class="es-lab"> µs/block</span>' +
          '<span class="es-sep"> · </span>' +
          '<span class="es-num es-pct" id="es-pct">—</span><span class="es-lab"> of the real-time budget</span>' +
          // The honest GPU-compute cost — the shader's own execution time, read from a WebGPU
          // timestamp query, distinct from the µs/block above (which includes cross-thread
          // transport). Stays hidden until the async timestamp readback populates it, so a
          // build without that core simply omits it rather than showing a placeholder.
          '<span id="es-gpumath-wrap" hidden><span class="es-sep"> · </span>' +
            '<span class="es-num" id="es-gpumath">—</span><span class="es-lab"> µs GPU math</span></span>' +
        '</span>';
      container.appendChild(stats);
      document.getElementById("es-cpu").textContent = cpuStatusText;

      // Engine and "GPU only" are rendered BELOW as a <select> and a checkbox — real
      // controls with words on them. Drawing them again as knobs would be two controls for
      // one parameter, and a continuous dial is the wrong shape for a switch anyway.
      const pending = mountPulpUi(canvas, adapter, {
        moduleUrl: "./${UI_MODULE}.js",
        hideParams: ["Engine", "GPU only"],
        // Wire the editor's own "SOURCE" chip as the IR loader (its file dialog).
        pluginState,
      });
      pending.catch((err) => { console.warn("Pulp UI failed to mount:", err); });

      // 10 Hz, on a setInterval and NOT requestAnimationFrame: a backgrounded tab
      // throttles rAF, and that is exactly when GPU deadlines are missed — the
      // readout must not freeze at the moment it gets interesting.
      let timer = 0;
      let lastProduced = 0;
      let gpuStall = 0;          // consecutive 100ms polls with no new GPU block
      let gpuCarrying = false;   // debounced "GPU is the emitting engine" state
      pending.then((ui) => {
        if (!ui) return;
        timer = setInterval(() => {
          const stats = laneAttached && lane ? lane.pollStats() : null;
          // No lane, no stats — and nothing to say. The engine is named by the <select>
          // right below the canvas, which is also the control that changes it; the plugin's
          // view tree carries no status line to update (a label whose text changes on a
          // 10 Hz timer is a layout event on a 10 Hz timer, and at phone width it sheared
          // through the knob labels).
          if (!stats) return;
          // "Is the GPU carrying the audio RIGHT NOW" — a RECENT-WINDOW test, not a
          // cumulative one. A "produced > 0" test would latch on the first block made
          // and then read "Engine: GPU" forever, including while a backgrounded tab
          // or a lost device is missing every deadline and the CPU net is producing
          // 100 % of what you hear.
          const producedNow = (stats.produced || 0) > lastProduced;
          lastProduced = stats.produced || 0;
          // HYSTERESIS. producedNow is a single 100ms window, and on mobile Safari the
          // worker's block cadence and this timer drift out of phase — so an isolated
          // poll lands with no NEW block even while the GPU is steadily carrying. Taken
          // raw, that strobed the readout back to "Convolving on the CPU" for one tick.
          // So: flip to GPU the instant a block lands, but require ~0.8s of continuous
          // silence (8 polls) before conceding back to CPU. A real engine switch stalls
          // the counter forever and still crosses that threshold; a phase blip never does.
          if (producedNow) { gpuStall = 0; gpuCarrying = true; }
          else if (++gpuStall >= 8) { gpuCarrying = false; }

          // AND the engine must actually BE the GPU. The worker keeps turning under
          // Engine=CPU — deliberately: the plugin drives the ring on every block so the
          // shared block timeline stays aligned, and a ring that idled through a CPU
          // stretch would hand the first pops after a flip back to GPU a pile of stale
          // audio. Those blocks are produced and then DISCARDED.
          //
          // So "blocks are advancing" is not the question. It reported "Engine: GPU —
          // 10,425 blocks on GPU" while the selector said CPU and not one sample of what
          // you were hearing came from the shader. The readout must describe the audio
          // that is being EMITTED, not the work that is being thrown away — a status line
          // that overstates the GPU is the exact failure this whole demo exists to avoid.
          // producedNow ALONE is the honest signal now. The worklet's engine gate keeps the
          // GPU worker fully IDLE while Engine=CPU (it submits nothing), so the worker produces
          // blocks ONLY when GPU is the emitting engine. The old engineIsGpu-AND-producedNow
          // test relied on a page-side engineIsGpu flag that the removed dropdown used to set;
          // the editor's own CPU/GPU chip is the switcher now, and the GPU's own produced
          // counter is a truer "is the shader carrying the audio" than any page param mirror.
          const gpuIsCarrying = gpuCarrying;
          // budget_us and rt_percent are derived HERE, by the same arithmetic
          // native gpu_status() uses (super_convolver.hpp), so the browser and
          // the native build print the same numbers computed the same way.
          const sampleRate = (audioContext && audioContext.sampleRate) || 48000;
          const budget_us = (INTERNAL_BLOCK / sampleRate) * 1e6;
          const avg_us = stats.avgBlockUs || 0;
          // Nothing to write into the canvas: the plugin no longer carries a status line.
          // The Engine <select> below says which engine is selected, and these DOM slots say
          // what it is doing.

          const pct = budget_us > 0 ? (avg_us / budget_us) * 100 : 0;
          const put = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };
          const show = (id, on) => { const e = document.getElementById(id); if (e) e.hidden = !on; };

          // Show the GPU figures ONLY while the GPU is the engine that is emitting. The worker
          // keeps producing blocks under Engine=CPU and they are all discarded; printing them
          // there describes work you cannot hear, beside a control that says CPU.
          show("es-cpu", !gpuIsCarrying);
          show("es-gpu", gpuIsCarrying);
          if (gpuIsCarrying) {
            put("es-blocks", (stats.produced || 0).toLocaleString());
            put("es-covered", (stats.miss || 0).toLocaleString());
            put("es-us", Math.round(avg_us).toLocaleString());
            put("es-pct", pct.toFixed(1) + "%");
            // Only present when the async timestamp path reported a non-zero shader
            // time (Metal/Chrome can quantize a fast pass to 0 ns). gpuNsLast is the
            // GPU-busy span in ns from pulp_gpu_stat(4); show it in µs beside the
            // round-trip so the two costs are honestly distinct.
            const gpuMathUs = (stats.gpuNsLast || 0) / 1000;
            show("es-gpumath-wrap", gpuMathUs > 0);
            if (gpuMathUs > 0)
                put("es-gpumath", (gpuMathUs >= 10 ? Math.round(gpuMathUs)
                                                   : gpuMathUs.toFixed(1)).toLocaleString());
          }
          window.__gpuStats = stats;
        }, 100);
      }, () => {});

      return {
        ready: pending,
        destroy: () => {
          if (timer) clearInterval(timer);
          if (lane && lane.ok) lane.shutdown();
          pending.then((ui) => ui && ui.destroy(), () => {});
        },
      };
    },
    createAdapter: async (ctx, urls) => {
      audioContext = ctx;
      // The SAB is handed to the worklet at construction; it is the only way the
      // audio thread can reach the worker that owns the WebGPU device.
      return createWclapAdapter(ctx, urls, gpuOk
        ? { gpuSab: lane.sab, gpuLatencyBlocks: lane.latencyBlocks }
        : {});
    },
    // NO separate upload area on the GPU page: the fancy editor's own "SOURCE" chip opens the
    // file dialog (see the pluginState wiring above), and it does the full live thing — the
    // plugin republishes its rebuilt IR (pulp_ir_*), the page forwards it with lane.setIr(),
    // the GPU worker re-prepares — so a second page-level loader would just duplicate the chip.
  });
</script>
<p style="max-width:640px;margin:0 auto;padding:0 20px 40px;
          font:13px/1.6 system-ui;color:#8b96a3">
  The CPU-only builds of the same plugin are at
  <a href="../super-convolver/" style="color:#2bd4be">SuperConvolver (WAM &amp; WebCLAP) &rarr;</a>.
</p>
</body>
</html>
`;
}

// The Pulp UI module trio (.js/.wasm/.data — the .data carries icudtl.dat, which
// SkUnicode needs or every Label shapes to zero width).
const UI_FILES = [`${UI_MODULE}.js`, `${UI_MODULE}.wasm`, `${UI_MODULE}.data`];


// Copy one Pulp-UI file into a page dir, PRE-COMPRESSING the .data MEMFS image.
//
// _headers declares `Content-Encoding: br` for every .data (Cloudflare does not auto-compress
// application/octet-stream, and this is the largest asset on the site). So the bytes on disk
// MUST actually be brotli. Plain-copying it ships raw bytes under a header that says brotli —
// the browser fails to decode, the UI module never loads, and the page sits on "Loading
// editor…" forever. The GPU page did exactly that, and its share card was a screenshot of
// that loading screen.
//
// One function, used by every page that carries the UI module, so the two cannot drift again.
async function copyUiFile(f, srcDir, destDir) {
  if (!f.endsWith(".data")) {
    await copyFile(join(srcDir, f), join(destDir, f));
    return;
  }
  const raw = await readFile(join(srcDir, f));
  const packed = brotliCompressSync(raw, {
    params: {
      [zlibConstants.BROTLI_PARAM_QUALITY]: 11,
      [zlibConstants.BROTLI_PARAM_SIZE_HINT]: raw.length,
    },
  });
  await writeFile(join(destDir, f), packed);
  console.log(`  ${f}  ${(raw.length / 1048576).toFixed(1)} MB → ` +
              `${(packed.length / 1048576).toFixed(1)} MB (brotli, pre-compressed)`);
}

async function emitSuperConvolverGpu() {
  if (!GPU_IR_HANDOFF_WIRED) {
    console.warn("assemble-gallery: NOT emitting super-convolver-gpu — the IR handoff " +
                 "is unwired (nothing sets window.__scGpuIr), so the page's GPU engine " +
                 "could never run and its title/description/OG would claim a WebGPU " +
                 "shader that never dispatches. See GPU_IR_HANDOFF_WIRED.");
    return 0;
  }
  const gpuWasm = [join(BUILD, `${GPU_TARGET}.wclap`, "module.wasm"), join(BUILD, `${GPU_TARGET}.wasm`)]
    .find((p) => existsSync(p));
  const cpuWasm = [join(BUILD, "SuperConvolver.wclap", "module.wasm"), join(BUILD, "SuperConvolver.wasm")]
    .find((p) => existsSync(p));
  const bridgeOk = GPU_BRIDGE_FILES.every((f) => existsSync(join(GPU_SRC, f)));
  const dspOk = GPU_DSP_FILES.every((f) => existsSync(join(GPU_DSP_BUILD, f)));
  // The GPU page has NO generated-grid fallback: its status line IS a Pulp UI
  // Label, so without the UI module there is nothing to report into.
  const withUi = UI_FILES.every((f) => existsSync(join(UI_BUILD, f)));
  if (!gpuWasm || !cpuWasm || !withUi || !bridgeOk || !dspOk) {
    console.warn("assemble-gallery: skipping super-convolver-gpu — needs " +
                 `${GPU_TARGET} + SuperConvolver in ${BUILD}, the Pulp UI module ` +
                 `(--ui-build), ${GPU_BRIDGE_FILES.join(" + ")} in ${GPU_SRC}, and ` +
                 `${GPU_DSP_FILES.join(" + ")} in ${GPU_DSP_BUILD} (--gpu-build).`);
    return 0;
  }
  const v = await playerVersion();
  const dir = join(OUT, "super-convolver-gpu");
  await mkdir(dir, { recursive: true });
  await writeFile(join(dir, "index.html"),
                  superConvolverGpuPage(`${SITE_BASE}/super-convolver-gpu/`,
                                        existsSync(join(dir, "og.png")), v));
  await copyFile(gpuWasm, join(dir, `${GPU_TARGET}.wasm`));
  await copyFile(cpuWasm, join(dir, "SuperConvolver.wasm"));
  for (const f of UI_FILES) await copyUiFile(f, UI_BUILD, dir);
  await copyFile(join(UI_SRC, "pulp-ui.js"), join(dir, "pulp-ui.js"));
  // The GPU page offers the same IR uploader as the CPU pages, and pulp-ui.js's sibling
  // import of it is relative — so it has to live in THIS directory too, not just the
  // super-convolver one. Without it the page 404s on ir-source.js and never mounts at all.
  await copyFile(join(UI_SRC, "ir-source.js"), join(dir, "ir-source.js"));
  for (const f of GPU_BRIDGE_FILES) await copyFile(join(GPU_SRC, f), join(dir, f));
  for (const f of GPU_DSP_FILES) await copyFile(join(GPU_DSP_BUILD, f), join(dir, f));
  console.log("  super-convolver-gpu/**     (WebGPU compute engine offered; CPU is the default " +
              "engine and the always-live fallback)");
  return 1;
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
  const uiFiles = UI_FILES;
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
    // The IR loader is imported by BOTH page variants (it hangs off the shell's
    // onReady seam, not customUi), so it ships whether or not the Pulp UI module
    // built — unlike pulp-ui.js, which is only imported when the module is there.
    await copyFile(join(UI_SRC, "ir-source.js"), join(pdir, "ir-source.js"));
    if (withUi) {
      for (const f of uiFiles) {
        await copyUiFile(f, UI_BUILD, pdir);
      }
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
// Independent of the pair above: /super-convolver-gpu/ is WebCLAP-only, so it
// does not need the WAM build tree, and the two shipped pages do not need it.
// It is also the one page that can be SKIPPED on honesty grounds — see
// GPU_IR_HANDOFF_WIRED — so the root banner links it only when it was written.
const gpuPages = await emitSuperConvolverGpu();
wrote += gpuPages;

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
      `<a href="./super-convolver/" style="color:#2bd4be">SuperConvolver (WAM &amp; WebCLAP)</a>` +
      (gpuPages > 0
        ? ` &middot; <a href="./super-convolver-gpu/" style="color:#2bd4be">SuperConvolver (WebGPU compute engine)</a>`
        : ``) +
      ` — every WAM demo, rebuilt as a threaded WebCLAP module behind the same player.</div>`;
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
