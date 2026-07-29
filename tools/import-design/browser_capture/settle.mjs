// SPDX-License-Identifier: MIT

export const MAX_LOGICAL_CAPTURE_DIMENSION = 8192;
export const MAX_CAPTURE_DEVICE_PIXELS = 64 * 1024 * 1024;

export function validateCaptureDimensions(
  width, height, dpr, subject = "capture viewport") {
  const devicePixels = width * height * dpr * dpr;
  if (!Number.isInteger(width) || width <= 0 ||
      !Number.isInteger(height) || height <= 0 ||
      !Number.isInteger(dpr) || dpr <= 0 ||
      width > MAX_LOGICAL_CAPTURE_DIMENSION ||
      height > MAX_LOGICAL_CAPTURE_DIMENSION ||
      devicePixels > MAX_CAPTURE_DEVICE_PIXELS) {
    const error = new Error(
      `${subject} ${width}x${height} at DPR ${dpr} exceeds the ` +
      "8192px axis or 64 megapixel safety limit");
    error.code = "capture-viewport-too-large";
    throw error;
  }
}

const SAMPLE_EXPRESSION = `(() => {
  const root = document.documentElement;
  const body = document.body;
  let visible = 0;
  let geometry = 2166136261 >>> 0;
  let textLength = 0;
  const hash = value => {
    geometry ^= value >>> 0;
    geometry = Math.imul(geometry, 16777619) >>> 0;
  };
  for (const element of document.querySelectorAll('*')) {
    const style = getComputedStyle(element);
    const rect = element.getBoundingClientRect();
    if (style.display === 'none' || style.visibility === 'hidden' ||
        Number(style.opacity || 1) <= 0.001 ||
        rect.width <= 0.25 || rect.height <= 0.25) continue;
    visible++;
    hash(Math.round(rect.left * 4));
    hash(Math.round(rect.top * 4));
    hash(Math.round(rect.width * 4));
    hash(Math.round(rect.height * 4));
    textLength += element.childNodes.length === 1 &&
      element.firstChild?.nodeType === Node.TEXT_NODE
      ? (element.textContent?.length || 0) : 0;
  }
  return JSON.stringify({
    ready: document.readyState,
    fonts: document.fonts ? document.fonts.status : 'loaded',
    width: Math.max(root?.scrollWidth || 0, body?.scrollWidth || 0),
    height: Math.max(root?.scrollHeight || 0, body?.scrollHeight || 0),
    visible,
    geometry,
    textLength
  });
})()`;

const EXTENT_EXPRESSION = `(() => {
  const root = document.documentElement;
  const body = document.body;
  let left = 0;
  let top = 0;
  let right = Math.max(root?.scrollWidth || 0, body?.scrollWidth || 0, 1);
  let bottom = Math.max(root?.scrollHeight || 0, body?.scrollHeight || 0, 1);
  let primary = { left: 0, top: 0, width: right, height: bottom, area: 0 };
  for (const element of document.querySelectorAll('body *')) {
    const style = getComputedStyle(element);
    const rect = element.getBoundingClientRect();
    if (style.display === 'none' || style.visibility === 'hidden' ||
        Number(style.opacity || 1) <= 0.001 ||
        rect.width <= 0.25 || rect.height <= 0.25) continue;
    left = Math.min(left, rect.left + window.scrollX);
    top = Math.min(top, rect.top + window.scrollY);
    right = Math.max(right, rect.right + window.scrollX);
    bottom = Math.max(bottom, rect.bottom + window.scrollY);
    const area = rect.width * rect.height;
    if (area > primary.area) {
      primary = {
        left: rect.left + window.scrollX,
        top: rect.top + window.scrollY,
        width: rect.width,
        height: rect.height,
        area
      };
    }
  }
  return {
    left: Math.floor(left),
    top: Math.floor(top),
    width: Math.ceil(right),
    height: Math.ceil(bottom),
    primarySurface: {
      left: primary.left,
      top: primary.top,
      width: primary.width,
      height: primary.height
    }
  };
})()`;

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function animationFrames(cdp, count) {
  await cdp.call("Runtime.evaluate", {
    expression: `new Promise(resolve => {
      let remaining = ${count};
      const step = () => {
        remaining -= 1;
        if (remaining <= 0) resolve(true);
        else requestAnimationFrame(step);
      };
      requestAnimationFrame(step);
    })`,
    awaitPromise: true,
    returnByValue: true,
  });
}

export async function disableMotion(cdp) {
  await cdp.call("Runtime.evaluate", {
    expression: `(() => {
      const id = '__pulp_browser_capture_motion__';
      let style = document.getElementById(id);
      if (!style) {
        style = document.createElement('style');
        style.id = id;
        style.textContent =
          '*,*::before,*::after{animation:none!important;' +
          'transition:none!important;caret-color:transparent!important;' +
          'scroll-behavior:auto!important}';
        (document.head || document.documentElement).appendChild(style);
      }
      return true;
    })()`,
    returnByValue: true,
  });
}

export async function freezeDynamicTime(cdp) {
  await cdp.call("Runtime.evaluate", {
    // One already-scheduled callback may still run, but its attempt to queue
    // the next frame reaches this no-op. Let that final presentation boundary
    // pass before pausing timers/virtual time.
    expression: `(() => {
      window.requestAnimationFrame = () => 0;
      window.cancelAnimationFrame = () => {};
      return true;
    })()`,
    returnByValue: true,
  });
  await delay(50);
  await cdp.call("Emulation.setVirtualTimePolicy", {
    policy: "pause",
  });
}

export async function waitForStable(cdp, options = {}) {
  const stableRoundsRequired = options.stableRounds ?? 3;
  const maximumRounds = options.maximumRounds ?? 50;
  const intervalMs = options.intervalMs ?? 100;
  const networkIdle = options.networkIdle ?? (() => true);
  const started = Date.now();

  await cdp.call("Runtime.evaluate", {
    expression: "document.fonts ? document.fonts.ready : Promise.resolve(true)",
    awaitPromise: true,
    returnByValue: true,
  });
  await animationFrames(cdp, 2);

  let previous = "";
  let stableRounds = 0;
  let rounds = 0;
  for (; rounds < maximumRounds; rounds++) {
    const evaluated = await cdp.call("Runtime.evaluate", {
      expression: SAMPLE_EXPRESSION,
      returnByValue: true,
    });
    const sample = evaluated.result?.value ?? "";
    let parsed;
    try {
      parsed = JSON.parse(sample);
    } catch {
      parsed = {};
    }
    const ready = parsed.ready === "complete" && parsed.fonts === "loaded" &&
      networkIdle();
    if (ready && sample === previous) stableRounds += 1;
    else stableRounds = 0;
    previous = sample;
    if (stableRounds >= stableRoundsRequired) break;
    await delay(intervalMs);
  }
  if (stableRounds < stableRoundsRequired) {
    const error = new Error(
      `document did not settle after ${maximumRounds} rounds`);
    error.code = "capture-not-stable";
    throw error;
  }
  return {
    rounds: rounds + 1,
    stableRounds,
    elapsedMs: Date.now() - started,
  };
}

export async function measureDocumentExtent(cdp, maximumLogicalSize = 8192) {
  const evaluated = await cdp.call("Runtime.evaluate", {
    expression: EXTENT_EXPRESSION,
    returnByValue: true,
  });
  const extent = evaluated.result?.value;
  if (!extent || !Number.isFinite(extent.width) ||
      !Number.isFinite(extent.height) ||
      extent.width < 1 || extent.height < 1) {
    const error = new Error("browser returned an invalid document extent");
    error.code = "capture-invalid-extent";
    throw error;
  }
  if (extent.width > maximumLogicalSize ||
      extent.height > maximumLogicalSize) {
    const error = new Error(
      `document extent ${extent.width}x${extent.height} exceeds the ` +
      `${maximumLogicalSize}px logical capture limit`);
    error.code = "capture-extent-too-large";
    throw error;
  }
  return extent;
}

export async function freezeAndMeasureDocumentExtent(
  cdp, maximumLogicalSize = 8192) {
  await freezeDynamicTime(cdp);
  return measureDocumentExtent(cdp, maximumLogicalSize);
}
