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
      // This server handles authored input. Never reflect an exception: it can
      // contain an absolute path, a loopback capability token, or a stack.
      response.end("browser capture resource unavailable");
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

// The glyph Chromium paints for each character of a masked run, by
// `-webkit-text-security` keyword. A password field's UA style is `disc`.
const MASK_GLYPHS = new Map([
  ["disc", "•"],
  ["circle", "◦"],
  ["square", "■"],
]);
const DEFAULT_MASK_GLYPH = "•";

// Columns of a DOMSnapshot whose entries are string-table indices.
const DOCUMENT_STRING_FIELDS = [
  "documentURL", "title", "baseURL", "contentLanguage", "encodingName",
  "publicId", "systemId", "frameId"];
const NODE_STRING_COLUMNS = ["nodeName", "nodeValue"];
const NODE_RARE_STRING_COLUMNS = [
  "shadowRootType", "textValue", "inputValue", "pseudoIdentifier",
  "currentSourceURL", "originURL"];
const LAYOUT_STRING_COLUMNS = ["text", "blendedBackgroundColors"];

/// Every string-table reference in a snapshot, as `[holder, key]` pairs whose
/// `holder[key]` is the index.
function stringReferences(snapshot) {
  const refs = [];
  const column = (array) => {
    if (!Array.isArray(array)) return;
    for (let i = 0; i < array.length; i += 1) {
      if (Number.isInteger(array[i])) refs.push([array, i]);
    }
  };
  for (const document of snapshot.documents ?? []) {
    for (const field of DOCUMENT_STRING_FIELDS) {
      if (Number.isInteger(document[field])) refs.push([document, field]);
    }
    const nodes = document.nodes ?? {};
    for (const name of NODE_STRING_COLUMNS) column(nodes[name]);
    for (const pairs of nodes.attributes ?? []) column(pairs);
    for (const name of NODE_RARE_STRING_COLUMNS) column(nodes[name]?.value);
    const layout = document.layout ?? {};
    for (const name of LAYOUT_STRING_COLUMNS) column(layout[name]);
    for (const row of layout.styles ?? []) column(row);
  }
  return refs;
}

/// ASCII-lowercase an attribute value, as HTML matches enumerated attributes.
function asciiLower(value) {
  return String(value ?? "").replace(/[A-Z]/gu, (c) => c.toLowerCase());
}

/// Replace, in place, every string a masked control or masked text run holds
/// with the mask glyphs Chromium paints for it, so the persisted snapshot and
/// every sidecar derived from it carry no secret.
///
/// Masked means: an `<input type=password>` (matched as HTML matches the
/// attribute: ASCII case-insensitively, untrimmed); any laid-out node whose
/// computed `-webkit-text-security` is not `none`; and any `<input>` or
/// `<textarea>` Chromium did not lay out, whose computed style the snapshot
/// does not carry and whose value is never painted, so it is masked rather
/// than guessed. The slots masked are a masked control's `inputValue`,
/// `textValue` and `value` attribute, the `nodeValue` of a masked element's
/// text children, and a masked text run's `nodeValue` and layout text.
///
/// Each character becomes one glyph per UTF-16 code unit, so a text box's
/// start and length still index the masked string. The secret's own
/// string-table entry is overwritten, not orphaned; a reference outside the
/// masked slots that shared it (the same string painted in clear elsewhere)
/// is repointed to an appended copy. The result is a pure function of the
/// snapshot. Non-rendered text outside a form control keeps its captured
/// value: Chromium paints none of it and no computed style says it is masked.
export function maskSecretText(snapshot, styleNames) {
  const strings = snapshot?.strings;
  if (!Array.isArray(strings)) return { masked_slots: 0 };
  const securityAt = (styleNames ?? []).indexOf("-webkit-text-security");
  const maskedSlots = [];
  for (const document of snapshot.documents ?? []) {
    const nodes = document.nodes ?? {};
    const layout = document.layout ?? {};
    const names = nodes.nodeName ?? [];
    const parents = nodes.parentIndex ?? [];
    const rowsOf = new Map();
    (layout.nodeIndex ?? []).forEach((node, row) => {
      if (!rowsOf.has(node)) rowsOf.set(node, []);
      rowsOf.get(node).push(row);
    });
    const styleGlyph = (node) => {
      if (securityAt < 0) return null;
      for (const row of rowsOf.get(node) ?? []) {
        const value = strings[layout.styles?.[row]?.[securityAt]];
        if (typeof value === "string" && value !== "" && value !== "none")
          return MASK_GLYPHS.get(value) ?? DEFAULT_MASK_GLYPH;
      }
      return null;
    };
    const tagOf = (node) => asciiLower(strings[names[node]]);
    const attributeSlot = (node, wanted) => {
      const pairs = nodes.attributes?.[node] ?? [];
      for (let i = 0; i + 1 < pairs.length; i += 2) {
        if (asciiLower(strings[pairs[i]]) === wanted) return [pairs, i + 1];
      }
      return null;
    };
    const rareSlot = (name, node) => {
      const rare = nodes[name];
      const at = rare?.index?.indexOf(node) ?? -1;
      return at >= 0 && Array.isArray(rare.value) ? [rare.value, at] : null;
    };
    const elementGlyph = new Map();
    for (let node = 0; node < names.length; node += 1) {
      const tag = tagOf(node);
      let glyph = styleGlyph(node);
      if (tag === "input" || tag === "textarea") {
        const typeSlot = tag === "input" ? attributeSlot(node, "type") : null;
        const type = typeSlot ? asciiLower(strings[typeSlot[0][typeSlot[1]]]) : "";
        if (!glyph && (type === "password" || !rowsOf.has(node)))
          glyph = DEFAULT_MASK_GLYPH;
        if (!glyph) continue;
        elementGlyph.set(node, glyph);
        for (const slot of [
          rareSlot("inputValue", node), rareSlot("textValue", node),
          tag === "input" ? attributeSlot(node, "value") : null]) {
          if (slot) maskedSlots.push([...slot, glyph]);
        }
        continue;
      }
      if (glyph && nodes.nodeType?.[node] === 1) elementGlyph.set(node, glyph);
    }
    for (let node = 0; node < names.length; node += 1) {
      if (nodes.nodeType?.[node] !== 3) continue;
      const glyph = styleGlyph(node) ?? elementGlyph.get(parents[node]);
      if (!glyph) continue;
      if (Array.isArray(nodes.nodeValue))
        maskedSlots.push([nodes.nodeValue, node, glyph]);
      for (const row of rowsOf.get(node) ?? []) {
        if (Array.isArray(layout.text))
          maskedSlots.push([layout.text, row, glyph]);
      }
    }
  }
  const secrets = new Map();
  const appended = new Map();
  const intern = (value) => {
    if (!appended.has(value)) {
      appended.set(value, strings.length);
      strings.push(value);
    }
    return appended.get(value);
  };
  let count = 0;
  for (const [holder, key, glyph] of maskedSlots) {
    const index = holder[key];
    const value = strings[index];
    if (!Number.isInteger(index) || typeof value !== "string" || value === "")
      continue;
    const mask = glyph.repeat(value.length);
    if (mask === value) continue;
    secrets.set(index, mask);
    holder[key] = intern(mask);
    count += 1;
  }
  if (secrets.size > 0) {
    const originals = new Map(
      [...secrets.keys()].map((index) => [index, strings[index]]));
    for (const [holder, key] of stringReferences(snapshot)) {
      if (originals.has(holder[key]))
        holder[key] = intern(originals.get(holder[key]));
    }
    for (const [index, mask] of secrets) strings[index] = mask;
  }
  return { masked_slots: count };
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
