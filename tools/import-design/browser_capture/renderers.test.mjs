// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  awaitExplicitReadiness,
  finalizeKnownRenderers,
} from "./renderers.mjs";

test("explicit page readiness is awaited when the source provides it",
  async () => {
    const cdp = {
      async call(method, options) {
        assert.equal(method, "Runtime.evaluate");
        assert.equal(options.awaitPromise, true);
        return {
          result: {
            value: {
              contract: "__pulpCaptureReady",
              awaited: true,
            },
          },
        };
      },
    };

    const result = await awaitExplicitReadiness(cdp);

    assert.equal(result.contract, "__pulpCaptureReady");
    assert.equal(result.awaited, true);
  });

test("a rejected explicit readiness contract fails capture", async () => {
  const cdp = {
    async call() {
      return {
        exceptionDetails: {
          text: "Uncaught (in promise)",
          exception: { description: "Error: assets did not initialize" },
        },
      };
    },
  };

  await assert.rejects(
    awaitExplicitReadiness(cdp),
    (error) =>
      error.code === "capture-readiness-rejected" &&
      /assets did not initialize/.test(error.message));
});

test("renderer registry resolves loaded Lucide placeholders", async () => {
  const cdp = {
    async call(method, options) {
      assert.equal(method, "Runtime.evaluate");
      assert.match(options.expression, /lucide.*createIcons/s);
      return {
        result: {
          value: { applied: true, placeholders: 30, remaining: 0 },
        },
      };
    },
  };

  const results = await finalizeKnownRenderers(cdp);

  assert.equal(results.length, 1);
  assert.equal(results[0].name, "lucide");
  assert.equal(results[0].remaining, 0);
});

test("renderer registry rejects unresolved Lucide placeholders", async () => {
  const cdp = {
    async call() {
      return {
        result: {
          value: { applied: true, placeholders: 30, remaining: 1 },
        },
      };
    },
  };

  await assert.rejects(
    finalizeKnownRenderers(cdp),
    (error) => error.code === "capture-renderer-not-ready");
});
