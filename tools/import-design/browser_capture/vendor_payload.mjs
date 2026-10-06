// SPDX-License-Identifier: MIT
// Capture-side provenance guard for browser-only runtime payloads.  A DOM
// marker is author-controlled input, so it is evidence of a claimed role, not
// proof that the referenced bytes are the runtime we are allowed to remove.

import { createHash } from "node:crypto";

// These are the exact payloads shipped by the supported browser runtime
// adapter.  Shape checks below remain useful for legacy/canonicalizer tests,
// but capture must also pin the bytes before it records removable provenance.
// A marker can be copied into authored code; a SHA-256 digest cannot be made
// to match one of these payloads without reproducing the payload itself.
export const SUPPORTED_VENDOR_PAYLOAD_SHA256 = Object.freeze({
  react: "28348fef6cb0ed8b2ceeb22deaf824428fd13875d84c73d38f77dd216fc24e7f",
  "react-dom": "f9044a5e9c39db8bb1a204dff924e526ec0a621e695bb69de1035811be8709e4",
  babel: "2623a9e22809915ce789b4461154e277ddce520d5a4320c14d44332a5d0dcea0",
});

export const SUPPORTED_VENDOR_KINDS = Object.freeze([
  "react",
  "react-dom",
  "babel",
]);

/**
 * Return true when `digest` is one of the exact bytes approved for `kind`.
 * This is kept separate so tests can exercise the immutable allowlist without
 * carrying multi-megabyte vendor fixtures in the source tree.
 */
export function trustedVendorPayloadDigest(kind, digest) {
  return typeof digest === "string" &&
    digest === SUPPORTED_VENDOR_PAYLOAD_SHA256[kind];
}

/**
 * Return true only for the known payload shapes emitted by the supported
 * browser runtime adapters.  Keep this deliberately conservative: an
 * unrecognised version stays in the materialized document and remains
 * loadable.  The role marker and the payload check are both required before a
 * canonicalizer may remove an asset.
 */
export function trustedVendorPayload(kind, source) {
  if (typeof source !== "string") return false;
  if (kind === "react") {
    return source.length >= 32 * 1024 &&
      source.includes("@license React") &&
      source.includes("react.development.js") &&
      source.includes("ReactVersion") &&
      source.includes("createElement");
  }
  if (kind === "react-dom") {
    return source.length >= 32 * 1024 &&
      source.includes("@license React") &&
      source.includes("react-dom.development.js") &&
      source.includes("ReactVersion") &&
      source.includes("createRoot");
  }
  if (kind === "babel") {
    return source.length > 1_000_000 &&
      source.slice(0, 1_000).includes(".Babel=") &&
      source.includes("transformScriptTags") &&
      source.includes("registerPlugin");
  }
  return false;
}

/**
 * Capture-only provenance check.  The capture path has to identify the exact
 * supported vendor bytes before writing `vendor_kind` into the materialized
 * document; shape/marker checks alone are spoofable by authored scripts.
 */
export function trustedCapturedVendorPayload(kind, source) {
  if (!trustedVendorPayload(kind, source)) return false;
  const digest = createHash("sha256").update(source, "utf8").digest("hex");
  return trustedVendorPayloadDigest(kind, digest);
}

/**
 * Identify a capture-owned vendor payload without relying on authored markup.
 * The exact digest is the authority; callers may safely use the result to add
 * the capture-only role marker to an otherwise unannotated script reference.
 */
export function classifyTrustedCapturedVendorPayload(source) {
  for (const kind of SUPPORTED_VENDOR_KINDS) {
    if (trustedCapturedVendorPayload(kind, source)) return kind;
  }
  return "";
}

function capturedAttribute(openTag, wanted) {
  let index = openTag.toLowerCase().indexOf("script") + 6;
  while (index < openTag.length) {
    while (/\s/.test(openTag[index])) ++index;
    if (index >= openTag.length || openTag[index] === ">" ||
        openTag[index] === "/") break;
    const start = index;
    while (index < openTag.length && !/[\s=/>]/.test(openTag[index])) ++index;
    const name = openTag.slice(start, index).toLowerCase();
    while (/\s/.test(openTag[index])) ++index;
    let value = "";
    if (openTag[index] === "=") {
      ++index;
      while (/\s/.test(openTag[index])) ++index;
      const quote = openTag[index] === '"' || openTag[index] === "'"
        ? openTag[index++] : "";
      const valueStart = index;
      if (quote) {
        while (index < openTag.length && openTag[index] !== quote) ++index;
      } else {
        while (index < openTag.length && !/[\s>]/.test(openTag[index])) ++index;
      }
      value = openTag.slice(valueStart, index);
      if (quote && openTag[index] === quote) ++index;
    }
    if (name === wanted) return value;
  }
  return undefined;
}

/**
 * Add capture-owned provenance to matching empty script references.  This is
 * deliberately conservative: any pre-existing conflicting marker, non-empty
 * script body, malformed tag, or URL mismatch leaves the complete document
 * unchanged.  That prevents a capture from rewriting authored behavior.
 */
export function annotateCapturedVendorReferences(html, assetUrl, vendorKind) {
  if (typeof html !== "string" || !html || typeof assetUrl !== "string" ||
      !assetUrl || !SUPPORTED_VENDOR_KINDS.includes(vendorKind)) return html;

  const lowerHtml = html.toLowerCase();
  const replacements = [];
  let cursor = 0;
  while (cursor < html.length) {
    const start = lowerHtml.indexOf("<script", cursor);
    if (start < 0) break;
    const boundary = lowerHtml[start + 7];
    if (boundary && !/[\s/>]/.test(boundary)) {
      cursor = start + 7;
      continue;
    }
    let openEnd = start + 7;
    let quote = "";
    for (; openEnd < html.length; ++openEnd) {
      const character = html[openEnd];
      if (quote) {
        if (character === quote) quote = "";
      } else if (character === '"' || character === "'") {
        quote = character;
      } else if (character === ">") {
        break;
      }
    }
    if (openEnd >= html.length) return html;
    const openTag = html.slice(start, openEnd + 1);
    const close = lowerHtml.indexOf("</script", openEnd + 1);
    if (close < 0) return html;
    if (capturedAttribute(openTag, "src") === assetUrl) {
      if (html.slice(openEnd + 1, close).trim() !== "") {
        return html;
      }
      const declared = capturedAttribute(openTag, "data-pulp-vendor");
      if (declared !== undefined && declared !== vendorKind) return html;
      if (declared === undefined) {
        replacements.push([
          start,
          openEnd + 1,
          `${openTag.slice(0, -1)} data-pulp-vendor="${vendorKind}">`,
        ]);
      }
    }
    cursor = close + 8;
  }
  let result = html;
  for (let index = replacements.length - 1; index >= 0; --index) {
    const [start, end, replacement] = replacements[index];
    result = `${result.slice(0, start)}${replacement}${result.slice(end)}`;
  }
  return result;
}
