// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";
import vm from "node:vm";

import { evaluateDesignTokens, TOKEN_EXPRESSION } from "./tokens.mjs";

test("browser token expression recognizes only exact px dimensions", () => {
  const style = {
    *[Symbol.iterator]() {
      yield "--radius";
      yield "--relative";
    },
  };
  const values = new Map([
    ["--radius", "12px"],
    ["--relative", "100%"],
  ]);
  const context = {
    document: {
      styleSheets: [{ cssRules: [{ style }] }],
      adoptedStyleSheets: [],
      documentElement: {},
      body: {},
      querySelectorAll() {
        return [];
      },
    },
    getComputedStyle() {
      return {
        getPropertyValue(name) {
          return values.get(name) ?? "";
        },
      };
    },
    CSS: {
      supports() {
        return false;
      },
    },
  };
  const records = vm.runInNewContext(TOKEN_EXPRESSION, context);
  assert.equal(records.find((record) => record.name === "--radius").px, 12);
  assert.equal(records.find((record) => record.name === "--relative").px, null);
});

test("browser tokens preserve colors, px dimensions, and relative CSS values", async () => {
  const cdp = {
    async call(method) {
      assert.equal(method, "Runtime.evaluate");
      return {
        result: {
          value: [
            { name: "--accent", value: "#7c5cff", is_color: true, px: null },
            { name: "--radius", value: "12px", is_color: false, px: 12 },
            { name: "--panel-width", value: "100%", is_color: false, px: null },
            { name: "--space", value: "calc(1rem + 2px)", is_color: false, px: null },
          ],
        },
      };
    },
  };

  const tokens = await evaluateDesignTokens(cdp);
  assert.equal(tokens.colors["css/accent"], "#7c5cff");
  assert.equal(tokens.dimensions["css/radius"], 12);
  assert.equal(tokens.strings["css/panel-width"], "100%");
  assert.equal(tokens.strings["css/space"], "calc(1rem + 2px)");
  assert.equal(tokens.source_identity["css/accent"].source_id, "--accent");
  assert.equal(
    tokens.source_identity["css/accent"].source_mode,
    "computed-capture-light");
  assert.deepEqual(tokens.capture_context, {
    color_scheme: "light",
    reduced_motion: "no-preference",
    scope: "active-computed-values",
  });
});
