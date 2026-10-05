// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  SUPPORTED_VENDOR_PAYLOAD_SHA256,
  trustedCapturedVendorPayload,
  trustedVendorPayload,
  trustedVendorPayloadDigest,
} from "./vendor_payload.mjs";

test("accepts the supported React, ReactDOM, and Babel payload shapes", () => {
  const react = "/** @license React react.development.js */\n" +
    "ReactVersion createElement " + " ".repeat(32 * 1024);
  const reactDom = "/** @license React react-dom.development.js */\n" +
    "ReactVersion createRoot " + " ".repeat(32 * 1024);
  assert.equal(trustedVendorPayload(
    "react", react), true);
  assert.equal(trustedVendorPayload(
    "react-dom", reactDom), true);
  const babel = `.Babel=${" ".repeat(1_000_000)}transformScriptTags registerPlugin`;
  assert.equal(trustedVendorPayload("babel", babel), true);
});

test("rejects marker collisions that are not trusted runtime payloads", () => {
  // The role marker must not turn an arbitrary small or unrelated payload into
  // removable browser code.  This is the negative control for capture-side
  // provenance: a caller that only checks the DOM marker would accept it.
  assert.equal(trustedVendorPayload(
    "react", "/* data-pulp-vendor=react */ window.keep = true;"), false);
  assert.equal(trustedVendorPayload(
    "react-dom", "/* @license React react-dom.development.js */"), false);
  assert.equal(trustedVendorPayload(
    "babel", `.Babel=${" ".repeat(1_000_000)}no-op`), false);
  assert.equal(trustedVendorPayload("unknown", "transform"), false);
});

test("pins the supported vendor identities to exact SHA-256 digests", () => {
  const reactSha =
    "28348fef6cb0ed8b2ceeb22deaf824428fd13875d84c73d38f77dd216fc24e7f";
  const reactDomSha =
    "f9044a5e9c39db8bb1a204dff924e526ec0a621e695bb69de1035811be8709e4";
  const babelSha =
    "2623a9e22809915ce789b4461154e277ddce520d5a4320c14d44332a5d0dcea0";
  assert.equal(SUPPORTED_VENDOR_PAYLOAD_SHA256.react, reactSha);
  assert.equal(SUPPORTED_VENDOR_PAYLOAD_SHA256["react-dom"], reactDomSha);
  assert.equal(SUPPORTED_VENDOR_PAYLOAD_SHA256.babel, babelSha);
  assert.equal(trustedVendorPayloadDigest(
    "react", reactSha), true);
  assert.equal(trustedVendorPayloadDigest(
    "react-dom", reactDomSha), true);
  assert.equal(trustedVendorPayloadDigest(
    "babel", babelSha), true);
  assert.equal(trustedVendorPayloadDigest(
    "react", "0".repeat(64)), false);
  assert.equal(trustedVendorPayloadDigest(
    "unknown", SUPPORTED_VENDOR_PAYLOAD_SHA256.react), false);
});

test("capture trust rejects padded marker spoofing even when the shape matches", () => {
  const spoof = "/** @license React react.development.js */\n" +
    "ReactVersion createElement " + " ".repeat(32 * 1024) +
    "globalThis.authored = true;";
  assert.equal(trustedVendorPayload("react", spoof), true);
  assert.equal(trustedCapturedVendorPayload("react", spoof), false);
});
