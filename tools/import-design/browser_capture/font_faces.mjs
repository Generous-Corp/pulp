// SPDX-License-Identifier: MIT
// The @font-face faces a page loads from url() sources.
//
// A plain page names files beside it (`src: url(fonts/x.ttf)`), and a bundle
// (a Claude standalone export) serves its fonts from Blob URLs it creates.
// Chromium draws the capture with those bytes, so the native import needs the
// same bytes and descriptors or its text is reshaped with whatever same-named
// face the host has. Only faces Chromium actually loaded and decoded are
// recorded: a face no text used was never fetched and shapes nothing, and a
// file its font sanitizer rejected never drew a glyph.
import { createHash } from "node:crypto";

// Bounds on authored input: @font-face rules read, distinct faces carried
// once verbatim repeats are folded, and distinct font bytes carried in total.
// A real bundle runs to about 60 faces (Spectr declares 59).
export const MAX_PAGE_FONT_RULES = 4096;
export const MAX_PAGE_FONT_FACES = 256;
export const MAX_PAGE_FONT_BYTES = 64 * 1024 * 1024;

export const PAGE_FONT_FACES_SCHEMA = "pulp-browser-capture-font-faces-v1";

// The container a font file's first four bytes declare, and the MIME type the
// import records for it, or null for bytes that are not a font. WOFF 1.0 is
// recognized so it can be refused by name: not every platform font manager
// Pulp renders through decodes it.
const SIGNATURES = [
  [[0x00, 0x01, 0x00, 0x00], "truetype", "font/ttf"],
  [[0x74, 0x72, 0x75, 0x65], "truetype", "font/ttf"], // 'true'
  [[0x4f, 0x54, 0x54, 0x4f], "opentype", "font/otf"], // 'OTTO'
  [[0x77, 0x4f, 0x46, 0x32], "woff2", "font/woff2"], // 'wOF2'
  [[0x77, 0x4f, 0x46, 0x46], "woff", "font/woff"], // 'wOFF'
];

export function fontSignature(bytes) {
  for (const [magic, container, mimeType] of SIGNATURES) {
    if (bytes.length >= 4 && magic.every((byte, index) => bytes[index] === byte)) {
      return { container, mimeType };
    }
  }
  return null;
}

// Every @font-face rule the document's readable style sheets declare, with
// each url() source resolved against its own sheet, and whether any face of
// the rule's family finished loading (the only signal an inline data: or
// Blob source has, since neither crosses the network).
//
// Descriptors come from the rule as authored when its sheet is an inline
// <style>, because CSSOM re-serializes them (unicode-range `U+0000-00FF`
// reads back as `U+0-FF`) and DesignIR keeps them verbatim; a linked sheet
// falls back to CSSOM. An inline sheet's @font-face blocks are matched to its
// rules in order, and only when the counts agree.
const LIST_EXPRESSION = `(() => {
  const unquote = (value) => String(value || "").trim().replace(/^(['"])(.*)\\1$/, "$2");
  const authoredBlocks = new Map();
  const authored = (sheet, index) => {
    if (!authoredBlocks.has(sheet)) {
      const owner = sheet.ownerNode;
      const blocks = owner && owner.localName === "style"
        ? [...owner.textContent.matchAll(/@font-face\\s*\\{([^}]*)\\}/gi)].map((m) => m[1])
        : null;
      let count = 0;
      const countRules = (list) => {
        for (const rule of list) {
          if (rule instanceof CSSFontFaceRule) count += 1;
          else if (!(rule instanceof CSSImportRule) && rule.cssRules) countRules(rule.cssRules);
        }
      };
      try { countRules(sheet.cssRules); } catch {}
      authoredBlocks.set(sheet, blocks && blocks.length === count ? blocks : null);
    }
    return authoredBlocks.get(sheet)?.[index];
  };
  const read = (block, name) => {
    const match = block.match(new RegExp("(?:^|;)\\\\s*" + name + "\\\\s*:\\\\s*([^;}]*)", "i"));
    return match ? match[1].trim() : "";
  };
  const rules = [];
  const perSheet = new Map();
  const visit = (list, base, sheet) => {
    for (const rule of list) {
      if (rule instanceof CSSFontFaceRule) {
        const index = perSheet.get(sheet) ?? 0;
        perSheet.set(sheet, index + 1);
        rules.push({ rule, base, block: authored(sheet, index) });
      }
      else if (rule instanceof CSSImportRule && rule.styleSheet) {
        try {
          visit(rule.styleSheet.cssRules, rule.styleSheet.href || base, rule.styleSheet);
        } catch {}
      } else if (rule.cssRules) visit(rule.cssRules, base, sheet);
    }
  };
  for (const sheet of document.styleSheets) {
    // A cross-origin sheet's rules are not readable; Chromium still applies it.
    try { visit(sheet.cssRules, sheet.href || document.baseURI, sheet); } catch {}
  }
  const loadedFamilies = new Set([...document.fonts]
    .filter((face) => face.status === "loaded").map((face) => unquote(face.family)));
  // The FontFace Chromium built for a rule, matched on the descriptors both
  // serialize the same way. Its status is Chromium's own verdict on the
  // file: "loaded" once its sanitizer accepted the bytes, "error" when not.
  const faceStatus = (family, style) => {
    const weight = style.getPropertyValue("font-weight").trim() || "normal";
    const fontStyle = style.getPropertyValue("font-style").trim() || "normal";
    const range = style.getPropertyValue("unicode-range").trim() || "U+0-10FFFF";
    const face = [...document.fonts].find((candidate) =>
      unquote(candidate.family) === family && candidate.weight === weight &&
      candidate.style === fontStyle && candidate.unicodeRange === range);
    return face ? face.status : null;
  };
  return rules.map(({ rule, base, block }) => {
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
    const descriptor = (name) =>
      block !== undefined ? read(block, name) : style.getPropertyValue(name).trim();
    return {
      family,
      weight: descriptor("font-weight") || "normal",
      style: descriptor("font-style") || "normal",
      unicode_range: descriptor("unicode-range"),
      sources,
      family_loaded: loadedFamilies.has(family),
      face_status: faceStatus(family, style),
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

// Where a source may be read from: the capture's own loopback server, inline
// data and the page's own Blob URLs always, a remote host only when the
// import allows the network.
function readable(url, { allowNetwork, privatePrefix }) {
  if (url.startsWith("data:") || url.startsWith("blob:")) return true;
  if (privatePrefix && url.startsWith(privatePrefix)) return true;
  // Only https: a plain-http font is refused even with the network allowed,
  // as the capture's network guard refuses plain-http origins.
  return allowNetwork && url.startsWith("https://");
}

// A source as capture evidence may name it: a staged file by its path under
// the page, inline data by its scheme, a remote font by origin and path.
function publicSource(url, privatePrefix) {
  if (url.startsWith("data:")) return "data:";
  // A Blob URL names a fresh UUID on every launch; it is evidence of nothing.
  if (url.startsWith("blob:")) return "blob:";
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
  const exceeded = (what) => {
    const error = new Error(`the page's @font-face ${what}`);
    error.code = "capture-font-faces-exceeded";
    return error;
  };
  if (rules.length > MAX_PAGE_FONT_RULES) {
    throw exceeded(`rules number more than ${MAX_PAGE_FONT_RULES}`);
  }
  const faces = [];
  const dropped = [];
  const assets = new Map();
  const bytesByUrl = new Map();
  const seen = new Set();
  let total = 0;
  for (const rule of rules) {
    const sources = rule.sources.map((source) => ({ ...source,
      status: source.url.startsWith("data:") || source.url.startsWith("blob:")
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
    const drop = (reason) => dropped.push({ family: rule.family, weight: rule.weight,
      style: rule.style, source: publicSource(source.url, privatePrefix), reason });
    if (rule.face_status === "error") {
      drop("Chromium fetched it and could not decode it, so its text drew in a fallback face");
      continue;
    }
    let bytes = bytesByUrl.get(source.url);
    if (!bytes) {
      const read = await cdp.call("Runtime.evaluate", {
        expression: fetchExpression(source.url), awaitPromise: true, returnByValue: true });
      const value = read.result?.value;
      if (!value || value.status !== 200 || typeof value.data_base64 !== "string") {
        // The page may revoke a Blob URL once its font has loaded; the face
        // drew, but its bytes are gone, so it is named rather than carried.
        if (source.url.startsWith("blob:")) {
          drop("the page revoked its Blob URL before the capture could read the font");
          continue;
        }
        throw missingFontError(rule, { ...source, status: value?.status ?? 0 }, privatePrefix);
      }
      bytes = Buffer.from(value.data_base64, "base64");
      bytesByUrl.set(source.url, bytes);
    }
    const signature = fontSignature(bytes);
    if (!signature) {
      drop("its bytes start with no TrueType, OpenType, WOFF or WOFF2 signature");
      continue;
    }
    if (signature.container === "woff") {
      drop("it is WOFF 1.0, which not every native font manager decodes; serve it as WOFF2 or TTF");
      continue;
    }
    const sha256 = createHash("sha256").update(bytes).digest("hex");
    // Typed by its own bytes: a Blob's type is whatever its page said.
    const blob = source.url.startsWith("blob:");
    const mimeType = signature.mimeType;
    // Bundle fonts keep the ids the materialized runtime names them by.
    const assetId = `${blob ? "pulp-materialized-asset" : "pulp-page-font"}-${sha256}`;
    // A rule repeated verbatim binds once, as the materialized lane always did.
    const key = JSON.stringify([rule.family, assetId, rule.weight, rule.style,
      rule.unicode_range]);
    if (seen.has(key)) continue;
    seen.add(key);
    if (seen.size > MAX_PAGE_FONT_FACES) {
      throw exceeded(`faces number more than ${MAX_PAGE_FONT_FACES}`);
    }
    if (!assets.has(sha256)) {
      total += bytes.length;
      if (total > MAX_PAGE_FONT_BYTES) throw exceeded("files exceed 64 MiB");
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
      asset_id: assetId,
    });
  }
  return { schema: PAGE_FONT_FACES_SCHEMA, faces, dropped, assets: [...assets.values()] };
}
