// SPDX-License-Identifier: MIT
// The @font-face faces a plain HTML page loads from url() sources.
//
// A materialized bundle hands its Blob-URL fonts over in the materialized
// document. A plain page names files beside it instead (`src: url(fonts/x.ttf)`),
// and Chromium draws the capture with those bytes, so the native import needs
// the same bytes and descriptors or its text is reshaped with whatever
// same-named face the host has. Only faces Chromium actually loaded are
// recorded: a face no text used was never fetched and shapes nothing.
import { createHash } from "node:crypto";

// Bounds on authored input: faces read, and font bytes carried in total.
export const MAX_PAGE_FONT_FACES = 64;
export const MAX_PAGE_FONT_BYTES = 32 * 1024 * 1024;

export const PAGE_FONT_FACES_SCHEMA = "pulp-browser-capture-font-faces-v1";

const FORMAT_MIME = Object.freeze({
  woff2: "font/woff2",
  woff: "font/woff",
  truetype: "font/ttf",
  ttf: "font/ttf",
  opentype: "font/otf",
  otf: "font/otf",
});

// The MIME type of a face's bytes from its format() hint, else its extension.
export function fontMimeType(format, url) {
  const hinted = FORMAT_MIME[String(format || "").toLowerCase()];
  if (hinted) return hinted;
  const extension = /\.([a-z0-9]+)(?:[?#]|$)/i.exec(url || "")?.[1]?.toLowerCase();
  return FORMAT_MIME[extension] ?? "font/ttf";
}

// Every @font-face rule the document's readable style sheets declare, with
// each url() source resolved against its own sheet, and whether any face of
// the rule's family finished loading (the only signal an inline data: source
// has, since it never crosses the network).
const LIST_EXPRESSION = `(() => {
  const unquote = (value) => String(value || "").trim().replace(/^(['"])(.*)\\1$/, "$2");
  const rules = [];
  const visit = (list, base) => {
    for (const rule of list) {
      if (rule instanceof CSSFontFaceRule) rules.push({ rule, base });
      else if (rule instanceof CSSImportRule && rule.styleSheet) {
        try { visit(rule.styleSheet.cssRules, rule.styleSheet.href || base); } catch {}
      } else if (rule.cssRules) visit(rule.cssRules, base);
    }
  };
  for (const sheet of document.styleSheets) {
    // A cross-origin sheet's rules are not readable; Chromium still applies it.
    try { visit(sheet.cssRules, sheet.href || document.baseURI); } catch {}
  }
  const loadedFamilies = new Set([...document.fonts]
    .filter((face) => face.status === "loaded").map((face) => unquote(face.family)));
  return rules.map(({ rule, base }) => {
    const style = rule.style;
    const family = unquote(style.getPropertyValue("font-family"));
    const sources = [];
    for (const match of style.getPropertyValue("src").matchAll(
        /url\\(\\s*(?:"([^"]*)"|'([^']*)'|([^)\\s]*))\\s*\\)(?:\\s*format\\(\\s*["']?([^"')]*)["']?\\s*\\))?/g)) {
      const authored = match[1] ?? match[2] ?? match[3] ?? "";
      try {
        sources.push({ authored, url: new URL(authored, base).href, format: match[4] || "" });
      } catch {}
    }
    return {
      family,
      weight: style.getPropertyValue("font-weight").trim() || "normal",
      style: style.getPropertyValue("font-style").trim() || "normal",
      unicode_range: style.getPropertyValue("unicode-range").trim(),
      sources,
      family_loaded: loadedFamilies.has(family),
    };
  });
})()`;

// Every response status the page received, by URL, recorded from before the
// first navigation. Chromium fetches a face's sources in order and only when
// text needs the face, so this is its own record of which source it used.
export function recordResponseStatuses(cdp) {
  const statuses = new Map();
  cdp.on("Network.responseReceived", ({ response }) => {
    if (response?.url) statuses.set(response.url, Number(response.status) || 0);
  });
  return statuses;
}

function fetchExpression(url) {
  return `(async () => {
    const response = await fetch(${JSON.stringify(url)}, { cache: "force-cache" });
    if (!response.ok) return { status: response.status };
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (bytes.length > ${MAX_PAGE_FONT_BYTES}) return { status: 413 };
    let binary = "";
    for (let offset = 0; offset < bytes.length; offset += 0x8000) {
      binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
    }
    return { status: 200, data_base64: btoa(binary) };
  })()`;
}

// Where a source may be read from: the capture's own loopback server and
// inline data always, a remote host only when the import allows the network.
function readable(url, { allowNetwork, privatePrefix }) {
  if (url.startsWith("data:")) return true;
  if (privatePrefix && url.startsWith(privatePrefix)) return true;
  return allowNetwork && url.startsWith("https://");
}

// A source as capture evidence may name it: a staged file by its path under
// the page, inline data by its scheme, a remote font by origin and path.
function publicSource(url, privatePrefix) {
  if (url.startsWith("data:")) return "data:";
  if (privatePrefix && url.startsWith(privatePrefix)) {
    return `pulp-capture:///${url.slice(privatePrefix.length).split(/[?#]/)[0]}`;
  }
  try {
    const parsed = new URL(url);
    return `${parsed.origin}${parsed.pathname}`;
  } catch {
    return "(invalid URL)";
  }
}

function missingFontError(face, source, privatePrefix) {
  const error = new Error(
    `@font-face "${face.family}" (weight ${face.weight}, ${face.style}) could not load ` +
    `${JSON.stringify(source.authored)}: ${publicSource(source.url, privatePrefix)} ` +
    `answered ${source.status}. Put the font file beside the page at that path, or ` +
    "remove the rule so the capture and the native import use the same fallback.");
  error.code = "capture-font-missing";
  return error;
}

// Reads every face Chromium loaded from a url() source and returns the
// font-faces.json document. A face whose file the page names but the staged
// project does not have fails the capture by name instead of leaving a 404
// for the health check to report without saying which font it was.
export async function capturePageFontFaces(cdp, options = {}) {
  const { privatePrefix = "", allowNetwork = false, statuses = new Map() } = options;
  const listed = await cdp.call("Runtime.evaluate",
    { expression: LIST_EXPRESSION, returnByValue: true });
  const rules = listed.result?.value ?? [];
  if (rules.length > MAX_PAGE_FONT_FACES) {
    const error = new Error(`the page declares more than ${MAX_PAGE_FONT_FACES} @font-face rules`);
    error.code = "capture-font-faces-exceeded";
    throw error;
  }
  const faces = [];
  const assets = new Map();
  const bytesByUrl = new Map();
  let total = 0;
  for (const rule of rules) {
    const sources = rule.sources.map((source) => ({ ...source,
      status: source.url.startsWith("data:")
        ? (rule.family_loaded ? 200 : null)
        : (statuses.get(source.url) ?? null) }));
    // Chromium moves past a source that fails to the next one, so a face is
    // missing only when a staged source failed and none answered.
    const source = sources.find((candidate) =>
      candidate.status >= 200 && candidate.status < 300);
    const failed = sources.find((candidate) => candidate.status >= 400 &&
      privatePrefix && candidate.url.startsWith(privatePrefix));
    if (!source) {
      if (failed) throw missingFontError(rule, failed, privatePrefix);
      continue;
    }
    if (!readable(source.url, { allowNetwork, privatePrefix })) continue;
    let bytes = bytesByUrl.get(source.url);
    if (!bytes) {
      const read = await cdp.call("Runtime.evaluate", {
        expression: fetchExpression(source.url), awaitPromise: true, returnByValue: true });
      const value = read.result?.value;
      if (!value || value.status !== 200 || typeof value.data_base64 !== "string") {
        throw missingFontError(rule, { ...source, status: value?.status ?? 0 }, privatePrefix);
      }
      bytes = Buffer.from(value.data_base64, "base64");
      total += bytes.length;
      if (total > MAX_PAGE_FONT_BYTES) {
        const error = new Error("the page's @font-face files exceed 32 MiB");
        error.code = "capture-font-faces-exceeded";
        throw error;
      }
      bytesByUrl.set(source.url, bytes);
    }
    const sha256 = createHash("sha256").update(bytes).digest("hex");
    const mimeType = fontMimeType(source.format, source.url);
    if (!assets.has(sha256)) {
      assets.set(sha256, { sha256, mime_type: mimeType, byte_length: bytes.length,
        data_base64: bytes.toString("base64") });
    }
    faces.push({
      family: rule.family,
      weight: rule.weight,
      style: rule.style,
      unicode_range: rule.unicode_range,
      format: source.format,
      source: publicSource(source.url, privatePrefix),
      sha256,
    });
  }
  return { schema: PAGE_FONT_FACES_SCHEMA, faces, assets: [...assets.values()] };
}
