// web-compat-runtime-apis.js — Phase 9 runtime API shims
//
// Extracted verbatim from the legacy web-compat.js bundle. Provides the
// browser runtime globals: performance, navigator.clipboard, localStorage /
// sessionStorage, Image, btoa / atob, console.time / timeEnd, crypto,
// TextEncoder / TextDecoder, structuredClone, and fetch. This bundle is not
// part of the runtime `PULP_JS_PRELUDES` chain (see core/view/CMakeLists.txt);
// it is read verbatim by the harness html adapter
// (tools/harness/adapters/html.py).

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 9: Runtime APIs
// ═══════════════════════════════════════════════════════════════════════════════

// performance.now() — high-resolution monotonic time
var performance = {
    now: function() {
        if (typeof __performanceNow__ === "function") return __performanceNow__();
        return Date.now();
    }
};

if (typeof globalThis.window === "undefined") {
    globalThis.window = window;
}

if (typeof globalThis.self === "undefined") {
    globalThis.self = globalThis.window;
}

if (typeof globalThis.requestAnimationFrame === "undefined") {
    globalThis.requestAnimationFrame = function(fn) {
        return globalThis.window.requestAnimationFrame(fn);
    };
}

if (typeof globalThis.cancelAnimationFrame === "undefined") {
    globalThis.cancelAnimationFrame = function(id) {
        return globalThis.window.cancelAnimationFrame(id);
    };
}

// navigator.clipboard — read/write clipboard
if (!window.navigator.clipboard) {
    window.navigator.clipboard = {
        readText: function() {
            if (typeof readClipboard === "function") return readClipboard();
            return "";
        },
        writeText: function(text) {
            if (typeof writeClipboard === "function") writeClipboard(text);
        }
    };
}

// localStorage — file-backed key-value store
var localStorage = {
    getItem: function(key) {
        if (typeof storageGetItem === "function") {
            var v = storageGetItem(key);
            return v || null;
        }
        return null;
    },
    setItem: function(key, value) {
        if (typeof storageSetItem === "function") storageSetItem(key, String(value));
    },
    removeItem: function(key) {
        if (typeof storageRemoveItem === "function") storageRemoveItem(key);
    },
    clear: function() { /* would need list of all keys */ },
    get length() { return 0; },
    key: function(i) { return null; }
};
var sessionStorage = localStorage; // alias

// Image constructor — for preloading images
function Image() {
    var self = this;
    self.src = "";
    self.width = 0;
    self.height = 0;
    self.onload = null;
    self.onerror = null;
    self.complete = false;

    Object.defineProperty(self, "src", {
        get: function() { return self._src || ""; },
        set: function(v) {
            self._src = v;
            if (v && typeof createImage === "function") {
                var id = __genId__();
                createImage(id, "");
                if (typeof setImageSource === "function") setImageSource(id, v);
                self.complete = true;
                if (self.onload) self.onload();
            }
        }
    });
}

// atob / btoa — base64 encoding/decoding
function btoa(str) {
    var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    var out = "";
    for (var i = 0; i < str.length; i += 3) {
        var a = str.charCodeAt(i);
        var b = i + 1 < str.length ? str.charCodeAt(i + 1) : 0;
        var c = i + 2 < str.length ? str.charCodeAt(i + 2) : 0;
        out += chars[(a >> 2) & 63];
        out += chars[((a << 4) | (b >> 4)) & 63];
        out += i + 1 < str.length ? chars[((b << 2) | (c >> 6)) & 63] : "=";
        out += i + 2 < str.length ? chars[c & 63] : "=";
    }
    return out;
}

function atob(encoded) {
    var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    var out = "";
    for (var i = 0; i < encoded.length; i += 4) {
        var a = chars.indexOf(encoded[i]);
        var b = chars.indexOf(encoded[i + 1]);
        var c = chars.indexOf(encoded[i + 2]);
        var d = chars.indexOf(encoded[i + 3]);
        out += String.fromCharCode((a << 2) | (b >> 4));
        if (c !== 64) out += String.fromCharCode(((b << 4) | (c >> 2)) & 255);
        if (d !== 64) out += String.fromCharCode(((c << 6) | d) & 255);
    }
    return out;
}

// console.time / timeEnd
var __timers__ = {};
if (typeof console !== "undefined") {
    console.time = function(label) {
        __timers__[label || "default"] = performance.now();
    };
    console.timeEnd = function(label) {
        var key = label || "default";
        if (__timers__[key] !== undefined) {
            var elapsed = performance.now() - __timers__[key];
            console.log(key + ": " + elapsed.toFixed(3) + "ms");
            delete __timers__[key];
        }
    };
}

// crypto.getRandomValues — for UUID generation
var crypto = {
    getRandomValues: function(arr) {
        for (var i = 0; i < arr.length; i++) {
            arr[i] = Math.floor(Math.random() * 256);
        }
        return arr;
    }
};

// TextEncoder / TextDecoder — UTF-8 encoding
function TextEncoder() {}
TextEncoder.prototype.encode = function(str) {
    var arr = [];
    for (var i = 0; i < str.length; i++) {
        var c = str.charCodeAt(i);
        if (c < 128) arr.push(c);
        else if (c < 2048) { arr.push(192 | (c >> 6)); arr.push(128 | (c & 63)); }
        else { arr.push(224 | (c >> 12)); arr.push(128 | ((c >> 6) & 63)); arr.push(128 | (c & 63)); }
    }
    return new Uint8Array(arr);
};

function TextDecoder() {}
TextDecoder.prototype.decode = function(buf) {
    var out = "";
    var arr = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    for (var i = 0; i < arr.length; ) {
        var b = arr[i];
        if (b < 128) { out += String.fromCharCode(b); i++; }
        else if (b < 224) { out += String.fromCharCode(((b & 31) << 6) | (arr[i+1] & 63)); i += 2; }
        else { out += String.fromCharCode(((b & 15) << 12) | ((arr[i+1] & 63) << 6) | (arr[i+2] & 63)); i += 3; }
    }
    return out;
};

// structuredClone — deep copy
function structuredClone(obj) {
    return JSON.parse(JSON.stringify(obj));
}

// fetch — minimal implementation via exec (P3)
function fetch(url, opts) {
    return new Promise(function(resolve, reject) {
        try {
            var method = (opts && opts.method) || "GET";
            var cmd = "curl -s";
            if (method !== "GET") cmd += " -X " + method;
            if (opts && opts.headers) {
                for (var k in opts.headers) cmd += " -H '" + k + ": " + opts.headers[k] + "'";
            }
            if (opts && opts.body) cmd += " -d '" + opts.body + "'";
            cmd += " '" + url + "'";
            var body = exec(cmd);
            resolve({
                ok: true, status: 200,
                text: function() { return body; },
                json: function() { return JSON.parse(body); }
            });
        } catch (e) { reject(e); }
    });
}
