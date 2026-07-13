#!/usr/bin/env node
// pulp-web-demo — validator. Exits non-zero on failure so it can BLOCK in CI.
//
//   node validate.mjs --config <path>                 # config schema + invariants
//   node validate.mjs --site <dir> --check-isolation  # WCLAP isolation covers the deployed base path
//   node validate.mjs --site <dir> --check-metadata   # OG/unfurl tags present (blocking)
//   node validate.mjs --site <dir> --check-ownership  # owned files unmodified since generation
//
// The headline risk (Codex): isolation must cover the REAL base path (e.g. /example-plugins/),
// not "/", or the page loads and only threaded init fails.

import { readFileSync, existsSync, readdirSync, statSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { createHash } from "node:crypto";

const args = process.argv.slice(2);
const opt = (k, d = null) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const has = (k) => args.includes(k);
const problems = [];
const ok = (m) => console.error(`  ok    ${m}`);
const bad = (m) => { problems.push(m); console.error(`  FAIL  ${m}`); };

function loadConfig(p) {
  if (!existsSync(p)) { bad(`config not found: ${p}`); return null; }
  try { return JSON.parse(readFileSync(p, "utf8")); } catch (e) { bad(`config not valid JSON: ${e.message}`); return null; }
}

// ---- config schema/invariants ----
function validateConfig(cfg) {
  const ALLOWED = new Set(["schemaVersion", "player", "theme", "deploy", "plugins", "gallery", "metadata", "modules"]);
  for (const k of Object.keys(cfg)) if (!ALLOWED.has(k)) bad(`unknown top-level key: "${k}"`);
  if (cfg.schemaVersion !== 1) bad(`schemaVersion must be 1`);
  if (!cfg.player?.package) bad(`player.package required`);
  if (!cfg.player?.version) bad(`player.version required (pin — no ranges)`);
  if (/[\^~*x]|>=|<=|\s-\s/.test(cfg.player?.version || "")) bad(`player.version "${cfg.player.version}" looks like a range; pin an exact version`);
  if (!Array.isArray(cfg.plugins) || !cfg.plugins.length) bad(`plugins[] required, non-empty`);
  for (const p of cfg.plugins || []) {
    if (!/^[a-z0-9][a-z0-9-]*$/.test(p.id || "")) bad(`plugin id "${p.id}" must be a lowercase slug`);
    for (const a of (p.abis || ["wam", "wclap"])) if (!["wam", "wclap"].includes(a)) bad(`plugin ${p.id}: bad abi "${a}"`);
  }
  for (const abi of ["wam", "wclap"]) {
    const prof = cfg.deploy?.[abi]?.profile;
    if (prof && !["github-pages", "cloudflare", "github-pages+coi"].includes(prof)) bad(`deploy.${abi}.profile invalid: ${prof}`);
    if (abi === "wclap" && prof === "github-pages") bad(`deploy.wclap.profile "github-pages" cannot provide cross-origin isolation — use cloudflare (default) or github-pages+coi (fallback)`);
  }
  if (!problems.length) ok(`config schema + invariants`);
}

// ---- isolation coverage across deployed paths ----
function checkIsolation(cfg, site) {
  const usesWclap = (cfg.plugins || []).some((p) => (p.abis || ["wam", "wclap"]).includes("wclap"));
  if (!usesWclap) { ok(`no WCLAP demos — isolation N/A`); return; }
  const profile = cfg.deploy?.wclap?.profile || "cloudflare";
  const base = cfg.deploy?.wclap?.basePath || "/";

  if (profile === "cloudflare") {
    const hp = join(site, "_headers");
    if (!existsSync(hp)) { bad(`cloudflare WCLAP but no _headers in site`); return; }
    const h = readFileSync(hp, "utf8");
    // The COOP/COEP rule must match the DEPLOYED base path, not just "/".
    const covers = new RegExp(`^\\s*${base.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\*`, "m").test(h);
    covers ? ok(`_headers COOP/COEP pattern covers base path ${base}`) : bad(`_headers does NOT cover base path ${base} (isolation would fail under ${base})`);
    /cross-origin-opener-policy:\s*same-origin/i.test(h) ? ok(`COOP: same-origin present`) : bad(`_headers missing COOP: same-origin`);
    /cross-origin-embedder-policy:\s*(require-corp|credentialless)/i.test(h) ? ok(`COEP present`) : bad(`_headers missing COEP`);
  } else if (profile === "github-pages+coi") {
    existsSync(join(site, "coi-serviceworker.js"))
      ? ok(`coi-serviceworker.js present for github-pages+coi fallback`)
      : bad(`github-pages+coi selected but coi-serviceworker.js missing from site`);
  }

  // Every WCLAP page must actually gate on crossOriginIsolated + live under the base path.
  for (const p of cfg.plugins || []) {
    if (!(p.abis || ["wam", "wclap"]).includes("wclap")) continue;
    const page = join(site, p.id, "wclap", "index.html");
    if (!existsSync(page)) { bad(`WCLAP page missing: ${p.id}/wclap/index.html`); continue; }
    const html = readFileSync(page, "utf8");
    /crossOriginIsolated/.test(html) ? ok(`${p.id}/wclap guards on crossOriginIsolated`) : bad(`${p.id}/wclap missing crossOriginIsolated guard`);
    if (profile === "github-pages+coi" && !/coi-serviceworker\.js/.test(html)) bad(`${p.id}/wclap (coi fallback) does not register coi-serviceworker`);
  }
}

// ---- metadata / OG (blocking) ----
//
// The generator EMITS an og:image tag; it does not RENDER the image (rendering needs a
// browser, and the generator is deliberately offline + deterministic). So the tag can
// point at nothing. Worse: a static host will happily serve a missing asset as HTTP 200
// (Cloudflare Pages does), so a status-code probe would confirm an image that isn't there.
// The only honest check is the bytes: the file must exist AND start with the PNG magic.
const PNG_MAGIC = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
function ogImageIsReal(site, pageRel, imgUrl) {
  // og:image is absolute (siteBase + basePath + id + "/og.png"); resolve it to the page dir
  const name = imgUrl.split("/").pop();
  const f = join(site, dirname(pageRel), name);
  if (!existsSync(f)) return { ok: false, why: `missing: ${dirname(pageRel)}/${name}` };
  const head = readFileSync(f).subarray(0, 8);
  if (!head.equals(PNG_MAGIC)) return { ok: false, why: `${dirname(pageRel)}/${name} is not a PNG (a host would still serve it 200)` };
  return { ok: true };
}

function checkMetadata(cfg, site) {
  const strategy = cfg.metadata?.ogImageStrategy || "screenshot";
  const pages = [];
  const walk = (d) => { for (const e of readdirSync(d)) { const f = join(d, e); statSync(f).isDirectory() ? walk(f) : (e === "index.html" && pages.push(f)); } };
  walk(site);
  for (const f of pages) {
    const h = readFileSync(f, "utf8");
    const rel = f.slice(site.length + 1);
    if (!/property="og:title"/.test(h)) bad(`${rel}: missing og:title`);
    if (!/property="og:url"/.test(h)) bad(`${rel}: missing og:url`);
    if (strategy === "screenshot" && !/property="og:image"/.test(h) && !/index\.html$/.test(rel) === false && !rel.startsWith("index")) {
      // gallery root may omit a per-plugin image; plugin pages must have one under screenshot strategy
    }
    if (strategy === "screenshot" && rel !== "index.html") {
      const m = h.match(/property="og:image" content="([^"]+)"/);
      if (!m) { bad(`${rel}: screenshot strategy requires og:image`); }
      else {
        const v = ogImageIsReal(site, rel, m[1]);
        if (!v.ok) bad(`${rel}: og:image ${v.why}`);
      }
    }
  }
  if (pages.length) ok(`metadata present on ${pages.length} pages (strategy: ${strategy})`);
}

// ---- ownership drift ----
function checkOwnership(site) {
  const mp = join(site, ".pulp-web-demo.manifest.json");
  if (!existsSync(mp)) { bad(`no manifest — cannot verify ownership`); return; }
  const man = JSON.parse(readFileSync(mp, "utf8"));
  for (const { path: rel, sha256 } of man.files) {
    if (rel === ".pulp-web-demo.manifest.json") continue;
    const f = join(site, rel);
    if (!existsSync(f)) { bad(`owned file missing: ${rel}`); continue; }
    const cur = createHash("sha256").update(readFileSync(f)).digest("hex");
    if (cur !== sha256) bad(`owned file locally modified (would be clobbered on regenerate): ${rel}`);
  }
  if (!problems.length) ok(`all ${man.files.length} owned files unmodified`);
}

// ---- run ----
const configPath = resolve(opt("--config", ".pulp-web-demo/config.json"));
const site = opt("--site") ? resolve(opt("--site")) : null;
const cfg = existsSync(configPath) ? loadConfig(configPath) : null;

if (cfg && !site) validateConfig(cfg);
if (site) {
  if (cfg) validateConfig(cfg);
  if (has("--check-isolation") && cfg) checkIsolation(cfg, site);
  if (has("--check-metadata") && cfg) checkMetadata(cfg, site);
  if (has("--check-ownership")) checkOwnership(site);
}

if (problems.length) { console.error(`\npulp-web-demo: ${problems.length} problem(s) — FAILED`); process.exit(1); }
console.error(`\npulp-web-demo: all checks passed`);
