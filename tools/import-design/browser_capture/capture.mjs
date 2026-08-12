#!/usr/bin/env node
// SPDX-License-Identifier: MIT
import { createHash } from "node:crypto";
import { mkdirSync, writeFileSync, writeSync } from "node:fs";
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
  measureClippedControls,
  measureDocumentExtent,
  resumeDynamicTime,
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
import {
  armCleanupDeadline,
  createCaptureProgress,
} from "./lifecycle.mjs";
import { evaluateDesignTokens } from "./tokens.mjs";
import { evaluatePlatformFonts } from "./platform_fonts.mjs";

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

function exitAfterCleanupDeadline(expiry, outputDir, browser) {
  const diagnostic = {
    schema: "pulp-browser-capture-error-v1",
    code: "browser-capture-timeout",
    phase: expiry.phase,
    message: `browser capture timed out; ${expiry.summary}`,
    ...(browser ? { browser } : {}),
  };
  if (outputDir) {
    try {
      mkdirSync(outputDir, { recursive: true });
      writeFileSync(
        path.join(outputDir, "capture-error.json"), serializeJson(diagnostic));
    } catch {
      // stderr below remains the authoritative error channel.
    }
  }
  try {
    writeSync(
      process.stderr.fd, `${diagnostic.code}: ${diagnostic.message}\n`);
  } catch {
    // The parent still sees exit 124 if its stderr pipe has already closed.
  }
  process.exit(124);
}

// The resolved browser identity belongs in every capture record, including the
// records of captures that never produce an envelope. Browser behaviour around
// screenshots and virtual time changes between Chromium releases, so a failure
// report that does not name the browser cannot be triaged.
function reportBrowser(version) {
  const product = String(version?.product ?? "");
  const [name, release] = product.split("/");
  const browser = {
    product: name || "Chromium",
    version: release || "",
    protocol_version: version?.protocolVersion ?? "",
    build_hash: version?.revision ?? "",
  };
  try {
    writeSync(
      process.stderr.fd,
      `[browser-capture] browser=${browser.product}/${browser.version} ` +
      `protocol=${browser.protocol_version} ` +
      `build=${browser.build_hash}\n`);
  } catch {
    // A closed stderr pipe must not fail an otherwise healthy capture.
  }
  return browser;
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
  constructor(webSocketUrl, timeoutMs, progress = createCaptureProgress()) {
    if (typeof WebSocket !== "function") {
      throw new Error("Node.js 22 or newer is required (WebSocket unavailable)");
    }
    this.socket = new WebSocket(webSocketUrl);
    this.timeoutMs = timeoutMs;
    this.progress = progress;
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
        // A protocol error is still a completed round trip: the browser
        // answered. Only an abandoned call leaves the step uncompleted.
        pending.settle();
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
        pending.settle(false);
        pending.reject(new Error("CDP WebSocket closed"));
      }
      this.pending.clear();
    });
  }

  call(method, params = {}, sessionId = undefined) {
    const id = this.nextId++;
    const settle = this.progress.begin(method);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        settle(false);
        reject(new Error(`${method} timed out`));
      }, this.timeoutMs);
      this.pending.set(id, { resolve, reject, settle, timer, method });
      this.socket.send(JSON.stringify({
        id,
        method,
        params,
        ...(sessionId ? { sessionId } : {}),
      }));
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

const MAX_CAPTURED_CANVASES = 32;
const MAX_CAPTURED_CANVAS_PIXELS = 64 * 1024 * 1024;

async function captureCanvasAssets(cdp, snapshot) {
  const document = snapshot.documents?.[0];
  const nodes = document?.nodes;
  const strings = snapshot.strings ?? [];
  if (!nodes) return [];

  const canvases = [];
  const seenBackendNodeIds = new Set();
  for (let index = 0; index < (nodes.nodeName?.length ?? 0); index++) {
    const tag = String(strings[nodes.nodeName[index]] ?? "").toLowerCase();
    if (tag !== "canvas") continue;
    const backendNodeId = nodes.backendNodeId?.[index];
    if (!Number.isInteger(backendNodeId) || backendNodeId <= 0) continue;
    if (seenBackendNodeIds.has(backendNodeId)) {
      throw new Error(
        `browser capture found duplicate canvas backend node ${backendNodeId}`);
    }
    seenBackendNodeIds.add(backendNodeId);
    canvases.push(backendNodeId);
  }
  if (canvases.length > MAX_CAPTURED_CANVASES) {
    throw new Error(
      `browser capture found ${canvases.length} canvases; maximum is ` +
      `${MAX_CAPTURED_CANVASES}`);
  }

  const assets = [];
  let totalPixels = 0;
  for (const backendNodeId of canvases) {
    const resolved = await cdp.call("DOM.resolveNode", { backendNodeId });
    const objectId = resolved.object?.objectId;
    if (!objectId) {
      throw new Error(`could not resolve canvas backend node ${backendNodeId}`);
    }
    try {
      // Inspect the backing-store dimensions before asking Chromium to encode
      // anything.  A malicious or accidental enormous canvas must fail before
      // toDataURL can allocate its unbounded intermediate PNG/base64 buffers.
      const measured = await cdp.call("Runtime.callFunctionOn", {
        objectId,
        functionDeclaration: `function() {
          if (!(this instanceof HTMLCanvasElement))
            throw new Error('resolved node is not a canvas');
          return { width: this.width, height: this.height };
        }`,
        returnByValue: true,
      });
      const width = Number(measured.result?.value?.width);
      const height = Number(measured.result?.value?.height);
      const pixels = width * height;
      if (!Number.isInteger(width) || !Number.isInteger(height) ||
          width <= 0 || height <= 0 || !Number.isSafeInteger(pixels)) {
        throw new Error(`canvas backend node ${backendNodeId} has invalid size`);
      }
      totalPixels += pixels;
      if (!Number.isSafeInteger(totalPixels) ||
          totalPixels > MAX_CAPTURED_CANVAS_PIXELS) {
        throw new Error(
          `captured canvas pixels exceed ${MAX_CAPTURED_CANVAS_PIXELS}`);
      }

      const captured = await cdp.call("Runtime.callFunctionOn", {
        objectId,
        functionDeclaration: `function() { return this.toDataURL('image/png'); }`,
        returnByValue: true,
      });
      const dataUrl = captured.result?.value;
      const prefix = "data:image/png;base64,";
      if (typeof dataUrl !== "string" || !dataUrl.startsWith(prefix)) {
        throw new Error(
          `canvas backend node ${backendNodeId} could not be captured as PNG`);
      }
      const encoded = dataUrl.slice(prefix.length);
      if (encoded.length === 0 || encoded.length % 4 !== 0 ||
          !/^[A-Za-z0-9+/]*={0,2}$/.test(encoded)) {
        throw new Error(
          `canvas backend node ${backendNodeId} returned invalid PNG base64`);
      }
      const bytes = Buffer.from(encoded, "base64");
      const dimensions = pngDimensions(bytes);
      if (dimensions.width !== width || dimensions.height !== height) {
        throw new Error(
          `canvas backend node ${backendNodeId} PNG size does not match backing store`);
      }
      assets.push({
        id: `canvas:${backendNodeId}`,
        kind: "canvas-snapshot",
        mime_type: "image/png",
        path: `canvas-${backendNodeId}.png`,
        sha256: sha256(bytes),
        width_px: width,
        height_px: height,
        backend_node_id: backendNodeId,
        bytes,
      });
    } finally {
      await cdp.call("Runtime.releaseObject", { objectId }).catch(() => {});
    }
  }
  return assets;
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
  await cdp.call("Page.addScriptToEvaluateOnNewDocument", {
    source: `(() => {
      // Capture the executable document at the browser loader's own handoff:
      // after product adapters have patched the source, but before React has
      // mounted into it.  A post-render outerHTML snapshot loses closures and
      // canvas programs; the DOMParser input retains the exact scripts that
      // produced the accepted Chromium frame.
      const parser = DOMParser.prototype.parseFromString;
      const createObjectURL = URL.createObjectURL.bind(URL);
      const blobs = new Map();
      globalThis.__pulpMaterializedDocument = null;
      globalThis.__pulpMaterializedBlobs = blobs;
      URL.createObjectURL = function(blob) {
        const url = createObjectURL(blob);
        if (blob instanceof Blob) blobs.set(url, blob);
        return url;
      };
      DOMParser.prototype.parseFromString = function(source, type) {
        if (String(type).toLowerCase() === 'text/html') {
          globalThis.__pulpMaterializedDocument = {
            html: String(source),
            mime_type: 'text/html'
          };
        }
        return parser.call(this, source, type);
      };
    })()`,
  });
  try {
    await cdp.call("Browser.setDownloadBehavior", { behavior: "deny" });
  } catch {
    // Older compatible CDP builds may expose the command only on the browser
    // target. Headless download UI is still absent and navigation stays guarded.
  }
}

async function captureMaterializedDocument(cdp) {
  const metadata = await cdp.call("Runtime.evaluate", {
    expression: `(async () => {
      const document = globalThis.__pulpMaterializedDocument;
      if (!document || typeof document.html !== 'string') return null;
      const urls = [...document.html.matchAll(/blob:[^\\s"'<>\\)]+/g)]
        .map((match) => match[0]);
      const unique = [...new Set(urls)];
      if (unique.length > 256) {
        throw new Error('materialized document references too many blob assets');
      }
      const assets = [];
      let total = 0;
      for (const url of unique) {
        const blob = globalThis.__pulpMaterializedBlobs?.get(url);
        if (!(blob instanceof Blob)) {
          throw new Error('materialized blob was not captured: ' + url);
        }
        const bytes = new Uint8Array(await blob.arrayBuffer());
        total += bytes.byteLength;
        if (total > 64 * 1024 * 1024) {
          throw new Error('materialized blob assets exceed 64 MiB');
        }
        assets.push({
          url,
          mime_type: blob.type || 'application/octet-stream',
          byte_length: bytes.byteLength
        });
      }
      return {
        schema: 'pulp-materialized-browser-document-v1',
        version: 1,
        html: document.html,
        mime_type: document.mime_type,
        assets
      };
    })()`,
    awaitPromise: true,
    returnByValue: true,
  });
  const materialized = metadata.result?.value ?? null;
  if (!materialized) return null;

  // Production bundles include Babel/React/font payloads. Returning them all
  // in one Runtime.evaluate result can exceed Chrome's DevTools WebSocket
  // message budget and close the connection without a protocol error. Read
  // each bounded blob independently and assemble the durable sidecar here.
  for (let index = 0; index < materialized.assets.length; index += 1) {
    const payload = await cdp.call("Runtime.evaluate", {
      expression: `(async () => {
        const item = [...globalThis.__pulpMaterializedDocument.html
          .matchAll(/blob:[^\\s"'<>\\)]+/g)].map((m) => m[0]);
        const url = [...new Set(item)][${index}];
        const blob = globalThis.__pulpMaterializedBlobs.get(url);
        const bytes = new Uint8Array(await blob.arrayBuffer());
        let binary = '';
        for (let offset = 0; offset < bytes.length; offset += 0x8000) {
          binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
        }
        return btoa(binary);
      })()`,
      awaitPromise: true,
      returnByValue: true,
    });
    const dataBase64 = payload.result?.value;
    if (typeof dataBase64 !== "string") {
      throw new Error(`materialized blob ${index} did not return bytes`);
    }
    materialized.assets[index].data_base64 = dataBase64;
    const bytes = Buffer.from(dataBase64, "base64");
    if (bytes.length !== materialized.assets[index].byte_length) {
      throw new Error(`materialized blob ${index} length changed during capture`);
    }
    materialized.assets[index].sha256 = sha256(bytes);
  }

  // Blob URLs are realm-scoped and typically contain a fresh UUID on every
  // launch.  They must not cross the browser/native handoff boundary.  Rewrite
  // the executable document to content-addressed asset IDs so identical source
  // material produces byte-identical sidecars across captures.
  const stableAssets = new Map();
  for (const asset of materialized.assets) {
    const id = `pulp-materialized-asset-${asset.sha256}`;
    materialized.html = materialized.html.split(asset.url).join(id);
    if (!stableAssets.has(id)) {
      stableAssets.set(id, {
        id,
        mime_type: asset.mime_type,
        byte_length: asset.byte_length,
        data_base64: asset.data_base64,
        sha256: asset.sha256,
      });
    }
  }
  materialized.assets = [...stableAssets.values()];
  if (materialized.html.includes("blob:")) {
    throw new Error("materialized document retained an unresolved blob URL");
  }
  return materialized;
}

async function runProbe(options) {
  const browserPath = required(options.values, "--browser");
  const timeoutMs = positiveInteger(
    options.values, "--timeout-ms", 15000);
  const profileDir = await createEmptyProfile(
    options.values.get("--profile-dir"));
  const progress = createCaptureProgress("browser-launch");
  let launched;
  let browserChild;
  let cdp;
  let browser;
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
  const expiry = { phase: progress.phase, summary: "" };
  const cancelDeadline = armCleanupDeadline({
    timeoutMs,
    cleanup,
    onExpiring: () => {
      expiry.phase = progress.phase;
      expiry.summary = progress.describe();
    },
    onExpired: () => exitAfterCleanupDeadline(expiry, "", browser),
  });
  try {
    launched = await launchBrowser(
      browserPath, profileDir, timeoutMs,
      (child) => {
        browserChild = child;
      });
    const target = await pageTarget(launched.endpoint.port, timeoutMs);
    cdp = new Cdp(target.webSocketDebuggerUrl, timeoutMs, progress);
    await cdp.open();
    progress.enterPhase("page-configuration");
    const version = await cdp.call("Browser.getVersion");
    browser = reportBrowser(version);
    await configurePage(cdp, 320, 240, 1);
    progress.enterPhase("probe-capture");
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
  } catch (error) {
    error.phase = error.phase || progress.phase;
    throw error;
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

  const progress = createCaptureProgress("loopback-server");
  let server;
  let denyProxy;
  let launched;
  let browserChild;
  let cdp;
  let browser;
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
  const expiry = { phase: progress.phase, summary: "" };
  const cancelDeadline = armCleanupDeadline({
    timeoutMs,
    cleanup,
    onExpiring: () => {
      expiry.phase = progress.phase;
      expiry.summary = progress.describe();
    },
    onExpired: () => exitAfterCleanupDeadline(expiry, outputDir, browser),
  });
  try {
    server = await serveAuthorizedRoot(
      authorized.root, authorized.relativeEntry);
    if (!allowNetwork || resolvedExternalOrigins.size === 0) {
      denyProxy = await serveDenyProxy();
    }
    progress.enterPhase("browser-launch");
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
    cdp = new Cdp(target.webSocketDebuggerUrl, timeoutMs, progress);
    await cdp.open();

    progress.enterPhase("page-configuration");
    browser = reportBrowser(await cdp.call("Browser.getVersion"));
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

    progress.enterPhase("navigation");
    const loaded = cdp.waitFor("Page.loadEventFired");
    const navigation = await cdp.call("Page.navigate", {
      url: server.entryUrl,
    });
    if (navigation.errorText) {
      throw new Error(`navigation failed: ${navigation.errorText}`);
    }
    await loaded;

    progress.enterPhase("page-settle");
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
      progress.enterPhase("browser-interactions");
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
      progress.enterPhase("page-settle");
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
        // The correction above re-centres CENTRED content by growing the
        // viewport, which is why it is expressed as -2 * top. Content placed
        // ABSOLUTELY above the origin does not move when the viewport grows, so
        // it survives that correction unchanged — and `--height` is the same
        // lever, so refusing with "pass an explicit --height" sent the caller
        // round a loop that could not terminate.
        //
        // Agents write `top: -5px` constantly (a badge nudged over an edge, a
        // ring inset), and refusing the whole design over a few pixels loses
        // the entire panel. Translate instead: push the document down by the
        // overhang so nothing sits above y=0. Relative geometry is untouched —
        // every box moves by the same amount, so the DOM snapshot and the
        // bitmap stay in the same coordinate space as each other.
        const shift = Math.ceil(-finalExtent.top);
        await cdp.call("Runtime.evaluate", {
          expression:
            `(() => { const s = document.createElement('style');` +
            ` s.setAttribute('data-pulp-origin-shift', '${shift}');` +
            ` s.textContent = 'body{margin-top:${shift}px !important}';` +
            ` document.head.appendChild(s); return true; })()`,
          returnByValue: true,
        });
        heightSettle = await waitForStable(cdp, {
          networkIdle: () => pendingNetwork.size === 0,
        });
        finalExtent = await measureDocumentExtent(cdp);
        if (finalExtent.top < 0) {
          const error = new Error(
            `content still begins at y=${finalExtent.top}px after a ` +
            `${shift}px origin shift. Something re-anchors to the viewport ` +
            "(position: fixed, or a negative margin on <html>), which a " +
            "document-level shift cannot move.");
          error.code = "capture-negative-overflow";
          throw error;
        }
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

    progress.enterPhase("same-frame-capture");
    await interactionNavigationGuard?.assertUnchanged();
    const materializedDocument = await captureMaterializedDocument(cdp);
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
    // Symmetric with the rule above. Content past the top or left is refused;
    // content past a clipping edge was silently dropped, so a panel could ship
    // with parameters bound to controls nobody can see.
    const clippedControls = await measureClippedControls(cdp);
    if (clippedControls.length > 0) {
      const detail = clippedControls
        .map(c => `${c.binding || "(unbound)"} cut by ${c.lost}px inside .${c.by}`)
        .join("; ");
      const error = new Error(
        `${clippedControls.length} bound control(s) are clipped out of view: ` +
        `${detail}. The panel declares a frame smaller than its own content; ` +
        "give the root the height its content needs, or remove what does not fit.");
      error.code = "capture-control-clipped";
      throw error;
    }
    const captureWidth = finalExtent.width;
    const captureHeight = finalExtent.height;

    // Where the authored frame sits INSIDE the captured image. Measured HERE,
    // while the page is still live — the envelope is assembled after teardown,
    // and evaluating there returns null.
    //
    // The capture is deliberately larger than the design: the root carries its
    // own padding, and the width growth above extends past a centered canvas so
    // captureBeyondViewport does not clip it. Both are correct; shrinking the
    // image would clip the drop shadows the growth exists to preserve. What was
    // missing is the OFFSET. Without it a consumer must guess where the design
    // starts, and a centered guess is wrong because the growth is asymmetric —
    // it scored a visually-close panel as 73% different, and two phantom
    // "renderer bugs" (a 9x-too-tall caret, advances 1/8 px short) were both
    // this offset misread as pixel error.
    //
    // Best effort: a capture that cannot resolve the frame still succeeds with
    // a null, because a missing offset must degrade to "cannot align" rather
    // than fail an otherwise good capture.
    let authoredFrame = null;
    try {
      const frameEval = await cdp.call("Runtime.evaluate", {
        expression: `(() => {
          if (!document.body) return null;
          // NOT firstElementChild: the harness injects a <style> into the body
          // for the scroll-shift correction, and a style tag measures 0x0.
          // Take the first child that actually occupies space.
          let el = null;
          for (const c of document.body.children) {
            const b = c.getBoundingClientRect();
            if (b.width > 0 && b.height > 0) { el = c; break; }
          }
          if (!el) return null;
          const r = el.getBoundingClientRect();
          return { x: r.left + window.scrollX, y: r.top + window.scrollY,
                   width: r.width, height: r.height };
        })()`,
        returnByValue: true,
      });
      const box = frameEval.result?.value;
      if (box && box.width > 0 && box.height > 0) {
        // Relative to the capture origin — the document extent's top-left,
        // which is negative for content starting left of the viewport.
        authoredFrame = {
          x: box.x - finalExtent.left,
          y: box.y - finalExtent.top,
          width: box.width,
          height: box.height,
        };
      }
    } catch {
      authoredFrame = null;
    }
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
    // Canvas backing stores are imperative paint, not computed style. Snapshot
    // each one while the same browser frame is frozen, then bind it back to the
    // DOMSnapshot through backendNodeId. This preserves transparent overlap and
    // paint order without flattening the whole editor into one photograph.
    const canvasAssets = await captureCanvasAssets(cdp, snapshot);
    if (materializedDocument) {
      materializedDocument.canvas_bindings = canvasAssets.map(
        (asset, index) => ({
          index,
          anchor: `chromium:backend-node:${asset.backend_node_id}`,
        }));
    }
    const semanticReport = await evaluateSemantics(
      cdp, snapshot, {
        width: captureWidth,
        height: captureHeight,
        device_scale_factor: dpr,
      });
    if (materializedDocument) {
      // Semantics and painted geometry were resolved from the same frozen
      // DOMSnapshot. Keep a bounded, renderer-neutral binding manifest beside
      // the executable document so the live native behavior tree can be joined
      // to DesignIR by Chromium identity rather than by text/bounds heuristics.
      materializedDocument.semantic_bindings = semanticReport.candidates
        .filter((candidate) =>
          candidate.resolved === true &&
          Number.isSafeInteger(candidate.backend_node_id) &&
          candidate.backend_node_id > 0)
        .map((candidate, index) => ({
          index,
          backend_node_id: candidate.backend_node_id,
          anchor: `chromium:backend-node:${candidate.backend_node_id}`,
          kind: String(candidate.kind ?? "unknown"),
          tag: String(candidate.tag ?? ""),
          name: String(candidate.name ?? ""),
          bounds: {
            left: Number(candidate.bounds?.left ?? 0),
            top: Number(candidate.bounds?.top ?? 0),
            width: Number(candidate.bounds?.width ?? 0),
            height: Number(candidate.bounds?.height ?? 0),
          },
        }));
    }
    const tokenReport = await evaluateDesignTokens(cdp);
    // Read before time resumes, like every other DOM-derived sidecar: the
    // faces reported have to be the ones the frozen frame was shaped with.
    const platformFontReport = await evaluatePlatformFonts(
      cdp, snapshot, COMPUTED_STYLES);
    const captureHealth = await verifyCaptureHealth(
      cdp, snapshot, healthMonitor, networkGuard.blocked);
    await networkGuard.awaitProvenance();
    // Every sidecar above is read from the DOM while virtual time is paused.
    // Pixels are different: the compositor cannot present a new frame with
    // virtual time paused, so the screenshot loop below runs with virtual time
    // released again.
    await resumeDynamicTime(cdp);
    // Compositor-backed pages can need several post-freeze presentation
    // boundaries even after DOM/timer motion is frozen. Always observe the
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
    // Capture the exact authored body beneath declared moving indicators.
    // Pixel inpainting cannot reconstruct arbitrary dither, tick rings, or
    // gradients once a pointer/thumb has covered them; the defect is merely
    // hidden at the declared value and becomes visible as soon as the control
    // moves. `visibility` removes only the marked paint without changing its
    // layout, so this second frame is a faithful source for static control
    // bodies while browser.png remains the untouched comparison oracle.
    const hiddenIndicators = await cdp.call("Runtime.evaluate", {
      expression: `(() => {
        window.__pulpCaptureIndicatorStyles =
          Array.from(document.querySelectorAll('[data-pulp-indicator]')).map(el => ({
            el,
            value: el.style.getPropertyValue('visibility'),
            priority: el.style.getPropertyPriority('visibility')
          }));
        for (const entry of window.__pulpCaptureIndicatorStyles)
          entry.el.style.setProperty('visibility', 'hidden', 'important');
        return window.__pulpCaptureIndicatorStyles.length;
      })()`,
      returnByValue: true,
    });
    const hiddenIndicatorCount = hiddenIndicators.result?.value ?? 0;
    let staticScreenshotBytes = screenshotBytes;
    if (hiddenIndicatorCount > 0) {
      staticScreenshotBytes =
        await captureStableScreenshot(cdp, screenshotOptions);
      await cdp.call("Runtime.evaluate", {
        expression: `(() => {
          for (const entry of (window.__pulpCaptureIndicatorStyles || [])) {
            if (entry.value)
              entry.el.style.setProperty('visibility', entry.value, entry.priority);
            else
              entry.el.style.removeProperty('visibility');
          }
          delete window.__pulpCaptureIndicatorStyles;
          return true;
        })()`,
        returnByValue: true,
      });
    }
    if (!staticScreenshotBytes) {
      const error = new Error(
        "the indicator-free visual frame did not stabilize");
      error.code = "capture-static-frame-not-deterministic";
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
    progress.enterPhase("artifact-write");
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
      writeFile(
        path.join(outputDir, "browser-static.png"), staticScreenshotBytes),
      ...canvasAssets.map((asset) =>
        writeFile(path.join(outputDir, asset.path), asset.bytes)),
      // `layout.styles` rows are positional: entry N is the Nth property of
      // the request. Recording the request order alongside the data keeps the
      // snapshot self-describing, so a consumer never has to hardcode a
      // parallel copy of COMPUTED_STYLES that would silently map a background
      // into a border the first time this list changes.
      writeJson(path.join(outputDir, "dom-snapshot.json"), {
        ...sanitizedSnapshot,
        computedStyleNames: COMPUTED_STYLES,
      }),
      writeJson(path.join(outputDir, "semantic-report.json"), semanticReport),
      writeJson(path.join(outputDir, "tokens.json"), tokenReport),
      writeJson(
        path.join(outputDir, "platform-fonts.json"), platformFontReport),
      ...(materializedDocument
        ? [writeJson(
            path.join(outputDir, "materialized-document.json"),
            materializedDocument)]
        : []),
      ...(interactionReport
        ? [writeFile(
            path.join(outputDir, "interaction-report.json"),
            interactionReportBytes)]
        : []),
    ]);

    const browserProductArg = options.values.get("--browser-product") ?? "";
    const browserVersionArg = options.values.get("--browser-version") ?? "";
    const envelope = {
      schema: "pulp-browser-capture-v1",
      version: 1,
      provenance: {
        capture_method: "chromium-cdp",
        browser: {
          product: browserProductArg || browser.product,
          version: browserVersionArg || browser.version,
          protocol_version: browser.protocol_version,
          build_hash: browser.build_hash,
          origin: options.values.get("--browser-origin") ?? "system",
        },
        source: {
          entry: authorized.relativeEntry.split(path.sep).join("/"),
          sha256: sha256(sourceBytes),
          ...(materializedDocument
            ? {
                materialized_document: "materialized-document.json",
                materialized_document_sha256: sha256(Buffer.from(
                  serializeJson(materializedDocument), "utf8")),
                materialized_asset_count: materializedDocument.assets.length,
              }
            : {}),
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
      }, {
        id: "reference:browser-static",
        kind: "screenshot",
        mime_type: "image/png",
        path: "browser-static.png",
        sha256: sha256(staticScreenshotBytes),
        width_px: pixels.width,
        height_px: pixels.height,
      }, ...canvasAssets.map(({ bytes: _bytes, ...asset }) => asset)],
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
      // Which typefaces the reference pixels were actually shaped with, as
      // opposed to which the style rows asked for. A consumer comparing its own
      // text against this capture has to know whether the reference used the
      // authored family or a substitute, because measuring against a substitute
      // and "fixing" the difference makes the renderer wrong on purpose.
      platform_fonts: {
        schema: platformFontReport.schema,
        report: "platform-fonts.json",
        text_run_count: platformFontReport.summary.text_runs,
        resolved_run_count: platformFontReport.summary.resolved,
        face_count: platformFontReport.faces_by_glyph_count.length,
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
        // Null when the frame could not be resolved, and null is meaningful:
        // it says "this capture cannot be registered", which a consumer must
        // treat as "refuse to score" rather than as zero offset.
        authored_frame: authoredFrame,
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
    error.phase = error.phase || progress.phase;
    error.browser = browser;
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
  if (error.browser) diagnostic.browser = error.browser;
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
