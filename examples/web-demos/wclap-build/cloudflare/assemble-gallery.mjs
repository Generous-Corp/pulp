// Assemble the WebCLAP 1:1 demo gallery into the Cloudflare Pages deploy dir.
//
// A WebCLAP version of EVERY plugin that already has a WAM demo, so this site
// mirrors both existing WAM galleries one-for-one — same rows, same order, same
// widgets, same copy — only the ABI underneath differs (a threaded WebCLAP
// .wasm hosted in a worklet-resident CLAP host, vs a WAM DSP module). Emits:
//
//   public/vendor-player/**              — ONE shared copy of @pulp/web-player
//                                          (shell, adapters/wclap.js, widgets,
//                                          theme, vendor/pulp-wasm worklet).
//   public/example-plugins/index.html    — gallery mirroring the WAM example
//                                          gallery (8 cards).
//   public/example-plugins/<slug>/       — per-plugin shared-player page +
//                                          <Target>.wasm  (8 dirs)
//   public/classic-effects/index.html    — gallery mirroring the WAM classic
//                                          effects gallery (15 cards).
//   public/classic-effects/<slug>/       — per-plugin page + <Target>.wasm (15)
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

// A per-plugin shared-player page: the SAME shell as the WAM demos, WebCLAP adapter.
function pluginPage(section, p) {
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
  <link rel="icon" href="data:,">
</head>
<body>
<div id="app"></div>
<script type="module">
  // The SAME shared player (@pulp/web-player) the WAM demos mount — imported
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
function galleryPage(section) {
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

// ── 1. shared @pulp/web-player copy (once). ─────────────────────────────────
await mkdir(OUT, { recursive: true });
await cp(PKG_SRC, join(OUT, "vendor-player"), { recursive: true });

// _headers if run standalone.
if (!existsSync(join(OUT, "_headers"))) await copyFile(join(HERE, "_headers"), join(OUT, "_headers"));

// ── 2. sections: gallery index + per-plugin pages + wasm. ───────────────────
let wrote = 0, wasmCopied = 0;
for (const section of SECTIONS) {
  const sectionDir = join(OUT, section.slug);
  await mkdir(sectionDir, { recursive: true });
  await writeFile(join(sectionDir, "index.html"), galleryPage(section));
  wrote++;
  for (const p of section.plugins) {
    const pdir = join(sectionDir, p.dir);
    await mkdir(pdir, { recursive: true });
    await writeFile(join(pdir, "index.html"), pluginPage(section, p));
    wrote++;
    const wasm = join(BUILD, `${p.target}.wclap`, "module.wasm");
    const wasmFlat = join(BUILD, `${p.target}.wasm`);
    const wasmSrc = existsSync(wasm) ? wasm : wasmFlat;
    if (!existsSync(wasmSrc)) die(`missing wasm for ${section.slug}/${p.dir} (${p.target}); build it first`);
    await copyFile(wasmSrc, join(pdir, `${p.target}.wasm`));
    wasmCopied++;
  }
}

// ── 3. surface both galleries from the generated root isolation-proof page. ──
const rootIndex = join(OUT, "index.html");
if (existsSync(rootIndex)) {
  let html = await readFile(rootIndex, "utf8");
  if (!html.includes('href="./example-plugins/"') && html.includes("<body>")) {
    const banner = `<body>\n  <div style="max-width:900px;margin:12px auto;padding:10px 14px;` +
      `border:1px solid #2bd4be;border-radius:8px;font:14px/1.5 system-ui;background:#0c1116;color:#cde">` +
      `<strong>WebCLAP demo gallery →</strong> ` +
      `<a href="./example-plugins/" style="color:#2bd4be">Example Plugins (8)</a> &middot; ` +
      `<a href="./classic-effects/" style="color:#2bd4be">Classic Effects (15)</a> ` +
      `— every WAM demo, rebuilt as a threaded WebCLAP module behind the same player.</div>`;
    html = html.replace("<body>", banner);
    await writeFile(rootIndex, html);
    console.log("  index.html                 (+ gallery banner → example-plugins / classic-effects)");
  }
}

const rel = (p) => p.replace(REPO + "/", "");
console.log("assemble-gallery: wrote WebCLAP 1:1 demo gallery → " + rel(OUT));
console.log(`  vendor-player/**           (shared shell + wclap adapter + worklet)`);
console.log(`  ${wrote} HTML pages          (2 galleries + 23 per-plugin shared-player pages)`);
console.log(`  ${wasmCopied} WebCLAP modules     (one .wasm per plugin)`);
