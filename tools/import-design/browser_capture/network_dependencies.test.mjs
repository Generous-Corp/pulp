// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  expandAuditedProviderDependencies,
} from "./network_dependencies.mjs";

test("Google Fonts authorizes only its audited font asset origin", () => {
  assert.deepEqual(
    expandAuditedProviderDependencies([
      "https://unpkg.com",
      "https://fonts.googleapis.com",
    ]),
    [
      "https://fonts.googleapis.com",
      "https://fonts.gstatic.com",
      "https://unpkg.com",
    ]);
});

test("unknown providers do not broaden the network allowlist", () => {
  assert.deepEqual(
    expandAuditedProviderDependencies(["https://example.com"]),
    ["https://example.com"]);
});
