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

// Whether encoded image bytes animate, and in which container: "gif", "apng"
// or "webp" for an image with more than one frame, null for a still image or
// bytes this does not recognize. Self-contained, because the motion freeze
// below runs its source inside the page.
export function animatedImageKind(bytes) {
  const n = bytes.length;
  const at = (offset, text) => {
    if (offset + text.length > n) return false;
    for (let i = 0; i < text.length; i += 1) {
      if (bytes[offset + i] !== text.charCodeAt(i)) return false;
    }
    return true;
  };
  if (at(0, "GIF87a") || at(0, "GIF89a")) {
    if (n < 13) return null;
    const colorTable = (packed) =>
      (packed & 0x80) ? 3 * (1 << ((packed & 7) + 1)) : 0;
    let p = 13 + colorTable(bytes[10]);
    const skipSubBlocks = () => {
      while (p < n) {
        const size = bytes[p++];
        if (size === 0) return true;
        p += size;
      }
      return false;
    };
    let frames = 0;
    while (p < n) {
      const block = bytes[p++];
      if (block === 0x21) {
        p += 1;
        if (!skipSubBlocks()) return null;
      } else if (block === 0x2c) {
        frames += 1;
        if (frames > 1) return "gif";
        if (p + 9 > n) return null;
        p += 9 + colorTable(bytes[p + 8]) + 1;
        if (!skipSubBlocks()) return null;
      } else {
        return null;
      }
    }
    return null;
  }
  if (at(0, "\x89PNG\r\n\x1a\n")) {
    const u32 = (offset) => ((bytes[offset] << 24) | (bytes[offset + 1] << 16) |
      (bytes[offset + 2] << 8) | bytes[offset + 3]) >>> 0;
    // acTL must precede the first IDAT; its first field is the frame count.
    for (let p = 8; p + 8 <= n; p += 12 + u32(p)) {
      if (at(p + 4, "acTL")) return p + 12 <= n && u32(p + 8) > 1 ? "apng" : null;
      if (at(p + 4, "IDAT")) return null;
    }
    return null;
  }
  if (at(0, "RIFF") && at(8, "WEBP")) {
    let frames = 0;
    for (let p = 12; p + 8 <= n;) {
      const size = (bytes[p + 4] | (bytes[p + 5] << 8) | (bytes[p + 6] << 16) |
        (bytes[p + 7] << 24)) >>> 0;
      if (at(p, "ANMF")) {
        frames += 1;
        if (frames > 1) return "webp";
      }
      p += 8 + size + (size & 1);
    }
    return null;
  }
  return null;
}

// Images the freeze reads to classify, and the byte budget for each. Both
// bound the work on a page that references thousands of images.
const MAX_MOTION_IMAGE_SOURCES = 256;
const MAX_MOTION_IMAGE_BYTES = 32 * 1024 * 1024;

const MOTION_FREEZE_SOURCE = `async (declareSources) => {
  const animatedImageKind = ${animatedImageKind.toString()};
  // Every pass over one document shares this state, so an animation or video
  // is counted once however often it is settled, and the receipt is the
  // document's total.
  const state = globalThis.__pulpMotionFreeze ??= {
    settled: new WeakSet(),
    rewound: new WeakSet(),
    receipt: {
      schema: "pulp-browser-capture-motion-v1",
      animations: { finished: 0, paused: 0, left_to_css_freeze: 0, not_time_driven: 0 },
      videos: { paused: 0, rewound: 0, poster: 0 },
      volatile_sources: [],
      images_examined: 0,
      images_unreadable: 0,
    },
  };
  const receipt = state.receipt;

  // Web Animations run on the document timeline, which the CSS freeze and the
  // scheduler stubs cannot reach. Settle each one to a single frame: a finite
  // animation to its end state, which is where the page comes to rest, and an
  // infinite or unresolved one to its start, which is the only frame it is
  // guaranteed to show. CSS animations and transitions keep their own path:
  // the motion stylesheet already removed them, and finishing one here would
  // replace that rule with a different end state.
  for (const animation of document.getAnimations ? document.getAnimations() : []) {
    // Settled before and not restarted by the page since.
    const first = !state.settled.has(animation);
    if (!first && animation.playState !== "running") continue;
    state.settled.add(animation);
    const count = (kind) => { if (first) receipt.animations[kind] += 1; };
    if ((typeof CSSAnimation === "function" && animation instanceof CSSAnimation) ||
        (typeof CSSTransition === "function" && animation instanceof CSSTransition)) {
      count("left_to_css_freeze");
      continue;
    }
    if (!(animation.timeline instanceof DocumentTimeline)) {
      // A scroll- or view-driven animation is a function of scroll position,
      // which the capture has already fixed; a null timeline never advances.
      count("not_time_driven");
      continue;
    }
    const end = animation.effect?.getComputedTiming?.().endTime;
    if (Number.isFinite(end) && animation.playbackRate !== 0) {
      try {
        animation.finish();
        count("finished");
        continue;
      } catch {}
    }
    animation.pause();
    animation.currentTime = 0;
    count("paused");
  }

  // A video that never presented a frame shows its poster; leave that frame.
  // Any other video is stopped on its first frame.
  const seeks = [];
  for (const video of document.querySelectorAll("video")) {
    const first = !state.settled.has(video);
    if (!first && video.paused) continue;
    state.settled.add(video);
    video.autoplay = false;
    video.pause();
    if (first) receipt.videos.paused += 1;
    if (video.poster && video.played.length === 0 && video.currentTime === 0) {
      if (first) receipt.videos.poster += 1;
      continue;
    }
    if (video.readyState >= 1 && video.currentTime !== 0) {
      seeks.push(new Promise((resolve) =>
        video.addEventListener("seeked", resolve, { once: true })));
      video.currentTime = 0;
      if (!state.rewound.has(video)) receipt.videos.rewound += 1;
      state.rewound.add(video);
    }
  }
  if (seeks.length > 0) {
    await Promise.race([
      Promise.all(seeks),
      new Promise((resolve) => setTimeout(resolve, 2000)),
    ]);
  }

  // An animated GIF, APNG or WebP advances on the image decoder's own clock,
  // which nothing in the page can pause. It is left to animate and declared,
  // so a consumer knows these pixels are one frame of several.
  if (!declareSources) return receipt;
  receipt.volatile_sources = [];
  receipt.images_examined = 0;
  receipt.images_unreadable = 0;
  const sources = new Map();
  const add = (url, element) => {
    if (!url || sources.size >= ${MAX_MOTION_IMAGE_SOURCES} || sources.has(url)) return;
    sources.set(url, element);
  };
  const cssUrls = /url\\(\\s*(['"]?)(.*?)\\1\\s*\\)/g;
  const imageProperties = ["background-image", "border-image-source",
    "list-style-image", "mask-image", "-webkit-mask-image", "content"];
  for (const element of document.querySelectorAll("*")) {
    const name = element.localName;
    if (name === "img") add(element.currentSrc || element.src, name);
    else if (name === "input" && element.type === "image") add(element.src, name);
    else if (name === "video" && element.poster) add(element.poster, "video-poster");
    else if (name === "image" && element.href?.baseVal) {
      try { add(new URL(element.href.baseVal, document.baseURI).href, name); } catch {}
    }
    for (const pseudo of [null, "::before", "::after"]) {
      const style = getComputedStyle(element, pseudo);
      for (const property of imageProperties) {
        const value = style.getPropertyValue(property);
        if (!value || !value.includes("url(")) continue;
        for (const match of value.matchAll(cssUrls)) {
          add(match[2], pseudo ? name + pseudo : name);
        }
      }
    }
  }
  for (const [url, element] of sources) {
    receipt.images_examined += 1;
    let bytes;
    try {
      const response = await fetch(url, { cache: "force-cache" });
      if (!response.ok) throw new Error(String(response.status));
      const buffer = await response.arrayBuffer();
      if (buffer.byteLength > ${MAX_MOTION_IMAGE_BYTES}) throw new Error("too large");
      bytes = new Uint8Array(buffer);
    } catch {
      receipt.images_unreadable += 1;
      continue;
    }
    const format = animatedImageKind(bytes);
    if (!format) continue;
    receipt.volatile_sources.push({
      kind: "animated-image",
      format,
      element,
      url: url.startsWith("data:") ? url.slice(0, url.search(/[;,]/)) : url,
      byte_length: bytes.length,
    });
  }
  return receipt;
}`;

// Stops the motion that keeps a still page from presenting one frame and
// returns what it did, for the capture receipt. A page-settle runs it as soon
// as the document has loaded, so a Web Animation that moves a box cannot keep
// the layout from settling, and the capture runs it once more after every
// settle and before the frame is read, for whatever started since. Only that
// last pass, `declareSources`, reads the page's images to declare animated
// ones. Each pass settles only what an earlier one did not, or what the page
// has restarted since.
export async function freezeMotion(cdp, { declareSources = true } = {}) {
  const evaluated = await cdp.call("Runtime.evaluate", {
    expression: `(${MOTION_FREEZE_SOURCE})(${declareSources === true})`,
    awaitPromise: true,
    returnByValue: true,
  });
  if (evaluated.exceptionDetails || !evaluated.result?.value) {
    const error = new Error(
      "the page's motion could not be frozen: " +
      (evaluated.exceptionDetails?.exception?.description ??
        evaluated.exceptionDetails?.text ?? "no receipt"));
    error.code = "capture-motion-freeze-failed";
    throw error;
  }
  return evaluated.result.value;
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

// A horizon accepts its last frame when the final STABLE_RUN_FRAMES frames are
// byte-identical. The run counter resets on any change and the loop never
// exits early, so that verdict, and the bytes returned, are a function of the
// final STABLE_RUN_FRAMES frames alone. Every earlier frame exists only to push
// the decisive ones past that many fresh compositor presentations (an early
// plateau must not hide a later change), and nothing reads its bytes.
export const SCREENSHOT_HORIZON_FRAMES = 32;
export const STABLE_RUN_FRAMES = 3;

// The output options of a presentation-only frame. Encoding happens after
// readback, so these change the cost of a frame and not which pixels are read.
// Everything that describes geometry (clip, captureBeyondViewport, fromSurface)
// is left exactly as the caller passed it: a geometry change resizes the
// capture surface and is visible to the page.
export const PRESENTATION_FRAME_ENCODING = Object.freeze({
  format: "jpeg",
  quality: 1,
});

export function presentationFrameOptions(
  screenshotOptions, encoding = PRESENTATION_FRAME_ENCODING) {
  return { ...screenshotOptions, ...encoding };
}

// The presentation encoding for a capture whose motion has been frozen, or
// null to take every frame in the caller's encoding. An animated image is the
// one motion the freeze leaves running, on the decoder's wall clock, and its
// verdict depends on when the decisive frames land. Cheaper presentation
// frames move them earlier, so a page that declares one keeps the horizon's
// full wall duration and the verdict it had.
export function horizonPresentationEncoding(motion) {
  return motion?.volatile_sources?.length > 0 ? null : PRESENTATION_FRAME_ENCODING;
}

// The decisive frames, exactly `requiredTrailingFrames` of them, are taken with
// the caller's own options and compared byte for byte, so they are the frames
// the rule judges and the bytes the capture ships. Because their count equals
// the required run, a presentation frame before them can never extend the run
// that decides: the first decisive frame always restarts it, as any change
// would. Requiring a longer run than the decisive frames would let encoding
// break that equivalence, so the two are the same parameter. A null
// `presentationEncoding` takes every frame with the caller's options.
export async function captureStableScreenshot(
  cdp, screenshotOptions, maximumAttempts = SCREENSHOT_HORIZON_FRAMES,
  intervalMs = 16, requiredTrailingFrames = STABLE_RUN_FRAMES,
  presentationEncoding = PRESENTATION_FRAME_ENCODING) {
  const presentationFrames = presentationEncoding
    ? Math.max(0, maximumAttempts - requiredTrailingFrames) : 0;
  const presentationOptions =
    presentationFrameOptions(screenshotOptions, presentationEncoding ?? {});
  let previous;
  let trailingFrames = 0;
  for (let attempt = 0; attempt < maximumAttempts; attempt += 1) {
    const screenshot = await cdp.call(
      "Page.captureScreenshot",
      attempt < presentationFrames ? presentationOptions : screenshotOptions);
    const current = Buffer.from(screenshot.data, "base64");
    trailingFrames =
      previous?.equals(current) ? trailingFrames + 1 : 1;
    previous = current;
    if (attempt + 1 < maximumAttempts) await delay(intervalMs);
  }
  return trailingFrames >= requiredTrailingFrames ? previous : undefined;
}

// The stable frame of a page with some of its paint hidden. When nothing was
// hidden the page is the one that produced `acceptedBytes`, so those exact
// bytes are the answer: proving the same frame again would spend a second
// full screenshot horizon, each frame of which waits for a fresh compositor
// presentation, and on a loaded machine that repeat alone can outlast the
// capture deadline. Only a count the page positively reported as zero is
// reused; anything else is observed, with one fresh horizon if the first
// misses its identical tail.
export async function captureStableFrameWithHiddenPaint(
  cdp, screenshotOptions, acceptedBytes, hiddenCount,
  presentationEncoding = PRESENTATION_FRAME_ENCODING) {
  if (hiddenCount === 0) return acceptedBytes;
  const horizon = () => captureStableScreenshot(cdp, screenshotOptions,
    undefined, undefined, undefined, presentationEncoding);
  return await horizon() ?? await horizon();
}

export async function waitForStable(cdp, options = {}) {
  const stableRoundsRequired = options.stableRounds ?? 3;
  const maximumRounds = options.maximumRounds ?? 50;
  const intervalMs = options.intervalMs ?? 100;
  const minimumElapsedMs = options.minimumElapsedMs ?? 0;
  const networkIdle = options.networkIdle ?? (() => true);
  // Runs before every sample, so work the page starts mid-settle is handled
  // before the round that would see it move.
  const beforeSample = options.beforeSample ?? (async () => {});
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
    await beforeSample();
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
