#!/usr/bin/env node
// SPDX-License-Identifier: MIT
import { createHash } from "node:crypto";
import { writeSync } from "node:fs";
import {
  mkdir,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import path from "node:path";

import {
  authorizeInput,
  hostResolverRules,
  installNetworkGuard,
  normalizeDeclaredHttpsOrigins,
  resolvePublicHttpsOrigins,
  sanitizeCaptureError,
  sanitizeSnapshot,
  serveAuthorizedRoot,
  serveDenyProxy,
} from "./security.mjs";
import {
  createEmptyProfile,
  launchBrowser,
  pageTarget,
  terminateBrowser,
} from "./browser_process.mjs";
import {
  captureStableScreenshot,
  disableMotion,
  freezeAndMeasureDocumentExtent,
  installDynamicWorkTracker,
  MAX_LOGICAL_CAPTURE_DIMENSION,
  measureDocumentExtent,
  validateCaptureDimensions,
  waitForStable,
} from "./settle.mjs";
import {
  awaitExplicitReadiness,
  finalizeKnownRenderers,
  mergeRendererHooks,
} from "./renderers.mjs";
import {
  expandAuditedProviderDependencies,
} from "./network_dependencies.mjs";
import {
  COMPUTED_STYLES,
  evaluateSemantics,
} from "./semantics.mjs";
import {
  installCaptureHealthMonitor,
  verifyCaptureHealth,
} from "./health.mjs";
import {
  createMainFrameNavigationGuard,
  executeInteractionPlan,
} from "./interaction_executor.mjs";
import { readInteractionPlan } from "./interaction_plan.mjs";
import { armCleanupDeadline } from "./lifecycle.mjs";
import { evaluateDesignTokens } from "./tokens.mjs";

function parseArguments(argv) {
  const command = argv[0] ?? "";
  const values = new Map();
  const flags = new Set();
  const multiValues = new Map();
  for (let index = 1; index < argv.length; index++) {
    const token = argv[index];
    if (!token.startsWith("--")) {
      throw new Error(`unexpected argument: ${token}`);
    }
    if (token === "--allow-network" || token === "--allow-browser-network") {
      flags.add("--allow-network");
      continue;
    }
    if (index + 1 >= argv.length) {
      throw new Error(`missing value for ${token}`);
    }
    const value = argv[++index];
    if (token === "--declared-network-origin") {
      const existing = multiValues.get(token) ?? [];
      existing.push(value);
      multiValues.set(token, existing);
    } else {
      values.set(token, value);
    }
  }
  return { command, values, flags, multiValues };
}

function required(values, name) {
  const value = values.get(name);
  if (!value) throw new Error(`missing required argument ${name}`);
  return value;
}

function positiveInteger(values, name, fallback) {
  const raw = values.get(name);
  if (raw === undefined) return fallback;
  const value = Number(raw);
  if (!Number.isInteger(value) || value <= 0) {
    throw new Error(`${name} must be a positive integer`);
  }
  return value;
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function exitAfterCleanupDeadline() {
  try {
    writeSync(
      process.stderr.fd,
      "browser-capture-timeout: browser capture timed out\n");
  } catch {
    // The parent still sees exit 124 if its stderr pipe has already closed.
  }
  process.exit(124);
}

function withTimeout(promise, milliseconds, label) {
  let timer;
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(
        `${label} timed out after ${milliseconds}ms`)), milliseconds);
    }),
  ]).finally(() => clearTimeout(timer));
}

class Cdp {
  constructor(webSocketUrl, timeoutMs) {
    if (typeof WebSocket !== "function") {
      throw new Error("Node.js 22 or newer is required (WebSocket unavailable)");
    }
    this.socket = new WebSocket(webSocketUrl);
    this.timeoutMs = timeoutMs;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Map();
  }

  async open() {
    await withTimeout(new Promise((resolve, reject) => {
      this.socket.addEventListener("open", resolve, { once: true });
      this.socket.addEventListener("error", reject, { once: true });
    }), this.timeoutMs, "CDP WebSocket connection");
    this.socket.addEventListener("message", (event) => {
      let message;
      try {
        message = JSON.parse(String(event.data));
      } catch {
        return;
      }
      if (message.id) {
        const pending = this.pending.get(message.id);
        if (!pending) return;
        this.pending.delete(message.id);
        clearTimeout(pending.timer);
        if (message.error) {
          pending.reject(new Error(
            `${pending.method}: ${message.error.message}`));
        } else {
          pending.resolve(message.result);
        }
        return;
      }
      for (const listener of this.listeners.get(message.method) ?? []) {
        Promise.resolve(listener(message.params ?? {})).catch(() => {});
      }
    });
    this.socket.addEventListener("close", () => {
      for (const pending of this.pending.values()) {
        clearTimeout(pending.timer);
        pending.reject(new Error("CDP WebSocket closed"));
      }
      this.pending.clear();
    });
  }

  call(method, params = {}) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${method} timed out`));
      }, this.timeoutMs);
      this.pending.set(id, { resolve, reject, timer, method });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }

  on(method, listener) {
    const listeners = this.listeners.get(method) ?? [];
    listeners.push(listener);
    this.listeners.set(method, listeners);
  }

  waitFor(method) {
    return withTimeout(new Promise((resolve) => {
      const listener = (params) => {
        const listeners = this.listeners.get(method) ?? [];
        this.listeners.set(method, listeners.filter((item) => item !== listener));
        resolve(params);
      };
      this.on(method, listener);
    }), this.timeoutMs, method);
  }

  close() {
    if (this.socket.readyState === WebSocket.OPEN ||
        this.socket.readyState === WebSocket.CONNECTING) {
      this.socket.close();
    }
  }
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function pngDimensions(bytes) {
  if (bytes.length < 24 ||
      bytes.subarray(1, 4).toString("ascii") !== "PNG") {
    throw new Error("browser screenshot is not a PNG");
  }
  return {
    width: bytes.readUInt32BE(16),
    height: bytes.readUInt32BE(20),
  };
}

function serializeJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

async function writeJson(file, value) {
  await writeFile(file, serializeJson(value));
}

async function configurePage(cdp, width, height, dpr) {
  await Promise.all([
    cdp.call("Page.enable"),
    cdp.call("Runtime.enable"),
    cdp.call("Log.enable"),
    cdp.call("DOMSnapshot.enable"),
    cdp.call("Network.enable"),
  ]);
  await cdp.call("Emulation.setDeviceMetricsOverride", {
    width,
    height,
    deviceScaleFactor: dpr,
    mobile: false,
    screenWidth: width,
    screenHeight: height,
  });
  await cdp.call("Emulation.setLocaleOverride", { locale: "en-US" });
  await cdp.call("Emulation.setTimezoneOverride", { timezoneId: "UTC" });
  await cdp.call("Emulation.setEmulatedMedia", {
    media: "screen",
    features: [
      { name: "prefers-color-scheme", value: "light" },
      { name: "prefers-reduced-motion", value: "no-preference" },
    ],
  });
  await cdp.call("Page.addScriptToEvaluateOnNewDocument", {
    source: `(() => {
      globalThis.__pulpCaptureMutationEpoch = 0;
      globalThis.__pulpCaptureMutationObserver = new MutationObserver(() => {
        globalThis.__pulpCaptureMutationEpoch += 1;
      });
      globalThis.__pulpCaptureMutationObserver.observe(document, {
        attributes: true,
        characterData: true,
        childList: true,
        subtree: true
      });
    })()`,
  });
  try {
    await cdp.call("Browser.setDownloadBehavior", { behavior: "deny" });
  } catch {
    // Older compatible CDP builds may expose the command only on the browser
    // target. Headless download UI is still absent and navigation stays guarded.
  }
}

async function runProbe(options) {
  const browserPath = required(options.values, "--browser");
  const timeoutMs = positiveInteger(
    options.values, "--timeout-ms", 15000);
  const profileDir = await createEmptyProfile(
    options.values.get("--profile-dir"));
  let launched;
  let browserChild;
  let cdp;
  let cleanupPromise;
  const cleanup = () => {
    cleanupPromise ??= (async () => {
      cdp?.close();
      await terminateBrowser(browserChild);
      await rm(profileDir, {
        force: true,
        recursive: true,
        maxRetries: 3,
        retryDelay: 50,
      });
    })();
    return cleanupPromise;
  };
  activeCleanup = cleanup;
  const cancelDeadline = armCleanupDeadline({
    timeoutMs,
    cleanup,
    onExpired: exitAfterCleanupDeadline,
  });
  try {
    launched = await launchBrowser(
      browserPath, profileDir, timeoutMs,
      (child) => {
        browserChild = child;
      });
    const target = await pageTarget(launched.endpoint.port, timeoutMs);
    cdp = new Cdp(target.webSocketDebuggerUrl, timeoutMs);
    await cdp.open();
    await configurePage(cdp, 320, 240, 1);
    const version = await cdp.call("Browser.getVersion");
    const snapshot = await cdp.call("DOMSnapshot.captureSnapshot", {
      computedStyles: ["display"],
      includePaintOrder: true,
      includeDOMRects: true,
    });
    const screenshot = await cdp.call("Page.captureScreenshot", {
      format: "png",
      fromSurface: true,
      captureBeyondViewport: false,
    });
    if (!snapshot.documents?.length || !screenshot.data) {
      throw new Error("required DOMSnapshot/Page capture result was empty");
    }
    process.stdout.write(`${JSON.stringify({
      ok: true,
      product: version.product,
      protocolVersion: version.protocolVersion,
      revision: version.revision,
    })}\n`);
  } finally {
    cancelDeadline();
    await cleanup();
    if (activeCleanup === cleanup) activeCleanup = null;
  }
}

async function runCapture(options) {
  const browserPath = required(options.values, "--browser");
  const inputPath = required(options.values, "--input");
  const rootPath = required(options.values, "--root");
  const outputDir = path.resolve(required(options.values, "--output"));
  const initialWidth = positiveInteger(
    options.values, "--initial-width", 1280);
  const initialHeight = positiveInteger(
    options.values, "--initial-height", 800);
  const dpr = positiveInteger(options.values, "--dpr", 2);
  if (dpr !== 2) throw new Error("browser capture currently requires DPR 2");
  validateCaptureDimensions(initialWidth, initialHeight, dpr);
  const timeoutMs = positiveInteger(
    options.values, "--timeout-ms", 60000);
  const interactionPlanPath = options.values.get("--interactions");
  const interactionPlan = interactionPlanPath
    ? await readInteractionPlan(interactionPlanPath)
    : null;
  const allowNetwork = options.flags.has("--allow-network");
  const declaredNetworkOrigins =
    options.multiValues.get("--declared-network-origin") ?? [];
  const allowedExternalOrigins = allowNetwork
    ? expandAuditedProviderDependencies(
        normalizeDeclaredHttpsOrigins(declaredNetworkOrigins))
    : [];
  const resolvedExternalOrigins = allowNetwork
    ? await resolvePublicHttpsOrigins(allowedExternalOrigins)
    : new Map();
  const authorized = await authorizeInput(rootPath, inputPath);
  const sourceBytes = await readFile(authorized.input);
  const profileDir = await createEmptyProfile(
    options.values.get("--profile-dir"));
  await mkdir(outputDir, { recursive: true });

  let phase = "loopback-server";
  let server;
  let denyProxy;
  let launched;
  let browserChild;
  let cdp;
  let cleanupPromise;
  const cleanup = () => {
    cleanupPromise ??= (async () => {
      cdp?.close();
      await terminateBrowser(browserChild);
      await denyProxy?.close();
      await server?.close();
      await rm(profileDir, {
        force: true,
        recursive: true,
        maxRetries: 3,
        retryDelay: 50,
      });
    })();
    return cleanupPromise;
  };
  activeCleanup = cleanup;
  const cancelDeadline = armCleanupDeadline({
    timeoutMs,
    cleanup,
    onExpired: exitAfterCleanupDeadline,
  });
  try {
    server = await serveAuthorizedRoot(
      authorized.root, authorized.relativeEntry);
    if (!allowNetwork || resolvedExternalOrigins.size === 0) {
      denyProxy = await serveDenyProxy();
    }
    phase = "browser-launch";
    launched = await launchBrowser(
      browserPath, profileDir, timeoutMs,
      (child) => {
        browserChild = child;
      },
      denyProxy
        ? [
            "--disable-quic",
            "--force-webrtc-ip-handling-policy=disable_non_proxied_udp",
            "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1",
            `--proxy-server=${denyProxy.url}`,
            `--proxy-bypass-list=<-loopback>;${server.origin}`,
          ]
        : [
            "--disable-quic",
            "--force-webrtc-ip-handling-policy=disable_non_proxied_udp",
            `--host-resolver-rules=${
              hostResolverRules(resolvedExternalOrigins)}`,
          ]);
    const target = await pageTarget(launched.endpoint.port, timeoutMs);
    cdp = new Cdp(target.webSocketDebuggerUrl, timeoutMs);
    await cdp.open();

    phase = "page-configuration";
    await configurePage(cdp, initialWidth, initialHeight, dpr);
    await installDynamicWorkTracker(cdp);
    const healthMonitor = installCaptureHealthMonitor(cdp);
    const networkGuard = installNetworkGuard(
      cdp, server.origin, allowedExternalOrigins, server.privatePrefix);
    await networkGuard.enable();
    const pendingNetwork = new Set();
    cdp.on("Network.requestWillBeSent", ({ requestId }) => {
      pendingNetwork.add(requestId);
    });
    cdp.on("Network.loadingFinished", ({ requestId }) => {
      pendingNetwork.delete(requestId);
    });
    cdp.on("Network.loadingFailed", ({ requestId }) => {
      pendingNetwork.delete(requestId);
    });

    phase = "navigation";
    const loaded = cdp.waitFor("Page.loadEventFired");
    const navigation = await cdp.call("Page.navigate", {
      url: server.entryUrl,
    });
    if (navigation.errorText) {
      throw new Error(`navigation failed: ${navigation.errorText}`);
    }
    await loaded;

    phase = "page-settle";
    await disableMotion(cdp);
    const firstSettle = await waitForStable(cdp, {
      networkIdle: () => pendingNetwork.size === 0,
      // Runnable design frameworks can briefly plateau before their
      // dynamically-loaded render helpers commit. Observe a full readiness
      // window before running completion hooks or freezing the page.
      minimumElapsedMs: 1000,
    });
    const readiness = await awaitExplicitReadiness(cdp);
    let rendererHooks = await finalizeKnownRenderers(cdp);
    const rendererSettle = await waitForStable(cdp, {
      networkIdle: () => pendingNetwork.size === 0,
    });
    let interactionReport = null;
    let interactionReadiness = null;
    let interactionNavigationGuard = null;
    let interactionSettle = { rounds: 0, stableRounds: 0, elapsedMs: 0 };
    if (interactionPlan) {
      phase = "browser-interactions";
      interactionNavigationGuard =
        await createMainFrameNavigationGuard(cdp);
      const settleAfterInteraction = async () => {
        const settled = await waitForStable(cdp, {
          networkIdle: () => pendingNetwork.size === 0,
          minimumElapsedMs: 100,
        });
        interactionSettle.rounds += settled.rounds;
        interactionSettle.stableRounds += settled.stableRounds;
        interactionSettle.elapsedMs += settled.elapsedMs;
      };
      interactionReport = await executeInteractionPlan(
        cdp, interactionPlan, {
          navigationGuard: interactionNavigationGuard,
          settle: settleAfterInteraction,
        });
      interactionReadiness = await awaitExplicitReadiness(
        cdp, "__pulpInteractionReady");
      rendererHooks = mergeRendererHooks(
        rendererHooks, await finalizeKnownRenderers(cdp));
      await settleAfterInteraction();
      await interactionNavigationGuard.assertUnchanged();
      phase = "page-settle";
    }
    // Keep the viewport that authored the responsive layout. Resizing it to the
    // measured document extent creates a feedback loop for 100vh/min-height
    // application shells: the act of measuring changes the page being measured.
    // CDP can capture content beyond the viewport directly, so one settled
    // layout is both more deterministic and more faithful.
    let finalExtent = await measureDocumentExtent(cdp);
    let resolvedViewportWidth = initialWidth;
    let resolvedViewportHeight = initialHeight;
    let widthSettle = { rounds: 0, stableRounds: 0, elapsedMs: 0 };
    if (finalExtent.left < 0) {
      // Fixed-width canvases are commonly centered in a viewport smaller than
      // their authored surface. Their negative left edge cannot be recovered
      // by captureBeyondViewport. Grow width once by both clipped margins,
      // then settle the responsive layout again. Each axis gets at most one
      // bounded correction so responsive shells cannot enter a growth loop.
      resolvedViewportWidth =
        initialWidth + Math.ceil(-2 * finalExtent.left);
      if (resolvedViewportWidth > MAX_LOGICAL_CAPTURE_DIMENSION) {
        const error = new Error(
          `centered content requires a ${resolvedViewportWidth}px viewport`);
        error.code = "capture-viewport-too-wide";
        throw error;
      }
      validateCaptureDimensions(
        resolvedViewportWidth, resolvedViewportHeight, dpr);
      await cdp.call("Emulation.setDeviceMetricsOverride", {
        width: resolvedViewportWidth,
        height: resolvedViewportHeight,
        deviceScaleFactor: dpr,
        mobile: false,
        screenWidth: resolvedViewportWidth,
        screenHeight: resolvedViewportHeight,
      });
      widthSettle = await waitForStable(cdp, {
        networkIdle: () => pendingNetwork.size === 0,
      });
      finalExtent = await measureDocumentExtent(cdp);
      if (finalExtent.left < 0) {
        const error = new Error(
          `content still begins at x=${finalExtent.left}px after one bounded ` +
          "viewport correction; pass an explicit --width");
        error.code = "capture-negative-overflow";
        throw error;
      }
    }
    let heightSettle = { rounds: 0, stableRounds: 0, elapsedMs: 0 };
    if (finalExtent.top < 0) {
      resolvedViewportHeight =
        initialHeight + Math.ceil(-2 * finalExtent.top);
      if (resolvedViewportHeight > MAX_LOGICAL_CAPTURE_DIMENSION) {
        const error = new Error(
          `centered content requires a ${resolvedViewportHeight}px viewport height`);
        error.code = "capture-viewport-too-tall";
        throw error;
      }
      validateCaptureDimensions(
        resolvedViewportWidth, resolvedViewportHeight, dpr);
      await cdp.call("Emulation.setDeviceMetricsOverride", {
        width: resolvedViewportWidth,
        height: resolvedViewportHeight,
        deviceScaleFactor: dpr,
        mobile: false,
        screenWidth: resolvedViewportWidth,
        screenHeight: resolvedViewportHeight,
      });
      heightSettle = await waitForStable(cdp, {
        networkIdle: () => pendingNetwork.size === 0,
      });
      finalExtent = await measureDocumentExtent(cdp);
      if (finalExtent.top < 0) {
        const error = new Error(
          `content still begins at y=${finalExtent.top}px after one bounded ` +
          "viewport correction; pass an explicit --height");
        error.code = "capture-negative-overflow";
        throw error;
      }
    }
    if (finalExtent.left < 0 || finalExtent.top < 0) {
      const error = new Error(
        `content begins outside the corrected viewport at ` +
        `(${finalExtent.left}, ${finalExtent.top})`);
      error.code = "capture-negative-overflow";
      throw error;
    }
    await cdp.call("Runtime.evaluate", {
      expression: "scrollTo(0, 0); true",
      returnByValue: true,
    });

    phase = "same-frame-capture";
    await interactionNavigationGuard?.assertUnchanged();
    // Pause page virtual time before collecting any sidecar. Canvas/WebGL
    // requestAnimationFrame callbacks and timers must not advance while DOM,
    // semantics, tokens, health, and the authoritative pixels are read.
    // Remeasure after the final permitted presentation boundary. A scroll
    // listener or already-queued callback may have changed the document since
    // the settle-time measurement; every consumer below must describe the
    // frozen frame.
    finalExtent = await freezeAndMeasureDocumentExtent(cdp);
    if (finalExtent.left < 0 || finalExtent.top < 0) {
      const error = new Error(
        `frozen content begins outside the corrected viewport at ` +
        `(${finalExtent.left}, ${finalExtent.top})`);
      error.code = "capture-negative-overflow";
      throw error;
    }
    const captureWidth = finalExtent.width;
    const captureHeight = finalExtent.height;
    validateCaptureDimensions(
      captureWidth, captureHeight, dpr, "final capture extent");
    const screenshotOptions = {
      format: "png",
      captureBeyondViewport: true,
      fromSurface: true,
      clip: {
        x: 0,
        y: 0,
        width: captureWidth,
        height: captureHeight,
        scale: 1,
      },
    };
    const snapshot = await cdp.call("DOMSnapshot.captureSnapshot", {
      computedStyles: COMPUTED_STYLES,
      includePaintOrder: true,
      includeDOMRects: true,
      includeBlendedBackgroundColors: true,
      includeTextColorOpacities: true,
    });
    const semanticReport = await evaluateSemantics(
      cdp, snapshot, {
        width: captureWidth,
        height: captureHeight,
        device_scale_factor: dpr,
      });
    const tokenReport = await evaluateDesignTokens(cdp);
    const captureHealth = await verifyCaptureHealth(
      cdp, snapshot, healthMonitor, networkGuard.blocked);
    await networkGuard.awaitProvenance();
    // Compositor-backed pages can need several post-freeze presentation
    // boundaries even after DOM/timer motion is paused. Always observe the
    // complete bounded horizon, then require a byte-identical trailing run;
    // an early A,A plateau must not hide a later B,B presentation.
    const screenshotBytes =
      await captureStableScreenshot(cdp, screenshotOptions);
    if (!screenshotBytes) {
      const error = new Error(
        "the visual frame did not stabilize while capture evidence was collected");
      error.code = "capture-frame-not-deterministic";
      throw error;
    }
    await interactionNavigationGuard?.assertUnchanged();
    const pixels = pngDimensions(screenshotBytes);
    if (pixels.width !== captureWidth * dpr ||
        pixels.height !== captureHeight * dpr) {
      const error = new Error(
        `browser screenshot size ${pixels.width}x${pixels.height} does not ` +
        `match ${captureWidth}x${captureHeight} at DPR ${dpr}`);
      error.code = "capture-dpr-mismatch";
      throw error;
    }

    phase = "artifact-write";
    const sanitizedSnapshot = sanitizeSnapshot(
      snapshot, server.privatePrefix);
    const interactionReportBytes = interactionReport
      ? serializeJson(interactionReport)
      : "";
    const interactionReportSha256 = interactionReport
      ? sha256(Buffer.from(interactionReportBytes, "utf8"))
      : "";
    await Promise.all([
      writeFile(path.join(outputDir, "browser.png"), screenshotBytes),
      writeJson(path.join(outputDir, "dom-snapshot.json"), sanitizedSnapshot),
      writeJson(path.join(outputDir, "semantic-report.json"), semanticReport),
      writeJson(path.join(outputDir, "tokens.json"), tokenReport),
      ...(interactionReport
        ? [writeFile(
            path.join(outputDir, "interaction-report.json"),
            interactionReportBytes)]
        : []),
    ]);

    const browserVersion = await cdp.call("Browser.getVersion");
    const browserProductArg = options.values.get("--browser-product") ?? "";
    const browserVersionArg = options.values.get("--browser-version") ?? "";
    const actualProductParts = String(browserVersion.product ?? "").split("/");
    const envelope = {
      schema: "pulp-browser-capture-v1",
      version: 1,
      provenance: {
        capture_method: "chromium-cdp",
        browser: {
          product: browserProductArg || actualProductParts[0] || "Chromium",
          version: browserVersionArg || actualProductParts[1] || "",
          protocol_version: browserVersion.protocolVersion ?? "",
          build_hash: browserVersion.revision ?? "",
          origin: options.values.get("--browser-origin") ?? "system",
        },
        source: {
          entry: authorized.relativeEntry.split(path.sep).join("/"),
          sha256: sha256(sourceBytes),
        },
        viewport: {
          initial: { width: initialWidth, height: initialHeight },
          resolved: {
            width: resolvedViewportWidth,
            height: resolvedViewportHeight,
          },
          document: {
            width: captureWidth,
            height: captureHeight,
            primary_surface: finalExtent.primarySurface,
          },
          device_scale_factor: dpr,
        },
        locale: "en-US",
        timezone: "UTC",
        color_scheme: "light",
        reduced_motion: false,
        settle: {
          rounds:
            firstSettle.rounds + rendererSettle.rounds +
            interactionSettle.rounds +
            widthSettle.rounds + heightSettle.rounds,
          stable_rounds:
            firstSettle.stableRounds + rendererSettle.stableRounds +
            interactionSettle.stableRounds +
            widthSettle.stableRounds + heightSettle.stableRounds,
          elapsed_ms:
            firstSettle.elapsedMs + rendererSettle.elapsedMs +
            interactionSettle.elapsedMs +
            widthSettle.elapsedMs + heightSettle.elapsedMs,
        },
        network: {
          external_allowed: allowedExternalOrigins.length > 0,
          allowed_origins: allowedExternalOrigins,
          external_resources: networkGuard.external,
          blocked_requests: networkGuard.blocked,
        },
        health: captureHealth,
        readiness,
        ...(interactionReadiness
          ? { interaction_readiness: interactionReadiness }
          : {}),
        renderer_hooks: rendererHooks,
        ...(interactionReport
          ? {
              interactions: {
                schema: interactionReport.schema,
                version: interactionReport.version,
                report: "interaction-report.json",
                report_sha256: interactionReportSha256,
                plan_sha256: interactionReport.plan_sha256,
                action_count: interactionReport.action_count,
              },
            }
          : {}),
      },
      documents: [{
        id: "document:0",
        url: server.publicUrl,
        node_count: snapshot.documents?.[0]?.nodes?.nodeType?.length ?? 0,
        layout_count:
          snapshot.documents?.[0]?.layout?.nodeIndex?.length ?? 0,
        paint_order: true,
        snapshot_asset: "dom-snapshot.json",
      }],
      assets: [{
        id: "reference:browser",
        kind: "screenshot",
        mime_type: "image/png",
        path: "browser.png",
        sha256: sha256(screenshotBytes),
        width_px: pixels.width,
        height_px: pixels.height,
      }],
      semantics: {
        schema: semanticReport.schema,
        report: "semantic-report.json",
        candidate_count: semanticReport.summary.candidates,
        resolved_count: semanticReport.summary.resolved,
        unresolved_count: semanticReport.summary.unresolved,
      },
      tokens: {
        schema: tokenReport.schema,
        report: "tokens.json",
        color_count: Object.keys(tokenReport.colors).length,
        dimension_count: Object.keys(tokenReport.dimensions).length,
        string_count: Object.keys(tokenReport.strings).length,
      },
      states: [{
        name: "default",
        reference_asset_id: "reference:browser",
      }],
      reference: {
        asset_id: "reference:browser",
        path: "browser.png",
        logical_width: captureWidth,
        logical_height: captureHeight,
        device_scale_factor: dpr,
      },
    };
    await writeJson(
      path.join(outputDir, "capture.json"),
      sanitizeSnapshot(envelope, server.privatePrefix));
    process.stdout.write(`${JSON.stringify({
      ok: true,
      schema: envelope.schema,
      viewport: envelope.reference,
      semantic_candidates: semanticReport.summary.candidates,
    })}\n`);
  } catch (error) {
    error.phase = error.phase || phase;
    const sanitized =
      sanitizeCaptureError(error, server?.privatePrefix ?? "");
    error.message = sanitized.message;
    if (sanitized.health) error.health = sanitized.health;
    throw error;
  } finally {
    cancelDeadline();
    await cleanup();
    if (activeCleanup === cleanup) activeCleanup = null;
  }
}

let activeCleanup = null;
let terminating = false;
for (const signal of ["SIGINT", "SIGTERM", "SIGHUP"]) {
  process.on(signal, async () => {
    if (terminating) return;
    terminating = true;
    try {
      await activeCleanup?.();
    } finally {
      process.exit(signal === "SIGINT" ? 130 : 143);
    }
  });
}

async function main() {
  const nodeMajor = Number(process.versions.node.split(".")[0]);
  if (!Number.isInteger(nodeMajor) || nodeMajor < 22) {
    throw new Error(
      `Node.js 22 or newer is required (found ${process.versions.node})`);
  }
  const options = parseArguments(process.argv.slice(2));
  if (options.command === "probe") {
    await runProbe(options);
    return;
  }
  if (options.command === "capture") {
    await runCapture(options);
    return;
  }
  throw new Error(
    "usage: capture.mjs probe|capture --browser <path> [options]");
}

try {
  await main();
} catch (error) {
  const sanitized = sanitizeCaptureError(error, "");
  const argv = process.argv.slice(2);
  const outputIndex = argv.indexOf("--output");
  const outputDir = outputIndex >= 0 ? argv[outputIndex + 1] : "";
  const diagnostic = {
    schema: "pulp-browser-capture-error-v1",
    code: error.code ?? "browser-capture-failed",
    phase: error.phase ?? "capture-runtime",
    message: sanitized.message,
  };
  if (sanitized.health) diagnostic.health = sanitized.health;
  if (outputDir) {
    try {
      await mkdir(outputDir, { recursive: true });
      await writeJson(path.join(outputDir, "capture-error.json"), diagnostic);
    } catch {
      // stderr below remains the authoritative error channel.
    }
  }
  process.stderr.write(`${diagnostic.code}: ${diagnostic.message}\n`);
  process.exitCode = 1;
}
