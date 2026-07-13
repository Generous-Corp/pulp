#!/usr/bin/env node
// pulp-web-demo — deterministic generator.
// config (.pulp-web-demo/config.json) → stamped WAM+WCLAP demo site + owned-files manifest.
// No network at generation time, no Date.now()/random — identical config ⇒ identical output.
//
//   node generate.mjs --config <path> --out <dir> [--force] [--check]
//
// The smarts live in the pinned shared player; this only wires + stamps versioned templates.

import { readFileSync, writeFileSync, mkdirSync, existsSync, readdirSync, copyFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";

const GENERATOR_VERSION = "1.0.0";
const SCHEMA_MAJOR = 1;
const HERE = dirname(fileURLToPath(import.meta.url));
const T = (p) => readFileSync(join(HERE, "templates", p), "utf8");
const sha8 = (s) => createHash("sha256").update(s).digest("hex").slice(0, 8);

// ---- args ----
const args = process.argv.slice(2);
const opt = (k, d = null) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const has = (k) => args.includes(k);
const configPath = resolve(opt("--config", ".pulp-web-demo/config.json"));
const outDir = resolve(opt("--out", "docs"));
const check = has("--check");

// ---- load + validate config (critical invariants; full schema lives in validate.mjs) ----
function fail(msg) { console.error(`pulp-web-demo: ${msg}`); process.exit(1); }
if (!existsSync(configPath)) fail(`config not found: ${configPath}`);
let cfg;
try { cfg = JSON.parse(readFileSync(configPath, "utf8")); } catch (e) { fail(`config is not valid JSON: ${e.message}`); }

const ALLOWED_TOP = new Set(["schemaVersion", "player", "theme", "deploy", "plugins", "gallery", "metadata", "modules"]);
for (const k of Object.keys(cfg)) if (!ALLOWED_TOP.has(k)) fail(`unknown config key: "${k}" (unknown keys are rejected)`);
if (cfg.schemaVersion !== SCHEMA_MAJOR) fail(`schemaVersion ${cfg.schemaVersion} != generator major ${SCHEMA_MAJOR}`);
if (!cfg.player?.package || !cfg.player?.version) fail(`player.package and player.version are required (pin the player — no ranges)`);
if (!Array.isArray(cfg.plugins) || cfg.plugins.length === 0) fail(`plugins[] must have at least one entry`);
console.error(`pulp-web-demo: config OK → ${configPath}`);

// ---- derived, deterministic ----
const player = cfg.player;
const playerHash = sha8(`${player.package}@${player.version}`);            // cache-bust, deterministic
const playerUrl = player.importBase
  ? player.importBase
  : `https://esm.sh/${player.package}@${player.version}`;                  // COEP-friendly CDN, pinned
const theme = cfg.theme || {};
const tokensHref = theme.tokensHref || "./theme/tokens.css";
const fontsHref = theme.fontHref || "./theme/fonts.css";
const themeMode = theme.mode || "auto";
const meta = cfg.metadata || {};
const siteBase = (meta.siteBase || "").replace(/\/$/, "");

const owned = [];  // { path, sha256 } for the regeneration-ownership manifest

function emit(relPath, contents) {
  const abs = join(outDir, relPath);
  if (!check) { mkdirSync(dirname(abs), { recursive: true }); writeFileSync(abs, contents); }
  owned.push({ path: relPath, sha256: createHash("sha256").update(contents).digest("hex") });
  console.error(`  ${check ? "would write" : "wrote"}  ${relPath}`);
}

// minimal, escaping-free mustache: {{KEY}} → vars[KEY] (string). Missing keys throw.
function fill(tmpl, vars) {
  return tmpl.replace(/\{\{([A-Z0-9_]+)\}\}/g, (_, k) => {
    if (!(k in vars)) throw new Error(`template var not provided: ${k}`);
    return String(vars[k]);
  });
}

// ---- per-plugin artifact resolution (default layout; artifactResolver seam handles the rest) ----
function artifactsFor(plugin, abi) {
  const explicit = plugin.artifacts?.[abi];
  if (explicit?.dspUrl && explicit?.processorUrl) return explicit;
  if (abi === "wam") return { dspUrl: "./wam-dsp.js", processorUrl: "./wam-processor.js" };
  // wclap: the plugin's threaded .wasm + the player-provided worklet CLAP host
  return { dspUrl: `./${plugin.id}.wasm`, processorUrl: `${playerUrl.replace(/\/$/, "")}/vendor/pulp-wasm/wclap-processor.js` };
}

const ogTag = (url) => meta.ogImageStrategy === "text" ? "" : `<meta property="og:image" content="${url}og.png" />`;

// ---- page generation ----
const wamTmpl = T("wam.index.html.tmpl");
const wclapTmpl = T("wclap.index.html.tmpl");
const coiSnippet = T("hosting/coi/coi-register.snippet.html");

const cards = [];
for (const p of cfg.plugins) {
  const abis = p.abis || ["wam", "wclap"];
  const modeLine = p.mode ? `mode: ${JSON.stringify(p.mode)},` : "";
  const paramBlock = p.paramOverrides ? `,\n    initialParams: ${JSON.stringify(p.paramOverrides)}` : "";
  const midiBlock = p.midiViz ? `,\n    midiViz: ${JSON.stringify(p.midiViz)}` : "";
  const cardAbis = [];

  for (const abi of abis) {
    const profileCfg = cfg.deploy?.[abi] || {};
    const profile = profileCfg.profile || (abi === "wam" ? "github-pages" : "cloudflare");
    const basePath = profileCfg.basePath || "/";                              // server path (isolation coverage)
    const art = artifactsFor(p, abi);
    // Canonical/OG url from this ABI's OWN public origin+base (WAM and WCLAP differ);
    // fall back to siteBase+basePath only if publicUrl is unset.
    const publicBase = (profileCfg.publicUrl || `${siteBase}${basePath}`).replace(/\/+$/, "");
    const pageUrl = `${publicBase}/${p.id}/`;
    const sourceUrl = meta.sourceLinkBase ? `${meta.sourceLinkBase.replace(/\/$/, "")}/${p.id}` : pageUrl;

    const common = {
      GENERATOR_VERSION, TITLE: p.title, SUBTITLE: p.subtitle || "",
      TITLE_JSON: JSON.stringify(p.title), SUBTITLE_JSON: JSON.stringify(p.subtitle || ""),
      MODE_LINE: modeLine, PAGE_URL: pageUrl, SOURCE_URL: sourceUrl,
      OG_IMAGE_TAG: ogTag(pageUrl),
      TOKENS_HREF: tokensHref, FONTS_HREF: fontsHref,
      TOKENS_HREF_JSON: JSON.stringify(tokensHref), FONTS_HREF_JSON: JSON.stringify(fontsHref),
      THEME_MODE_JSON: JSON.stringify(themeMode),
      PLAYER_PACKAGE: player.package, PLAYER_URL: playerUrl, PLAYER_HASH: playerHash,
      BASE_PATH: basePath,
      PARAM_OVERRIDES_BLOCK: paramBlock, MIDI_VIZ_BLOCK: midiBlock,
    };

    if (abi === "wam") {
      emit(`${p.id}/index.html`, fill(wamTmpl, {
        ...common, WAM_DSP_URL_JSON: JSON.stringify(art.dspUrl), WAM_PROCESSOR_URL_JSON: JSON.stringify(art.processorUrl),
      }));
      cardAbis.push(`<a href="./${p.id}/">WAM →</a>`);
    } else {
      const isCoi = profile === "github-pages+coi";
      const coiBlock = isCoi ? fill(coiSnippet, { BASE_PATH: basePath }) : "";
      // adapterModule seam: default to the player's bundled createWclapAdapter
      const adapterFactory = cfg.modules?.adapterModule ? cfg.modules.adapterModule : "createWclapAdapter";
      const adapterImport = cfg.modules?.adapterModule
        ? `import { ${cfg.modules.adapterModule} } from "./.pulp-web-demo/hooks.mjs";` : "";
      emit(`${p.id}/wclap/index.html`, fill(wclapTmpl, {
        ...common,
        WCLAP_PROFILE: profile,
        ISOLATION_SOURCE: isCoi ? "a coi-serviceworker mirror (FALLBACK)" : "server COOP/COEP headers",
        COI_REGISTER_BLOCK: coiBlock,
        WCLAP_DSP_URL_JSON: JSON.stringify(art.dspUrl), WCLAP_PROCESSOR_URL_JSON: JSON.stringify(art.processorUrl),
        ADAPTER_FACTORY: adapterFactory, ADAPTER_IMPORT_LINE: adapterImport,
      }));
      cardAbis.push(`<a href="./${p.id}/wclap/">WCLAP →</a>`);
    }
  }
  cards.push(`  <div class="card"><h2>${p.title}</h2><p>${p.subtitle || ""}</p><div class="abis">${cardAbis.join(" ")}</div></div>`);
}

// ---- gallery landing page ----
const gallery = cfg.gallery || {};
const crossLinks = (gallery.crossLinks || []).map((c) => `<a href="${c.href}">${c.label}</a>`).join(" · ");
emit("index.html", fill(T("gallery.index.html.tmpl"), {
  GENERATOR_VERSION,
  GALLERY_TITLE: gallery.title || "Pulp web demos",
  GALLERY_URL: (cfg.deploy?.wam?.publicUrl || `${siteBase}${(cfg.deploy?.wam?.basePath) || "/"}`).replace(/\/+$/, "") + "/",
  TOKENS_HREF: tokensHref, FONTS_HREF: fontsHref,
  LAYOUT_CLASS: `layout-${gallery.layout || "grid"}`,
  CROSS_LINKS: crossLinks, PLUGIN_CARDS: cards.join("\n"),
}));

// ---- hosting artifacts per profile ----
function outDirName(abi) { return (cfg.deploy?.[abi]?.outputDir) || "docs"; }
const wclapProfile = cfg.deploy?.wclap?.profile || "cloudflare";
const wclapBase = cfg.deploy?.wclap?.basePath || "/";
if (cfg.plugins.some((p) => (p.abis || ["wam", "wclap"]).includes("wclap"))) {
  if (wclapProfile === "cloudflare") {
    emit("_headers", fill(T("hosting/cloudflare/_headers.tmpl"), { GENERATOR_VERSION, BASE_PATH: wclapBase }));
    emit("wrangler.toml", fill(T("hosting/cloudflare/wrangler.toml.tmpl"), {
      GENERATOR_VERSION, PROJECT_NAME: cfg.deploy?.wclap?.projectName || "wclap-demos", OUTPUT_DIR: outDirName("wclap"),
    }));
  } else if (wclapProfile === "github-pages+coi") {
    emit("coi-serviceworker.js", T("hosting/coi/coi-serviceworker.js"));  // verbatim, owned
  }
}

// ---- owned-files manifest (regeneration/upgrade safety) ----
const manifest = { generator: GENERATOR_VERSION, schemaVersion: cfg.schemaVersion, player: `${player.package}@${player.version}`, files: owned.sort((a, b) => a.path.localeCompare(b.path)) };
emit(".pulp-web-demo.manifest.json", JSON.stringify(manifest, null, 2) + "\n");

console.error(`\npulp-web-demo: ${check ? "CHECK (no files written)" : `generated ${owned.length} files → ${outDir}`}`);
