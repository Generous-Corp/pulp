// SPDX-License-Identifier: MIT

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
    const paintHitAt = (x, y) => {
      const override = document.createElement("style");
      override.textContent =
        "*,:before,:after{pointer-events:auto!important}";
      (document.head || document.documentElement).append(override);
      try {
        return document.elementFromPoint(x, y);
      } finally {
        override.remove();
      }
    };
    let visualTreeVisible = true;
    for (let ancestor = element; ancestor; ancestor = ancestor.parentElement) {
      const ancestorStyle = getComputedStyle(ancestor);
      if (ancestor.hidden || ancestor.inert ||
          ancestorStyle.display === "none" ||
          ancestorStyle.visibility === "hidden" ||
          ancestorStyle.visibility === "collapse" ||
          Number(ancestorStyle.opacity) <= 0) {
        visualTreeVisible = false;
        break;
      }
    }
    const hasUncoveredPoint = (candidate) => {
      const left = Math.max(0, candidate.left);
      const top = Math.max(0, candidate.top);
      const right = Math.min(innerWidth, candidate.right);
      const bottom = Math.min(innerHeight, candidate.bottom);
      if (right <= left || bottom <= top) return false;
      const insetX = Math.min(1, (right - left) / 4);
      const insetY = Math.min(1, (bottom - top) / 4);
      const points = [
        [(left + right) / 2, (top + bottom) / 2],
        [left + insetX, top + insetY],
        [right - insetX, top + insetY],
        [left + insetX, bottom - insetY],
        [right - insetX, bottom - insetY],
      ];
      return points.some(([x, y]) => {
        const hit = paintHitAt(x, y);
        return Boolean(hit && (element === hit || element.contains(hit)));
      });
    };
    const visible = visualTreeVisible && rect.width > 0 && rect.height > 0;
    if (${JSON.stringify(operation)} === "observe") {
      return {
        ok: true,
        state: visible && hasUncoveredPoint(rect) ? "visible" : "hidden"
      };
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
    const hit = paintHitAt(x, y);
    if (!hit || !(element === hit || element.contains(hit))) {
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
  }
  evidence.status = "completed";
  return evidence;
}

function navigationFailure() {
  const error = new Error(
    "main-frame navigation and popup pages are forbidden while browser interactions run");
  error.code = "browser-interaction-navigation-rejected";
  return error;
}

export async function createMainFrameNavigationGuard(cdp) {
  const initialTree = await cdp.call("Page.getFrameTree");
  const initialFrame = initialTree?.frameTree?.frame;
  if (!initialFrame?.id || typeof initialFrame.loaderId !== "string") {
    throw navigationFailure();
  }

  let violation = false;
  const popupBlock = await cdp.call("Runtime.evaluate", {
    expression: `(() => {
      globalThis.__pulpInteractionPopupAttempted = false;
      const reject = (event) => {
        const path = typeof event.composedPath === "function"
          ? event.composedPath()
          : [];
        const target = path.find((item) =>
          item instanceof HTMLAnchorElement ||
          item instanceof HTMLAreaElement ||
          item instanceof HTMLFormElement);
        const name = target?.target?.trim().toLowerCase() ?? "";
        if (name && !["_self", "_top", "_parent"].includes(name)) {
          globalThis.__pulpInteractionPopupAttempted = true;
          event.preventDefault();
          event.stopImmediatePropagation();
        }
      };
      addEventListener("click", reject, true);
      addEventListener("submit", reject, true);
      globalThis.open = () => {
        globalThis.__pulpInteractionPopupAttempted = true;
        return null;
      };
      return true;
    })()`,
    returnByValue: true,
  });
  if (popupBlock.exceptionDetails ||
      popupBlock.result?.value !== true) {
    throw navigationFailure();
  }
  const rejectPopup = (targetInfo) => {
    if (targetInfo?.type !== "page") return;
    violation = true;
    if (targetInfo.targetId) {
      Promise.resolve(cdp.call("Target.closeTarget", {
        targetId: targetInfo.targetId,
      })).catch(() => {});
    }
  };
  cdp.on("Page.windowOpen", () => {
    violation = true;
  });
  cdp.on("Target.attachedToTarget", ({ targetInfo }) => {
    rejectPopup(targetInfo);
  });
  await cdp.call("Target.setAutoAttach", {
    autoAttach: true,
    waitForDebuggerOnStart: false,
    flatten: true,
    filter: [{ type: "page", exclude: false }],
  });
  cdp.on("Page.frameNavigated", ({ frame }) => {
    if (frame?.id === initialFrame.id || !frame?.parentId) {
      violation = true;
      Promise.resolve(cdp.call("Page.stopLoading")).catch(() => {});
    }
  });

  return {
    async assertUnchanged() {
      if (violation) throw navigationFailure();
      const popupAttempted = await evaluate(
        cdp, "Boolean(globalThis.__pulpInteractionPopupAttempted)");
      if (popupAttempted) throw navigationFailure();
      const currentTree = await cdp.call("Page.getFrameTree");
      const currentFrame = currentTree?.frameTree?.frame;
      if (violation || currentFrame?.id !== initialFrame.id ||
          currentFrame?.loaderId !== initialFrame.loaderId) {
        throw navigationFailure();
      }
    },
  };
}

export async function executeInteractionPlan(cdp, plan, options = {}) {
  const wait = options.delay ?? delay;
  const settle = options.settle ?? (async () => {});
  const navigationGuard = options.navigationGuard ??
    (typeof cdp.on === "function"
      ? await createMainFrameNavigationGuard(cdp)
      : null);
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
    await navigationGuard?.assertUnchanged();
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
