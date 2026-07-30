// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  installCaptureHealthMonitor,
  verifyCaptureHealth,
} from "./health.mjs";

function fakeCdp(visual) {
  const listeners = new Map();
  return {
    listeners,
    on(name, callback) {
      listeners.set(name, callback);
    },
    async call(method) {
      assert.equal(method, "Runtime.evaluate");
      return { result: { value: visual } };
    },
  };
}

function snapshot(layoutCount) {
  return {
    documents: [{
      layout: { nodeIndex: Array.from({ length: layoutCount }, (_, i) => i) },
    }],
  };
}

test("capture health rejects blank dependency failures with an opt-in hint",
    async () => {
      const cdp = fakeCdp({
        visible_elements: 0,
        visible_text_characters: 0,
        painted_surfaces: 0,
      });
      const monitor = installCaptureHealthMonitor(cdp);
      cdp.listeners.get("Runtime.exceptionThrown")({
        exceptionDetails: { text: "React is unavailable" },
      });
      await assert.rejects(
        verifyCaptureHealth(
          cdp,
          snapshot(3),
          monitor,
          [{ url: "https://example.invalid/react.js" }]),
        (error) => {
          assert.equal(error.code, "capture-source-unresolved");
          assert.match(error.message, /React is unavailable/);
          assert.match(error.message, /--allow-browser-network/);
          assert.equal(error.health.status, "unresolved");
          return true;
        });
    });

test("capture health accepts a meaningfully painted document", async () => {
  const cdp = fakeCdp({
    visible_elements: 24,
    visible_text_characters: 120,
    painted_surfaces: 8,
  });
  const report = await verifyCaptureHealth(
    cdp, snapshot(30), installCaptureHealthMonitor(cdp), []);
  assert.equal(report.status, "healthy");
  assert.equal(report.layout_count, 30);
});

test("capture health rejects a visible shell with execution failures",
    async () => {
      const cdp = fakeCdp({
        visible_elements: 24,
        visible_text_characters: 120,
        painted_surfaces: 8,
      });
      const monitor = installCaptureHealthMonitor(cdp);
      cdp.listeners.get("Runtime.exceptionThrown")({
        exceptionDetails: { text: "application bootstrap failed" },
      });
      await assert.rejects(
        verifyCaptureHealth(cdp, snapshot(30), monitor, []),
        (error) => {
          assert.equal(error.code, "capture-source-unresolved");
          assert.match(error.message, /did not execute cleanly/);
          assert.match(error.message, /application bootstrap failed/);
          return true;
        });
    });

test("capture health rejects missing resources even when a shell painted",
    async () => {
      const cdp = fakeCdp({
        visible_elements: 24,
        visible_text_characters: 120,
        painted_surfaces: 8,
      });
      const monitor = installCaptureHealthMonitor(cdp);
      cdp.listeners.get("Network.responseReceived")({
        response: {
          status: 404,
          url: "https://example.invalid/private/missing.png?token=secret#x",
        },
      });
      await assert.rejects(
        verifyCaptureHealth(cdp, snapshot(30), monitor, []),
        (error) => {
          assert.match(
            error.message,
            /404 response from https:\/\/example\.invalid/);
          assert.doesNotMatch(
            error.message, /private|secret|missing/);
          return true;
        });
    });
