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
    textLength,
    mutationEpoch: globalThis.__pulpCaptureMutationEpoch || 0
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

function visualStabilityKey(sample) {
  let parsed;
  try {
    parsed = JSON.parse(sample);
  } catch {
    return sample;
  }
  // React applications may continuously replace equivalent DOM attributes or
  // nodes while their resolved presentation remains unchanged. The mutation
  // epoch is diagnostic evidence, not visual evidence: requiring it to stop
  // makes a stable menu or modal impossible to capture. Geometry, visibility,
  // text and document extent still have to agree here, and the later
  // byte-identical screenshot tail independently proves compositor stillness.
  delete parsed.mutationEpoch;
  return JSON.stringify(parsed);
}

const DYNAMIC_WORK_TRACKER_EXPRESSION = `(() => {
  const timeouts = new Set();
  const intervals = new Set();
  const frames = new Set();
  const nativeSetTimeout = window.setTimeout.bind(window);
  const nativeClearTimeout = window.clearTimeout.bind(window);
  const nativeSetInterval = window.setInterval.bind(window);
  const nativeClearInterval = window.clearInterval.bind(window);
  const nativeRequestAnimationFrame =
    window.requestAnimationFrame.bind(window);
  const nativeCancelAnimationFrame =
    window.cancelAnimationFrame.bind(window);
  // The accepted pixels are a particular animation frame, not just a stable
  // DOM layout. Preserve its presentation timestamp so native replay can
  // render the same canonical frame before switching to its live clock.
  globalThis.__pulpLastAnimationFrameTimestamp = 0;
  window.setTimeout = (callback, ...args) => {
    let handle;
    const trackedCallback = (...callbackArgs) => {
      timeouts.delete(handle);
      if (typeof callback === "function")
        return callback(...callbackArgs);
      return (0, eval)(String(callback));
    };
    handle = nativeSetTimeout(trackedCallback, ...args);
    timeouts.add(handle);
    return handle;
  };
  window.clearTimeout = (handle) => {
    timeouts.delete(handle);
    return nativeClearTimeout(handle);
  };
  window.setInterval = (...args) => {
    const handle = nativeSetInterval(...args);
    intervals.add(handle);
    return handle;
  };
  window.clearInterval = (handle) => {
    intervals.delete(handle);
    return nativeClearInterval(handle);
  };
  window.requestAnimationFrame = (callback) => {
    let handle;
    handle = nativeRequestAnimationFrame((timestamp) => {
      frames.delete(handle);
      if (Number.isFinite(timestamp))
        globalThis.__pulpLastAnimationFrameTimestamp = timestamp;
      callback(timestamp);
    });
    frames.add(handle);
    return handle;
  };
  window.cancelAnimationFrame = (handle) => {
    frames.delete(handle);
    return nativeCancelAnimationFrame(handle);
  };
  Object.defineProperty(window, "__pulpFreezeDynamicWork", {
    configurable: false,
    enumerable: false,
    value: () => {
      for (const handle of timeouts) nativeClearTimeout(handle);
      for (const handle of intervals) nativeClearInterval(handle);
      for (const handle of frames) nativeCancelAnimationFrame(handle);
      timeouts.clear();
      intervals.clear();
      frames.clear();
    },
  });
})()`;

export async function installDynamicWorkTracker(cdp) {
  await cdp.call("Page.addScriptToEvaluateOnNewDocument", {
    source: DYNAMIC_WORK_TRACKER_EXPRESSION,
  });
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
    // Stop tracked page work, then reject any scheduling after the freeze.
    // Let the final presentation boundary pass before pausing virtual time.
    expression: `(() => {
      window.__pulpFreezeDynamicWork?.();
      window.setTimeout = () => 0;
      window.clearTimeout = () => {};
      window.setInterval = () => 0;
      window.clearInterval = () => {};
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

export async function resumeDynamicTime(cdp) {
  // Paused virtual time suppresses the compositor's BeginFrame source, and
  // Chromium's screenshot path waits for a fresh presented frame. The first
  // Page.captureScreenshot after a pause still resolves because
  // captureBeyondViewport resizes the capture surface and forces one commit;
  // every subsequent call then waits forever for a frame that virtual time
  // will never produce. Pixels therefore have to be read with virtual time
  // running. Page-driven motion is already gone by this point — tracked
  // timers/intervals/rAFs are cancelled, the schedulers are stubbed, and CSS
  // animations and transitions are disabled — and the byte-identical trailing
  // run required by captureStableScreenshot is the observable proof of
  // stillness, so determinism does not rest on the pause.
  await cdp.call("Emulation.setVirtualTimePolicy", {
    policy: "advance",
  });
}

export async function captureStableScreenshot(
  cdp, screenshotOptions, maximumAttempts = 32, intervalMs = 16,
  requiredTrailingFrames = 3) {
  let previous;
  let trailingFrames = 0;
  for (let attempt = 0; attempt < maximumAttempts; attempt += 1) {
    const screenshot =
      await cdp.call("Page.captureScreenshot", screenshotOptions);
    const current = Buffer.from(screenshot.data, "base64");
    trailingFrames =
      previous?.equals(current) ? trailingFrames + 1 : 1;
    previous = current;
    if (attempt + 1 < maximumAttempts) await delay(intervalMs);
  }
  return trailingFrames >= requiredTrailingFrames ? previous : undefined;
}

export async function waitForStable(cdp, options = {}) {
  const stableRoundsRequired = options.stableRounds ?? 3;
  const maximumRounds = options.maximumRounds ?? 50;
  const intervalMs = options.intervalMs ?? 100;
  const minimumElapsedMs = options.minimumElapsedMs ?? 0;
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
  const recentSamples = [];
  for (; rounds < maximumRounds; rounds++) {
    const evaluated = await cdp.call("Runtime.evaluate", {
      expression: SAMPLE_EXPRESSION,
      returnByValue: true,
    });
    const sample = evaluated.result?.value ?? "";
    const stabilityKey = visualStabilityKey(sample);
    recentSamples.push(sample);
    if (recentSamples.length > 3) recentSamples.shift();
    let parsed;
    try {
      parsed = JSON.parse(sample);
    } catch {
      parsed = {};
    }
    const ready = parsed.ready === "complete" && parsed.fonts === "loaded" &&
      networkIdle();
    if (ready && stabilityKey === previous) stableRounds += 1;
    else stableRounds = 0;
    previous = stabilityKey;
    if (stableRounds >= stableRoundsRequired &&
        Date.now() - started >= minimumElapsedMs) break;
    await delay(intervalMs);
  }
  if (stableRounds < stableRoundsRequired) {
    const error = new Error(
      `document did not settle after ${maximumRounds} rounds; recent samples: ` +
      recentSamples.join(" -> "));
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

// Bound controls whose box escapes a clipping ancestor.
//
// The document-extent checks cannot see this. `overflow: hidden` means the
// content never grows the document, so a design that declares a 540px root and
// fills it with 900px measures as a clean 540px document and captures without
// complaint -- while a third of it, including controls bound to real
// parameters, is cut away. Content past the TOP or LEFT is refused outright as
// capture-negative-overflow; content past the BOTTOM was simply lost, and
// every stage downstream reported success.
//
// Deliberately scoped to BOUND CONTROLS rather than all content. Clipped text
// is ordinary and usually intentional -- `text-overflow: ellipsis` needs
// `overflow: hidden`, and design systems use it throughout -- so failing on it
// would reject correct panels. A control the user cannot reach is never
// intentional: it drives a parameter that can now only be automated.
const CLIPPED_CONTROLS_EXPRESSION = `(() => {
  const TOLERANCE_PX = 1;
  const clips = value =>
    value === 'hidden' || value === 'clip' ||
    value === 'scroll' || value === 'auto';
  const describe = element => {
    const name = String(element.className || '').trim().split(/\\s+/)[0];
    return name || element.tagName.toLowerCase();
  };
  const clipped = [];
  const controls =
    document.querySelectorAll('[data-pulp-param],[data-pulp-meter]');
  for (const control of controls) {
    const rect = control.getBoundingClientRect();
    if (rect.width <= 0.25 || rect.height <= 0.25) continue;
    for (let node = control.parentElement; node; node = node.parentElement) {
      const style = getComputedStyle(node);
      const clipsX = clips(style.overflowX);
      const clipsY = clips(style.overflowY);
      if (!clipsX && !clipsY) continue;
      const box = node.getBoundingClientRect();
      const lost = Math.max(
        clipsX ? box.left - rect.left : 0,
        clipsX ? rect.right - box.right : 0,
        clipsY ? box.top - rect.top : 0,
        clipsY ? rect.bottom - box.bottom : 0);
      if (lost > TOLERANCE_PX) {
        clipped.push({
          binding: control.getAttribute('data-pulp-param') ||
                   control.getAttribute('data-pulp-meter') || '',
          lost: Math.round(lost),
          by: describe(node)
        });
        break;
      }
    }
  }
  return clipped;
})()`;

export async function measureClippedControls(cdp) {
  const evaluated = await cdp.call("Runtime.evaluate", {
    expression: CLIPPED_CONTROLS_EXPRESSION,
    returnByValue: true,
  });
  const clipped = evaluated.result?.value;
  return Array.isArray(clipped) ? clipped : [];
}
