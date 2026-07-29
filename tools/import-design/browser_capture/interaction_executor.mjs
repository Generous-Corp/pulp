// SPDX-License-Identifier: MIT
import { createHash } from "node:crypto";

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function evaluate(cdp, expression) {
  const result = await cdp.call("Runtime.evaluate", {
    expression,
    awaitPromise: true,
    returnByValue: true,
  });
  if (result.exceptionDetails) {
    throw new Error(
      result.exceptionDetails.exception?.description ??
      result.exceptionDetails.text ??
      "browser interaction evaluation failed");
  }
  return result.result?.value;
}

function selectorProbeExpression(selector, operation) {
  return `(() => {
    const selector = ${JSON.stringify(selector)};
    let element;
    try {
      element = document.querySelector(selector);
    } catch (cause) {
      return { ok: false, error: "invalid CSS selector: " + cause.message };
    }
    if (!element) return { ok: false, state: "detached" };
    const style = getComputedStyle(element);
    const rect = element.getBoundingClientRect();
    const inert = Boolean(element.closest("[inert]"));
    const visible = !element.hidden && !inert &&
      style.display !== "none" && style.visibility !== "hidden" &&
      Number(style.opacity) > 0 && rect.width > 0 && rect.height > 0;
    if (${JSON.stringify(operation)} === "observe") {
      return { ok: true, state: visible ? "visible" : "hidden" };
    }
    if (!visible) return { ok: false, state: "hidden" };
    element.scrollIntoView({ block: "center", inline: "center" });
    const positioned = element.getBoundingClientRect();
    const x = Math.max(0, Math.min(innerWidth - 1,
      positioned.left + positioned.width / 2));
    const y = Math.max(0, Math.min(innerHeight - 1,
      positioned.top + positioned.height / 2));
    if (${JSON.stringify(operation)} === "type") {
      const tag = element.tagName.toLowerCase();
      const textInputTypes = new Set([
        "text", "search", "url", "tel", "email", "number"
      ]);
      const editable = tag === "textarea" ||
        (tag === "input" && textInputTypes.has(element.type)) ||
        element.isContentEditable;
      if (!editable) {
        return { ok: false, error: "target is not an editable text control" };
      }
      if (element.disabled || element.readOnly) {
        return { ok: false, error: "target is disabled or read-only" };
      }
      element.focus();
      if (typeof element.select === "function") element.select();
      else {
        const selection = getSelection();
        const range = document.createRange();
        range.selectNodeContents(element);
        selection.removeAllRanges();
        selection.addRange(range);
      }
      return { ok: true, x, y };
    }
    if (element.disabled || style.pointerEvents === "none") {
      return { ok: false, error: "target is disabled or ignores pointer events" };
    }
    const hit = document.elementFromPoint(x, y);
    if (!hit || !(element === hit || element.contains(hit) ||
                  hit.contains(element))) {
      return { ok: false, error: "target is covered by another rendered element" };
    }
    return { ok: true, x, y };
  })()`;
}

function actionError(index, action, detail) {
  const error = new Error(
    `browser interaction ${index + 1} (${action.action}) failed: ${detail}`);
  error.code = "browser-interaction-failed";
  return error;
}

async function probeUntil(cdp, action, index, operation, predicate, wait) {
  const deadline = Date.now() + action.timeout_ms;
  let last;
  do {
    last = await evaluate(
      cdp, selectorProbeExpression(action.selector, operation));
    if (last?.error) throw actionError(index, action, last.error);
    if (predicate(last)) return last;
    await wait(Math.min(50, Math.max(0, deadline - Date.now())));
  } while (Date.now() < deadline);
  throw actionError(
    index, action,
    `selector ${JSON.stringify(action.selector)} did not reach the required state`);
}

function publicActionEvidence(action) {
  const evidence = { action: action.action };
  if (action.selector !== undefined) evidence.selector = action.selector;
  if (action.timeout_ms !== undefined) evidence.timeout_ms = action.timeout_ms;
  if (action.state !== undefined) evidence.state = action.state;
  if (action.milliseconds !== undefined) {
    evidence.milliseconds = action.milliseconds;
  }
  if (action.text !== undefined) {
    evidence.text_length = action.text.length;
    evidence.text_sha256 =
      createHash("sha256").update(action.text, "utf8").digest("hex");
  }
  evidence.status = "completed";
  return evidence;
}

export async function executeInteractionPlan(cdp, plan, options = {}) {
  const wait = options.delay ?? delay;
  const settle = options.settle ?? (async () => {});
  const completed = [];

  for (let index = 0; index < plan.actions.length; index += 1) {
    const action = plan.actions[index];
    if (action.action === "wait-ms") {
      await wait(action.milliseconds);
    } else if (action.action === "wait-for") {
      await probeUntil(
        cdp, action, index, "observe",
        (result) => {
          if (action.state === "attached") return result?.ok === true;
          if (action.state === "detached") return result?.state === "detached";
          if (action.state === "visible") return result?.state === "visible";
          return result?.state === "hidden" || result?.state === "detached";
        },
        wait);
    } else if (action.action === "click") {
      const target = await probeUntil(
        cdp, action, index, "click", (result) => result?.ok === true, wait);
      await cdp.call("Input.dispatchMouseEvent", {
        type: "mousePressed", x: target.x, y: target.y,
        button: "left", buttons: 1, clickCount: 1,
      });
      await cdp.call("Input.dispatchMouseEvent", {
        type: "mouseReleased", x: target.x, y: target.y,
        button: "left", buttons: 0, clickCount: 1,
      });
      await settle();
    } else if (action.action === "type") {
      await probeUntil(
        cdp, action, index, "type", (result) => result?.ok === true, wait);
      await cdp.call("Input.insertText", { text: action.text });
      await settle();
    }
    completed.push(publicActionEvidence(action));
  }

  return {
    schema: plan.schema,
    version: plan.version,
    plan_sha256: plan.sha256,
    action_count: completed.length,
    actions: completed,
  };
}
