// SPDX-License-Identifier: MIT
// Capture-side provenance guard for browser-only runtime payloads.  A DOM
// marker is author-controlled input, so it is evidence of a claimed role, not
// proof that the referenced bytes are the runtime we are allowed to remove.

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
