// SPDX-License-Identifier: MIT

const HEALTH_EXPRESSION = `(() => {
  let visibleElements = 0;
  let visibleTextCharacters = 0;
  let paintedSurfaces = 0;
  for (const element of document.querySelectorAll(
      'body *:not(script):not(style):not(link):not(meta):not(template)')) {
    const style = getComputedStyle(element);
    const rect = element.getBoundingClientRect();
    if (style.display === 'none' || style.visibility === 'hidden' ||
        Number(style.opacity || 1) <= 0.001 ||
        rect.width <= 0.25 || rect.height <= 0.25) continue;
    visibleElements++;
    if (element.childElementCount === 0) {
      visibleTextCharacters +=
        (element.textContent || '').replace(/\\s+/g, '').length;
    }
    if (element.matches('canvas,svg,img,video') ||
        style.backgroundImage !== 'none' ||
        style.backgroundColor !== 'rgba(0, 0, 0, 0)') {
      paintedSurfaces++;
    }
  }
  return {
    visible_elements: visibleElements,
    visible_text_characters: visibleTextCharacters,
    painted_surfaces: paintedSurfaces
  };
})()`;

function exceptionMessage(event) {
  const details = event?.exceptionDetails ?? {};
  return details.exception?.description || details.text || "runtime exception";
}

function diagnosticUrl(value) {
  try {
    const url = new URL(String(value || ""));
    if (url.protocol === "pulp-capture:") {
      return `pulp-capture://${url.pathname}`;
    }
    if (url.protocol === "http:" || url.protocol === "https:") {
      return url.origin;
    }
    return url.protocol || "<redacted-url>";
  } catch {
    return "<redacted-url>";
  }
}

export function installCaptureHealthMonitor(cdp) {
  const health = {
    runtime_exceptions: [],
    log_errors: [],
    failed_responses: [],
  };
  cdp.on("Runtime.exceptionThrown", (event) => {
    health.runtime_exceptions.push(exceptionMessage(event).slice(0, 1000));
  });
  cdp.on("Log.entryAdded", ({ entry }) => {
    if (entry?.level === "error") {
      health.log_errors.push(String(entry.text || "browser error").slice(0, 1000));
    }
  });
  cdp.on("Network.responseReceived", ({ response }) => {
    if (Number(response?.status) >= 400) {
      health.failed_responses.push({
        // Response URLs can contain credentials, signed queries, fragments,
        // or project-private paths. Capture evidence only needs a safe origin
        // (or the local pulp-capture route) to diagnose the failed resource.
        url: diagnosticUrl(response.url),
        status: Number(response.status),
      });
    }
  });
  return health;
}

function blockedHint(blockedRequests) {
  if (!blockedRequests.length) return "";
  const urls = [...new Set(blockedRequests.map((item) => item.url))]
    .slice(0, 4)
    .join(", ");
  return ` Required external resources were blocked (${urls}). ` +
    "Review the source, then retry with --allow-browser-network.";
}

export async function verifyCaptureHealth(
    cdp, snapshot, monitor, blockedRequests) {
  const evaluated = await cdp.call("Runtime.evaluate", {
    expression: HEALTH_EXPRESSION,
    returnByValue: true,
  });
  const visual = evaluated.result?.value ?? {};
  const layoutCount =
    snapshot.documents?.[0]?.layout?.nodeIndex?.length ?? 0;
  const healthy = layoutCount > 3 &&
    ((visual.visible_elements ?? 0) > 1 ||
     (visual.painted_surfaces ?? 0) > 0 ||
     (visual.visible_text_characters ?? 0) > 0);
  const executionComplete =
    monitor.runtime_exceptions.length === 0 &&
    monitor.log_errors.length === 0 &&
    monitor.failed_responses.length === 0 &&
    blockedRequests.length === 0;

  const report = {
    status: healthy && executionComplete ? "healthy" : "unresolved",
    layout_count: layoutCount,
    visible_elements: visual.visible_elements ?? 0,
    visible_text_characters: visual.visible_text_characters ?? 0,
    painted_surfaces: visual.painted_surfaces ?? 0,
    runtime_exceptions: monitor.runtime_exceptions,
    log_errors: monitor.log_errors,
    failed_responses: monitor.failed_responses,
  };
  if (!healthy || !executionComplete) {
    const failedResponse = monitor.failed_responses[0];
    const firstError =
      monitor.runtime_exceptions[0] ||
      (failedResponse
        ? `${failedResponse.status} response from ${failedResponse.url}`
        : monitor.log_errors[0] || "");
    const error = new Error(
      (healthy
        ? "the source did not execute cleanly enough for a faithful capture"
        : "the source produced no meaningful visible design") +
      (firstError ? `; browser reported: ${firstError}` : "") +
      blockedHint(blockedRequests));
    error.code = "capture-source-unresolved";
    error.health = report;
    throw error;
  }
  return report;
}
