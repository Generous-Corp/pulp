// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  MAX_INTERACTION_ACTIONS,
  MAX_ACTION_TIMEOUT_MS,
  MAX_INTERACTION_PLAN_BYTES,
  MAX_SELECTOR_LENGTH,
  MAX_TOTAL_WAIT_MS,
  MAX_TYPE_TEXT_LENGTH,
  MAX_WAIT_MS,
  parseInteractionPlan,
} from "./interaction_plan.mjs";
import {
  createMainFrameNavigationGuard,
  executeInteractionPlan,
} from "./interaction_executor.mjs";

function plan(actions) {
  return parseInteractionPlan(JSON.stringify({
    schema: "pulp-browser-interactions-v1",
    version: 1,
    actions,
  }));
}

test("interaction plan accepts the bounded public action vocabulary", () => {
  const parsed = plan([
    { action: "click", selector: "#open" },
    { action: "context-click", selector: "#band" },
    { action: "type", selector: "[name=query]", text: "delay" },
    {
      action: "wait-for",
      selector: "[data-screen=patch]",
      state: "visible",
      timeout_ms: 9000,
    },
    { action: "wait-ms", milliseconds: 25 },
  ]);
  assert.equal(parsed.actions.length, 5);
  assert.equal(parsed.actions[0].timeout_ms, 5000);
  assert.match(parsed.sha256, /^[0-9a-f]{64}$/);
});

test("published interaction schema matches enforced parser bounds", async () => {
  const schema = JSON.parse(await readFile(fileURLToPath(
    new URL("./interaction_plan_protocol.json", import.meta.url)), "utf8"));
  const actions = schema.properties.actions;
  assert.equal(
    schema["x-pulp-max-document-bytes"],
    MAX_INTERACTION_PLAN_BYTES);
  assert.equal(
    schema["x-pulp-max-total-wait-ms"],
    MAX_TOTAL_WAIT_MS);
  assert.equal(actions.maxItems, MAX_INTERACTION_ACTIONS);

  const definitions = Object.fromEntries(actions.items.oneOf.map(
    (definition) => [definition.properties.action.const, definition]));
  assert.deepEqual(Object.keys(definitions).sort(),
    ["click", "context-click", "type", "wait-for", "wait-ms"]);
  for (const name of ["click", "context-click", "type", "wait-for"]) {
    assert.equal(
      definitions[name].properties.selector.maxLength,
      MAX_SELECTOR_LENGTH);
  }
  assert.equal(
    definitions.type.properties.text.maxLength,
    MAX_TYPE_TEXT_LENGTH);
  assert.equal(
    definitions.click.properties.timeout_ms.maximum,
    MAX_ACTION_TIMEOUT_MS);
  assert.equal(
    definitions["wait-ms"].properties.milliseconds.maximum,
    MAX_WAIT_MS);
});

test("interaction plan rejects executable or unbounded inputs", () => {
  assert.throws(() => plan([
    { action: "script", source: "document.body.remove()" },
  ]), /must be click, context-click, type, wait-for, or wait-ms/);
  assert.throws(() => plan([
    { action: "click", selector: "#open", script: "alert(1)" },
  ]), /unknown field "script"/);
  assert.throws(() => plan(Array.from(
    { length: MAX_INTERACTION_ACTIONS + 1 },
    () => ({ action: "wait-ms", milliseconds: 0 }))),
  /actions must contain/);
  assert.throws(() => plan([
    { action: "wait-ms", milliseconds: 5001 },
  ]), /must be an integer/);
  assert.throws(() => plan([
    { action: "type", selector: "input", text: "x".repeat(4097) },
  ]), /no longer than 4096/);
  assert.throws(() => parseInteractionPlan(JSON.stringify({
    schema: "pulp-browser-interactions-v1",
    version: 1,
    actions: [{ action: "wait-ms", milliseconds: 0 }],
    padding: "x".repeat(MAX_INTERACTION_PLAN_BYTES),
  })), /file must contain 1 to 65536 bytes/);
  assert.throws(() => plan(Array.from(
    { length: 7 },
    () => ({ action: "wait-ms", milliseconds: 5000 }))),
  /wait-ms actions total 35000ms; maximum is 30000ms/);
});

test("executor records reproducible evidence without typed plaintext", async () => {
  const calls = [];
  const cdp = {
    async call(method, params) {
      calls.push({ method, params });
      if (method === "Runtime.evaluate") {
        return { result: { value: { ok: true, state: "visible", x: 20, y: 30 } } };
      }
      return {};
    },
  };
  let settles = 0;
  const report = await executeInteractionPlan(cdp, plan([
    { action: "click", selector: "#open" },
    { action: "context-click", selector: "#band" },
    { action: "type", selector: "input", text: "private draft" },
    { action: "wait-for", selector: "#screen", state: "visible" },
    { action: "wait-ms", milliseconds: 2 },
  ]), {
    delay: async () => {},
    settle: async () => { settles += 1; },
  });

  assert.equal(report.action_count, 5);
  assert.equal(report.actions[2].text_length, 13);
  assert.equal("text_sha256" in report.actions[2], false);
  assert.equal("text" in report.actions[2], false);
  assert.equal(JSON.stringify(report).includes("private draft"), false);
  assert.equal(settles, 3);
  assert.equal(
    calls.filter(({ method }) => method === "Input.dispatchMouseEvent").length,
    4);
  const pointerCalls = calls.filter(
    ({ method }) => method === "Input.dispatchMouseEvent");
  assert.deepEqual(pointerCalls.map(({ params }) => params.button),
    ["left", "left", "right", "right"]);
  assert.deepEqual(pointerCalls.map(({ params }) => params.buttons),
    [1, 0, 2, 0]);
  assert.deepEqual(
    calls.find(({ method }) => method === "Input.insertText").params,
    { text: "private draft" });
});

test("published plan identity redacts typed plaintext", () => {
  const first = plan([
    { action: "type", selector: "input", text: "1234" },
  ]);
  const candidate = plan([
    { action: "type", selector: "input", text: "9876" },
  ]);
  const differentLength = plan([
    { action: "type", selector: "input", text: "12345" },
  ]);
  assert.equal(first.sha256, candidate.sha256);
  assert.notEqual(first.sha256, differentLength.sha256);
});

test("wait-for observes rendered state rather than snapshot strings", async () => {
  let probes = 0;
  const cdp = {
    async call(method) {
      assert.equal(method, "Runtime.evaluate");
      probes += 1;
      return {
        result: {
          value: probes < 3
            ? { ok: true, state: "hidden" }
            : { ok: true, state: "visible" },
        },
      };
    },
  };
  const report = await executeInteractionPlan(cdp, plan([{
    action: "wait-for",
    selector: "#working-screen",
    state: "visible",
    timeout_ms: 1000,
  }]), { delay: async () => {} });
  assert.equal(probes, 3);
  assert.equal(report.actions[0].status, "completed");
});

test("executor rejects a covered click target", async () => {
  const cdp = {
    async call() {
      return {
        result: {
          value: { ok: false, error: "target is covered by another rendered element" },
        },
      };
    },
  };
  await assert.rejects(
    executeInteractionPlan(cdp, plan([
      { action: "click", selector: "#hidden-button", timeout_ms: 10 },
    ]), { delay: async () => {} }),
    /covered by another rendered element/);
});

test("executor retries transient actionability failures until the timeout",
  async () => {
    let probes = 0;
    const cdp = {
      async call(method, params) {
        if (method === "Runtime.evaluate") {
          assert.match(params.expression, /matches\(":disabled"\)/);
          assert.match(params.expression, /document\.activeElement/);
          probes += 1;
          return {
            result: {
              value: probes < 3
                ? { ok: false, error: "target is disabled or read-only" }
                : { ok: true, x: 20, y: 30 },
            },
          };
        }
        return {};
      },
    };
    const report = await executeInteractionPlan(cdp, plan([{
      action: "type",
      selector: "input",
      text: "ready",
      timeout_ms: 1000,
    }]), { delay: async () => {} });
    assert.equal(probes, 3);
    assert.equal(report.actions[0].status, "completed");
  });

test("main-frame navigation guard allows same-document routing and ignores subframes",
  async () => {
    const listeners = new Map();
    const calls = [];
    let currentUrl = "http://127.0.0.1/editor.html";
    const cdp = {
      async call(method, params, sessionId) {
        calls.push({ method, params, sessionId });
        if (method === "Page.getFrameTree") {
          return {
            frameTree: {
              frame: { id: "main", loaderId: "loader-1", url: currentUrl },
            },
          };
        }
        if (method === "Runtime.evaluate") {
          return { result: { value: params.expression.startsWith("(() =>") } };
        }
        return {};
      },
      on(method, listener) {
        listeners.set(method, listener);
      },
    };
    const guard = await createMainFrameNavigationGuard(cdp);
    listeners.get("Page.frameNavigated")({
      frame: { id: "subframe", parentId: "main", loaderId: "sub-loader" },
    });
    await guard.assertUnchanged();
    currentUrl = "http://127.0.0.1/editor.html#/working";
    await guard.assertUnchanged();
    listeners.get("Page.frameNavigated")({
      frame: { id: "main", loaderId: "loader-2", url: currentUrl },
    });
    await assert.rejects(
      guard.assertUnchanged(),
      (error) => error.code ===
        "browser-interaction-navigation-rejected");
    assert.equal(
      calls.some(({ method }) => method === "Page.stopLoading"), true);
  });

test("main-frame navigation guard closes popup pages before they run",
  async () => {
    const listeners = new Map();
    const calls = [];
    const cdp = {
      async call(method, params, sessionId) {
        calls.push({ method, params, sessionId });
        if (method === "Page.getFrameTree") {
          return {
            frameTree: {
              frame: {
                id: "main",
                loaderId: "loader-1",
                url: "http://127.0.0.1/editor.html",
              },
            },
          };
        }
        if (method === "Runtime.evaluate") {
          return { result: { value: params.expression.startsWith("(() =>") } };
        }
        return {};
      },
      on(method, listener) {
        listeners.set(method, listener);
      },
    };
    const guard = await createMainFrameNavigationGuard(cdp);
    assert.deepEqual(
      calls.find(({ method }) => method === "Target.setAutoAttach").params,
      {
        autoAttach: true,
        waitForDebuggerOnStart: true,
        flatten: true,
        filter: [{ type: "page", exclude: false }],
      });
    listeners.get("Target.attachedToTarget")({
      sessionId: "popup-session-1",
      targetInfo: { targetId: "popup-1", type: "page" },
    });
    await assert.rejects(
      guard.assertUnchanged(),
      (error) => error.code ===
        "browser-interaction-navigation-rejected");
    assert.deepEqual(
      calls.find(({ method }) => method === "Page.close"),
      {
        method: "Page.close",
        params: {},
        sessionId: "popup-session-1",
      });
  });
