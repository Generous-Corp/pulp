// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import { trustedVendorPayload } from "./vendor_payload.mjs";

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
