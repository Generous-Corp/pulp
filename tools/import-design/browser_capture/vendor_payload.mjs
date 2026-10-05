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
