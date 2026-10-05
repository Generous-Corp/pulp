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
  assert.equal(trustedVendorPayloadDigest(
    "react", SUPPORTED_VENDOR_PAYLOAD_SHA256.react), true);
  assert.equal(trustedVendorPayloadDigest(
    "react-dom", SUPPORTED_VENDOR_PAYLOAD_SHA256["react-dom"]), true);
  assert.equal(trustedVendorPayloadDigest(
    "babel", SUPPORTED_VENDOR_PAYLOAD_SHA256.babel), true);
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
