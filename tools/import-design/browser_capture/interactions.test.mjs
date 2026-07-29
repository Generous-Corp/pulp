// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  MAX_INTERACTION_ACTIONS,
  MAX_ACTION_TIMEOUT_MS,
  MAX_SELECTOR_LENGTH,
  MAX_TYPE_TEXT_LENGTH,
  MAX_WAIT_MS,
  parseInteractionPlan,
} from "./interaction_plan.mjs";
import { executeInteractionPlan } from "./interaction_executor.mjs";

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
    { action: "type", selector: "[name=query]", text: "delay" },
    {
      action: "wait-for",
      selector: "[data-screen=patch]",
      state: "visible",
      timeout_ms: 9000,
    },
    { action: "wait-ms", milliseconds: 25 },
  ]);
  assert.equal(parsed.actions.length, 4);
  assert.equal(parsed.actions[0].timeout_ms, 5000);
  assert.match(parsed.sha256, /^[0-9a-f]{64}$/);
});

test("published interaction schema matches enforced parser bounds", async () => {
  const schema = JSON.parse(await readFile(fileURLToPath(
    new URL("./interaction_plan_protocol.json", import.meta.url)), "utf8"));
  const actions = schema.properties.actions;
  assert.equal(actions.maxItems, MAX_INTERACTION_ACTIONS);

  const definitions = Object.fromEntries(actions.items.oneOf.map(
    (definition) => [definition.properties.action.const, definition]));
  assert.deepEqual(Object.keys(definitions).sort(),
    ["click", "type", "wait-for", "wait-ms"]);
  for (const name of ["click", "type", "wait-for"]) {
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
  ]), /must be click, type, wait-for, or wait-ms/);
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
    { action: "type", selector: "input", text: "private draft" },
    { action: "wait-for", selector: "#screen", state: "visible" },
    { action: "wait-ms", milliseconds: 2 },
  ]), {
    delay: async () => {},
    settle: async () => { settles += 1; },
  });

  assert.equal(report.action_count, 4);
  assert.equal(report.actions[1].text_length, 13);
  assert.equal("text_sha256" in report.actions[1], false);
  assert.equal("text" in report.actions[1], false);
  assert.equal(JSON.stringify(report).includes("private draft"), false);
  assert.equal(settles, 2);
  assert.equal(
    calls.filter(({ method }) => method === "Input.dispatchMouseEvent").length,
    2);
  assert.deepEqual(
    calls.find(({ method }) => method === "Input.insertText").params,
    { text: "private draft" });
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
