// SPDX-License-Identifier: MIT
import { createHash } from "node:crypto";
import { open } from "node:fs/promises";

export const INTERACTION_PLAN_SCHEMA = "pulp-browser-interactions-v1";
export const MAX_INTERACTION_PLAN_BYTES = 64 * 1024;
export const MAX_INTERACTION_ACTIONS = 32;
export const MAX_SELECTOR_LENGTH = 512;
export const MAX_TYPE_TEXT_LENGTH = 4096;
export const MAX_ACTION_TIMEOUT_MS = 10_000;
export const MAX_WAIT_MS = 5_000;
export const MAX_TOTAL_WAIT_MS = 30_000;

const ACTION_FIELDS = {
  click: new Set(["action", "selector", "timeout_ms"]),
  "context-click": new Set(["action", "selector", "timeout_ms"]),
  type: new Set(["action", "selector", "text", "timeout_ms"]),
  "wait-for": new Set(["action", "selector", "state", "timeout_ms"]),
  "wait-ms": new Set(["action", "milliseconds"]),
};
const WAIT_FOR_STATES = new Set(["attached", "detached", "visible", "hidden"]);

function fail(message) {
  const error = new Error(`invalid browser interaction plan: ${message}`);
  error.code = "browser-interaction-plan-invalid";
  throw error;
}

function plainObject(value, label) {
  if (value === null || typeof value !== "object" ||
      Array.isArray(value) || Object.getPrototypeOf(value) !== Object.prototype) {
    fail(`${label} must be an object`);
  }
}

function exactFields(value, allowed, label) {
  for (const field of Object.keys(value)) {
    if (!allowed.has(field)) fail(`${label} has unknown field "${field}"`);
  }
}

function boundedInteger(value, label, minimum, maximum) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    fail(`${label} must be an integer from ${minimum} to ${maximum}`);
  }
  return value;
}

function selector(value, label) {
  if (typeof value !== "string" || value.length === 0 ||
      value.length > MAX_SELECTOR_LENGTH) {
    fail(`${label} must be a non-empty CSS selector no longer than ` +
         `${MAX_SELECTOR_LENGTH} characters`);
  }
  return value;
}

function actionTimeout(value, label) {
  if (value === undefined) return 5_000;
  return boundedInteger(value, label, 1, MAX_ACTION_TIMEOUT_MS);
}

function redactedPlanIdentity(actions) {
  const redactedActions = actions.map((action) => {
    if (action.action !== "type") return action;
    const { text, ...publicAction } = action;
    return { ...publicAction, text_length: text.length };
  });
  return createHash("sha256").update(JSON.stringify({
    schema: INTERACTION_PLAN_SCHEMA,
    version: 1,
    actions: redactedActions,
  })).digest("hex");
}

export function parseInteractionPlan(raw) {
  const bytes = Buffer.isBuffer(raw) ? raw : Buffer.from(String(raw), "utf8");
  if (bytes.length === 0 || bytes.length > MAX_INTERACTION_PLAN_BYTES) {
    fail(`file must contain 1 to ${MAX_INTERACTION_PLAN_BYTES} bytes`);
  }

  let document;
  try {
    document = JSON.parse(bytes.toString("utf8"));
  } catch (error) {
    fail(`JSON could not be parsed: ${error.message}`);
  }
  plainObject(document, "root");
  exactFields(document, new Set(["schema", "version", "actions"]), "root");
  if (document.schema !== INTERACTION_PLAN_SCHEMA || document.version !== 1) {
    fail(`expected schema "${INTERACTION_PLAN_SCHEMA}" version 1`);
  }
  if (!Array.isArray(document.actions) || document.actions.length === 0 ||
      document.actions.length > MAX_INTERACTION_ACTIONS) {
    fail(`actions must contain 1 to ${MAX_INTERACTION_ACTIONS} entries`);
  }

  let totalWaitMs = 0;
  const actions = document.actions.map((candidate, index) => {
    const label = `actions[${index}]`;
    plainObject(candidate, label);
    const allowed = ACTION_FIELDS[candidate.action];
    if (!allowed) {
      fail(`${label}.action must be click, context-click, type, wait-for, or wait-ms`);
    }
    exactFields(candidate, allowed, label);

    if (candidate.action === "wait-ms") {
      const milliseconds = boundedInteger(
        candidate.milliseconds, `${label}.milliseconds`, 0, MAX_WAIT_MS);
      totalWaitMs += milliseconds;
      return { action: "wait-ms", milliseconds };
    }

    const normalized = {
      action: candidate.action,
      selector: selector(candidate.selector, `${label}.selector`),
      timeout_ms: actionTimeout(candidate.timeout_ms, `${label}.timeout_ms`),
    };
    if (candidate.action === "type") {
      if (typeof candidate.text !== "string" ||
          candidate.text.length > MAX_TYPE_TEXT_LENGTH) {
        fail(`${label}.text must be a string no longer than ` +
             `${MAX_TYPE_TEXT_LENGTH} characters`);
      }
      normalized.text = candidate.text;
    } else if (candidate.action === "wait-for") {
      normalized.state = candidate.state ?? "visible";
      if (!WAIT_FOR_STATES.has(normalized.state)) {
        fail(`${label}.state must be attached, detached, visible, or hidden`);
      }
    }
    return normalized;
  });
  if (totalWaitMs > MAX_TOTAL_WAIT_MS) {
    fail(`wait-ms actions total ${totalWaitMs}ms; maximum is ` +
         `${MAX_TOTAL_WAIT_MS}ms`);
  }

  return {
    schema: INTERACTION_PLAN_SCHEMA,
    version: 1,
    actions,
    // This durable identity deliberately excludes typed plaintext. Hashing the
    // raw plan would let an observer recover low-entropy text by testing
    // candidate plans against the published digest.
    sha256: redactedPlanIdentity(actions),
  };
}

export async function readInteractionPlan(file) {
  let handle;
  try {
    handle = await open(file, "r");
    const stat = await handle.stat();
    if (!stat.isFile()) fail("path is not a regular file");
    if (stat.size === 0 || stat.size > MAX_INTERACTION_PLAN_BYTES) {
      fail(`file must contain 1 to ${MAX_INTERACTION_PLAN_BYTES} bytes`);
    }
    return parseInteractionPlan(await handle.readFile());
  } catch (error) {
    if (error.code === "browser-interaction-plan-invalid") throw error;
    const wrapped = new Error(
      `could not read browser interaction plan: ${error.message}`);
    wrapped.code = "browser-interaction-plan-unreadable";
    throw wrapped;
  } finally {
    await handle?.close();
  }
}
