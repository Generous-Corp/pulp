// SPDX-License-Identifier: MIT
import { createReadStream } from "node:fs";
import { realpath, stat } from "node:fs/promises";
import { createServer } from "node:http";
import { createHash, randomBytes } from "node:crypto";
import { lookup } from "node:dns/promises";
import { isIP } from "node:net";
import path from "node:path";

const MIME_TYPES = new Map([
  [".css", "text/css; charset=utf-8"],
  [".gif", "image/gif"],
  [".htm", "text/html; charset=utf-8"],
  [".html", "text/html; charset=utf-8"],
  [".ico", "image/x-icon"],
  [".jpeg", "image/jpeg"],
  [".jpg", "image/jpeg"],
  [".js", "text/javascript; charset=utf-8"],
  [".json", "application/json; charset=utf-8"],
  [".mjs", "text/javascript; charset=utf-8"],
  [".otf", "font/otf"],
  [".png", "image/png"],
  [".svg", "image/svg+xml"],
  [".ttf", "font/ttf"],
  [".wasm", "application/wasm"],
  [".webp", "image/webp"],
  [".woff", "font/woff"],
  [".woff2", "font/woff2"],
]);

export function isPathInside(root, candidate) {
  const relative = path.relative(root, candidate);
  return relative === "" ||
    (!relative.startsWith(`..${path.sep}`) && relative !== ".." &&
      !path.isAbsolute(relative));
}

export async function authorizeInput(rootPath, inputPath) {
  const root = await realpath(rootPath);
  const input = await realpath(inputPath);
  const [rootStat, inputStat] = await Promise.all([stat(root), stat(input)]);
  if (!rootStat.isDirectory()) throw new Error("staged root is not a directory");
  if (!inputStat.isFile()) throw new Error("capture input is not a regular file");
  if (!isPathInside(root, input)) {
    throw new Error("capture input escapes the authorized staged root");
  }
  return { root, input, relativeEntry: path.relative(root, input) };
}

function encodeRelativePath(relativePath) {
  return relativePath.split(path.sep).map(encodeURIComponent).join("/");
}

async function resolveServedFile(root, requestPath) {
  let decoded;
  try {
    decoded = decodeURIComponent(requestPath);
  } catch {
    return null;
  }
  if (decoded.includes("\0") || decoded.includes("\\")) return null;
  const unresolved = path.resolve(root, decoded.replace(/^\/+/, ""));
  if (!isPathInside(root, unresolved)) return null;

  let resolved;
  try {
    resolved = await realpath(unresolved);
  } catch {
    return null;
  }
  if (!isPathInside(root, resolved)) return null;
  const fileStat = await stat(resolved);
  return fileStat.isFile() ? { resolved, size: fileStat.size } : null;
}

function servedContentType(request, servedPath, authorizedEntry) {
  if (servedPath === authorizedEntry)
    return "text/html; charset=utf-8";
  const extension = path.extname(servedPath).toLowerCase();
  const known = MIME_TYPES.get(extension);
  if (known) return known;
  if (!extension) {
    const destination =
      String(request.headers["sec-fetch-dest"] || "").toLowerCase();
    if (destination === "script" || destination === "worker" ||
        destination === "serviceworker") {
      return "text/javascript; charset=utf-8";
    }
    if (destination === "style")
      return "text/css; charset=utf-8";
  }
  return "application/octet-stream";
}

function tolerateClientSocketErrors(server) {
  // Browser shutdown and parallel capture cleanup can reset a loopback
  // connection while a response is still being written. Raw CONNECT sockets
  // are handed directly to serveDenyProxy's listener, so Node's HTTP machinery
  // does not retain an error handler for them after the event is dispatched.
  server.on("connection", (socket) => {
    socket.on("error", () => {});
  });
  server.on("clientError", (_error, socket) => {
    socket.destroy();
  });
}

export async function serveAuthorizedRoot(root, relativeEntry) {
  const token = randomBytes(24).toString("hex");
  const prefix = `/${token}/`;
  const authorizedEntry =
    await realpath(path.resolve(root, relativeEntry));
  const server = createServer(async (request, response) => {
    try {
      if (request.method !== "GET" && request.method !== "HEAD") {
        response.writeHead(405, { Allow: "GET, HEAD" });
        response.end();
        return;
      }
      const requestUrl = new URL(request.url ?? "/", "http://127.0.0.1");
      // Chromium requests this automatically when a document does not declare
      // an icon. Keep that browser-generated request out of capture health
      // without treating arbitrary missing authored resources as successful.
      if (requestUrl.pathname === "/favicon.ico") {
        response.writeHead(204, { "Cache-Control": "no-store" });
        response.end();
        return;
      }
      if (!requestUrl.pathname.startsWith(prefix)) {
        response.writeHead(404);
        response.end();
        return;
      }
      const served = await resolveServedFile(
        root, requestUrl.pathname.slice(prefix.length));
      if (!served) {
        response.writeHead(404);
        response.end();
        return;
      }
      response.writeHead(200, {
        "Cache-Control": "no-store",
        "Content-Length": String(served.size),
        "Content-Type": servedContentType(
          request, served.resolved, authorizedEntry),
        "X-Content-Type-Options": "nosniff",
      });
      if (request.method === "HEAD") {
        response.end();
        return;
      }
      createReadStream(served.resolved).pipe(response);
    } catch (error) {
      response.writeHead(500);
      response.end(String(error));
    }
  });
  tolerateClientSocketErrors(server);

  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  if (!address || typeof address === "string") {
    server.close();
    throw new Error("loopback server did not expose a TCP address");
  }
  const origin = `http://127.0.0.1:${address.port}`;
  const entryUrl = `${origin}${prefix}${encodeRelativePath(relativeEntry)}`;
  return {
    origin,
    entryUrl,
    privatePrefix: `${origin}${prefix}`,
    publicUrl: `pulp-capture:///${encodeRelativePath(relativeEntry)}`,
    close: () =>
      new Promise((resolve) => server.close(() => resolve())),
  };
}

export async function serveDenyProxy() {
  const server = createServer((request, response) => {
    response.writeHead(403, {
      "Cache-Control": "no-store",
      "Connection": "close",
    });
    response.end("browser capture network access denied");
  });
  tolerateClientSocketErrors(server);
  server.on("connect", (_request, socket) => {
    socket.end(
      "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
  });
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  if (!address || typeof address === "string") {
    server.close();
    throw new Error("deny proxy did not expose a TCP address");
  }
  return {
    url: `http://127.0.0.1:${address.port}`,
    close: () =>
      new Promise((resolve) => server.close(() => resolve())),
  };
}

function isPublicIpv4(address) {
  const octets = address.split(".").map(Number);
  if (octets.length !== 4 || octets.some((value) =>
    !Number.isInteger(value) || value < 0 || value > 255)) {
    return false;
  }
  const [a, b, c] = octets;
  return a !== 0 && a !== 10 && a !== 127 && a < 224 &&
    !(a === 100 && b >= 64 && b <= 127) &&
    !(a === 169 && b === 254) &&
    !(a === 172 && b >= 16 && b <= 31) &&
    !(a === 192 && b === 0 && (c === 0 || c === 2)) &&
    !(a === 192 && b === 168) &&
    !(a === 192 && b === 88 && c === 99) &&
    !(a === 198 && (b === 18 || b === 19)) &&
    !(a === 198 && b === 51 && c === 100) &&
    !(a === 203 && b === 0 && c === 113);
}

function isPublicIpv6(address) {
  const normalized = address.toLowerCase().split("%", 1)[0];
  if (normalized.startsWith("::ffff:")) {
    return isPublicIpv4(normalized.slice("::ffff:".length));
  }
  return normalized !== "::" && normalized !== "::1" &&
    !normalized.startsWith("64:ff9b:") &&
    !normalized.startsWith("100:") &&
    !normalized.startsWith("2001:0:") &&
    !normalized.startsWith("2001:2:") &&
    !normalized.startsWith("2001:10:") &&
    !normalized.startsWith("2001:20:") &&
    !normalized.startsWith("fc") && !normalized.startsWith("fd") &&
    !normalized.startsWith("fe8") && !normalized.startsWith("fe9") &&
    !normalized.startsWith("fea") && !normalized.startsWith("feb") &&
    !normalized.startsWith("ff") &&
    !normalized.startsWith("2001:db8:") &&
    !normalized.startsWith("2002:");
}

export function isPublicAddress(address) {
  const family = isIP(address);
  if (family === 4) return isPublicIpv4(address);
  if (family === 6) return isPublicIpv6(address);
  return false;
}

function addressHostname(hostname) {
  return hostname.startsWith("[") && hostname.endsWith("]")
    ? hostname.slice(1, -1)
    : hostname;
}

export function normalizeDeclaredHttpsOrigins(candidates) {
  const origins = new Set();
  for (const candidate of candidates) {
    let url;
    try {
      url = new URL(candidate);
    } catch {
      continue;
    }
    if (url.protocol !== "https:" || url.username || url.password)
      continue;
    const hostname = addressHostname(url.hostname);
    if (isIP(hostname) && !isPublicAddress(hostname))
      continue;
    origins.add(url.origin);
  }
  return [...origins].sort();
}

export async function resolvePublicHttpsOrigins(origins) {
  const resolved = new Map();
  for (const origin of origins) {
    const url = new URL(origin);
    if (url.protocol !== "https:")
      throw new Error(`network allowlist rejected non-HTTPS origin ${origin}`);
    const hostname = addressHostname(url.hostname);
    const addresses = isIP(hostname)
      ? [{ address: hostname, family: isIP(hostname) }]
      : await lookup(hostname, { all: true, verbatim: true });
    if (!addresses.length ||
        addresses.some(({ address }) => !isPublicAddress(address))) {
      throw new Error(
        `network allowlist origin ${url.origin} resolved to a non-public address`);
    }
    // Pin one vetted address for this capture. Redirects remain subject to the
    // exact-origin Fetch guard below, and MAP * ~NOTFOUND prevents Chromium
    // from resolving an undeclared host behind the guard.
    const preferred =
      addresses.find(({ family }) => family === 4) ?? addresses[0];
    resolved.set(url.origin, {
      hostname,
      address: preferred.address,
    });
  }
  return resolved;
}

export function hostResolverRules(resolvedOrigins) {
  const rules = [];
  const hosts = new Map();
  for (const { hostname, address } of resolvedOrigins.values()) {
    const previous = hosts.get(hostname);
    if (previous && previous !== address) {
      throw new Error(`network allowlist resolved ${hostname} inconsistently`);
    }
    hosts.set(hostname, address);
  }
  for (const [hostname, address] of [...hosts].sort()) {
    rules.push(`MAP ${hostname} ${address}`);
  }
  rules.push("MAP * ~NOTFOUND", "EXCLUDE 127.0.0.1");
  return rules.join(", ");
}

function externalResourceUrl(value) {
  const url = new URL(value);
  return url.origin;
}

export function installNetworkGuard(
    cdp, allowedOrigin, allowedExternalOrigins = [], privatePrefix = "") {
  const blocked = [];
  const external = [];
  const allowed = new Set(allowedExternalOrigins);
  const pendingProvenance = new Set();
  const responses = new Map();
  let provenanceError = null;
  cdp.on("Fetch.requestPaused", async ({ requestId, request }) => {
    try {
      const url = new URL(request.url);
      const local = url.origin === allowedOrigin;
      const embedded = url.protocol === "data:" || url.protocol === "blob:" ||
        url.protocol === "about:";
      const externalAllowed =
        url.protocol === "https:" && allowed.has(url.origin);
      if (local || embedded || externalAllowed) {
        const localRootRelative =
          local && privatePrefix &&
          !request.url.startsWith(privatePrefix) &&
          url.pathname !== "/favicon.ico";
        await cdp.call("Fetch.continueRequest", {
          requestId,
          ...(localRootRelative
            ? {
                url: `${privatePrefix}${url.pathname.replace(/^\/+/, "")}` +
                  url.search,
              }
            : {}),
        });
      } else {
        blocked.push({ url: sanitizedUrl(request.url), reason: "blockedByClient" });
        await cdp.call("Fetch.failRequest", {
          requestId,
          errorReason: "BlockedByClient",
        });
      }
    } catch {
      blocked.push({ url: "(invalid URL)", reason: "blockedByClient" });
      await cdp.call("Fetch.failRequest", {
        requestId,
        errorReason: "BlockedByClient",
      });
    }
  });
  cdp.on("Network.webSocketCreated", ({ url }) => {
    blocked.push({
      url: sanitizedUrl(url),
      reason: "blockedByClient",
    });
  });
  cdp.on("Network.responseReceived", ({ requestId, response }) => {
    let url;
    try {
      url = new URL(response.url);
    } catch {
      return;
    }
    if (url.protocol !== "https:" || !allowed.has(url.origin))
      return;
    responses.set(requestId, {
      url: externalResourceUrl(response.url),
      origin: url.origin,
      status: response.status,
      mime_type: response.mimeType || "",
    });
  });
  cdp.on("Network.loadingFinished", ({ requestId }) => {
    const response = responses.get(requestId);
    responses.delete(requestId);
    if (!response) return;
    const pending = (async () => {
      try {
        const body = await cdp.call(
          "Network.getResponseBody", { requestId });
        const bytes = body.base64Encoded
          ? Buffer.from(body.body, "base64")
          : Buffer.from(body.body);
        external.push({
          ...response,
          sha256: createHash("sha256").update(bytes).digest("hex"),
          size_bytes: bytes.length,
        });
      } catch (cause) {
        provenanceError = new Error(
          `could not content-hash allowed external resource ${response.url}`,
          { cause });
        provenanceError.code = "capture-network-provenance-incomplete";
      }
    })();
    pendingProvenance.add(pending);
    pending.then(
      () => pendingProvenance.delete(pending),
      () => pendingProvenance.delete(pending));
  });
  return {
    blocked,
    external,
    awaitProvenance: async () => {
      await Promise.all([...pendingProvenance]);
      if (provenanceError) throw provenanceError;
      external.sort((left, right) =>
        left.url.localeCompare(right.url) ||
        left.sha256.localeCompare(right.sha256));
    },
    enable: async () => {
      await cdp.call("Network.setBlockedURLs", {
        urls: ["ws://*", "wss://*"],
      });
      await cdp.call("Fetch.enable", {
        patterns: [{ urlPattern: "*" }],
      });
    },
  };
}

export function sanitizeSnapshot(value, privatePrefix) {
  if (typeof value === "string") {
    let sanitized = privatePrefix
      ? value.split(privatePrefix).join("pulp-capture:///")
      : value;
    sanitized = sanitized.replace(
      /\bspawn\s+.+?\s+(ENOENT|EACCES|EPERM)\b/giu,
      "spawn <local-path> $1");
    sanitized = sanitized.replace(
      /\b(?:https?|wss?):\/\/[^\s"'<>]+/giu,
      (url) => sanitizedUrl(url));
    const captureUrls = [];
    sanitized = sanitized.replace(
      /pulp-capture:\/\/\/[^\s"'<>)]*/giu,
      (url) => {
        const marker = `PULP_CAPTURE_URL_${captureUrls.length}_`;
        captureUrls.push(url);
        return marker;
      });
    sanitized = sanitized.replace(
      /file:\/\/\/[^\s"'<>]+/giu, "file:///<local-path>");
    sanitized = sanitized.replace(
      /(["'])(\/[^"'\r\n]+)\1/gu, "$1<local-path>$1");
    // Requires at least one character after the slash: a lone "/" delimited by
    // whitespace names no file, but it IS how CSS separates a colour's alpha
    // (`oklab(L a b / .34)`) and a box-shadow layer's colour. Matching zero
    // trailing characters rewrote that separator to "<local-path>" and made
    // every translucent computed colour unparseable downstream.
    //
    // Requires a SECOND segment for the same reason. This runs over captured
    // text as well as diagnostics, and "/16" is panel copy on any arpeggiator
    // or delay, not a filename. A path worth hiding has a parent — "/Users/x",
    // "/workspace/y" — so the inner slash is what distinguishes a leaked path
    // from a musical note division.
    sanitized = sanitized.replace(
      /(?<![\w:/.-])\/(?!\/)[^\s"'<>;,)]*\/[^\s"'<>;,)]*/gu, "<local-path>");
    sanitized = sanitized.replace(
      /\b[A-Za-z]:\\[^\s"'<>]*/gu, "<local-path>");
    captureUrls.forEach((url, index) => {
      sanitized = sanitized.replace(`PULP_CAPTURE_URL_${index}_`, url);
    });
    return sanitized;
  }
  if (Array.isArray(value)) {
    return value.map((item) => sanitizeSnapshot(item, privatePrefix));
  }
  if (value && typeof value === "object") {
    const result = {};
    for (const [key, item] of Object.entries(value)) {
      result[key] = sanitizeSnapshot(item, privatePrefix);
    }
    return result;
  }
  return value;
}

export function sanitizeCaptureError(error, privatePrefix) {
  const sanitized = {
    message: sanitizeSnapshot(
      String(error?.message ?? error), privatePrefix),
  };
  if (error?.health) {
    sanitized.health = sanitizeSnapshot(error.health, privatePrefix);
  }
  return sanitized;
}

export function sanitizedUrl(value) {
  try {
    const url = new URL(value);
    if (url.hostname === "127.0.0.1" || url.hostname === "localhost") {
      return "pulp-capture:///";
    }
    return `${url.protocol}//${url.host}`;
  } catch {
    return "(invalid URL)";
  }
}

export function browserEnvironment(source = process.env) {
  const result = {};
  for (const key of [
    "LANG",
    "LC_ALL",
    "PATH",
    "SystemRoot",
    "TEMP",
    "TMP",
    "TMPDIR",
    "WINDIR",
  ]) {
    if (source[key]) result[key] = source[key];
  }
  return result;
}
