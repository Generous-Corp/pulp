// SPDX-License-Identifier: MIT

const RENDERER_HOOKS = [
  {
    name: "lucide",
    expression: `(() => {
      const placeholders =
        document.querySelectorAll('i[data-lucide]').length;
      if (placeholders === 0 ||
          typeof globalThis.lucide?.createIcons !== 'function') {
        return { applied: false, placeholders, remaining: placeholders };
      }
      globalThis.lucide.createIcons();
      return {
        applied: true,
        placeholders,
        remaining: document.querySelectorAll('i[data-lucide]').length
      };
    })()`,
  },
];

export async function awaitExplicitReadiness(
  cdp, contract = "__pulpCaptureReady") {
  const evaluated = await cdp.call("Runtime.evaluate", {
    expression: `(async () => {
      const contract = ${JSON.stringify(contract)};
      let ready = globalThis[contract];
      if (typeof ready === 'function') ready = ready();
      if (ready === undefined || ready === null) {
        return { contract: '', awaited: false };
      }
      await Promise.resolve(ready);
      return { contract, awaited: true };
    })()`,
    awaitPromise: true,
    returnByValue: true,
  });
  if (evaluated.exceptionDetails) {
    const message =
      evaluated.exceptionDetails.exception?.description ??
      evaluated.exceptionDetails.text ??
      "the page readiness contract rejected";
    const error = new Error(message);
    error.code = "capture-readiness-rejected";
    throw error;
  }
  return evaluated.result?.value ?? { contract: "", awaited: false };
}

export async function finalizeKnownRenderers(cdp) {
  const results = [];
  for (const hook of RENDERER_HOOKS) {
    const evaluated = await cdp.call("Runtime.evaluate", {
      expression: hook.expression,
      returnByValue: true,
    });
    const result = evaluated.result?.value ?? {};
    if (result.applied && result.remaining !== 0) {
      const error = new Error(
        `${hook.name} left ${result.remaining} unresolved placeholders`);
      error.code = "capture-renderer-not-ready";
      throw error;
    }
    if (result.applied) results.push({ name: hook.name, ...result });
  }
  return results;
}

export function mergeRendererHooks(...groups) {
  const merged = new Map();
  for (const hooks of groups) {
    for (const hook of hooks) {
      const prior = merged.get(hook.name);
      merged.set(hook.name, prior
        ? {
            ...hook,
            placeholders: prior.placeholders + hook.placeholders,
          }
        : { ...hook });
    }
  }
  return [...merged.values()];
}
