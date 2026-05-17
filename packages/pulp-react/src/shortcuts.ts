// @pulp/react keyboard shortcut runtime injection (pulp #135 Phase B).
//
// Counterpart to the pulp-import-design CLI's static codegen path
// (pulp #2128). That path emits `registerShortcut(key, mod, cbName)`
// + a separate top-level callback function into the generated ui.js.
// This module gives React app authors the same surface at runtime via
// a hook, plus clash detection so two parts of the app (or one app +
// the import-design path) registering the same chord can be surfaced
// instead of silently overwriting.
//
// Architecture: one dispatcher callback per unique (keyCode, modMask)
// pair is registered C++-side. The dispatcher consults a JS-side
// registry keyed by the chord and fires every live handler. Handlers
// are reference-counted so a component re-mount cleans up cleanly.
//
// C++ surface (see core/view/src/widget_bridge.cpp `registerShortcut`):
//
//   registerShortcut(keyCode: int, modMask: int, callbackName: string)
//
// There is no `unregisterShortcut` C++-side — the dispatcher pattern
// here is what makes runtime unregistration possible without a bridge
// change. (Re-registering with the same dispatcher name is harmless;
// the C++ side stores the registration in a vector and looks up the
// callback by name at fire time.)

import { useEffect } from 'react';

/// Modifier mask values mirroring core/view/include/pulp/view/input_events.hpp
/// `kMod*` constants. Kept here so the parser can produce them without
/// importing anything from the C++ side.
export const MOD_SHIFT = 1 << 0;
export const MOD_CTRL  = 1 << 1;
export const MOD_ALT   = 1 << 2;
export const MOD_META  = 1 << 3;
export const MOD_CMD   = 1 << 4;

export interface ParsedShortcut {
    /// Lowercased + sorted modifier names, joined with '+', for a
    /// stable binding key independent of input formatting.
    readonly canonical: string;
    readonly keyCode: number;
    readonly modMask: number;
}

/// Parse a string like 'cmd+,', 'shift+s', or 'escape' into the
/// (keyCode, modMask) shape the C++ `registerShortcut` expects.
/// Throws if the spec is empty or contains no non-modifier key.
///
/// Recognized modifier tokens (case-insensitive):
///   cmd / meta — kModCmd (macOS Command)
///   ctrl       — kModCtrl
///   shift      — kModShift
///   alt / opt  — kModAlt
///
/// Recognized key tokens: single ASCII characters (a-z, 0-9, punctuation
/// that matches the keycode_to_w3c_key mapping in core/view/src/input_events.cpp),
/// or named keys: escape, enter/return, tab, space, backspace, delete,
/// arrowup/arrowdown/arrowleft/arrowright, f1-f12.
export function parseShortcut(spec: string): ParsedShortcut {
    if (typeof spec !== 'string' || spec.length === 0) {
        throw new Error(`[useShortcut] empty spec`);
    }
    const tokens = spec.toLowerCase().split('+').map(t => t.trim()).filter(Boolean);
    if (tokens.length === 0) {
        throw new Error(`[useShortcut] no tokens in spec: ${spec}`);
    }

    let modMask = 0;
    const mods: string[] = [];
    let keyToken: string | undefined;

    for (const t of tokens) {
        switch (t) {
            case 'cmd':   case 'command':  modMask |= MOD_CMD;   mods.push('cmd'); continue;
            case 'meta':                   modMask |= MOD_META;  mods.push('meta'); continue;
            case 'ctrl':  case 'control':  modMask |= MOD_CTRL;  mods.push('ctrl'); continue;
            case 'shift':                  modMask |= MOD_SHIFT; mods.push('shift'); continue;
            case 'alt':   case 'opt':
            case 'option':                 modMask |= MOD_ALT;   mods.push('alt'); continue;
        }
        if (keyToken !== undefined) {
            throw new Error(`[useShortcut] multiple key tokens in spec: ${spec}`);
        }
        keyToken = t;
    }

    if (keyToken === undefined) {
        throw new Error(`[useShortcut] no key token in spec: ${spec}`);
    }

    const keyCode = keyTokenToKeyCode(keyToken);
    if (keyCode === undefined) {
        throw new Error(`[useShortcut] unrecognized key token "${keyToken}" in spec: ${spec}`);
    }

    mods.sort();
    const canonical = mods.length > 0 ? `${mods.join('+')}+${keyToken}` : keyToken;
    return { canonical, keyCode, modMask };
}

function keyTokenToKeyCode(token: string): number | undefined {
    // Single-char ASCII path — matches the same mapping space the
    // platform host uses (NSEvent.charactersIgnoringModifiers
    // returns the unmodified char, then PulpView casts it to
    // KeyCode). 'a'..'z' and '0'..'9' are passed verbatim, plus a
    // handful of punctuation that has a clean ASCII keycode.
    if (token.length === 1) return token.charCodeAt(0);

    switch (token) {
        case 'escape':  case 'esc':       return 27;
        case 'enter':   case 'return':    return 13;
        case 'tab':                       return 9;
        case 'space':                     return 32;
        case 'backspace':                 return 8;
        case 'delete':                    return 127;
        case 'arrowup':                   return 0x26;
        case 'arrowdown':                 return 0x28;
        case 'arrowleft':                 return 0x25;
        case 'arrowright':                return 0x27;
    }
    if (/^f([1-9]|1[0-2])$/.test(token)) {
        return 0x70 + (parseInt(token.slice(1), 10) - 1);
    }
    return undefined;
}

// ── Runtime registry ───────────────────────────────────────────────

interface Registration {
    spec: string;
    canonical: string;
    handler: () => void;
    source: string;
}

type GlobalWithRegistry = typeof globalThis & {
    __pulpShortcutRegistry__?: Map<string, Registration[]>;
    __pulpShortcutDispatchers__?: Set<string>;
    [k: string]: unknown;
};

/// Stable global registry — exposed on `globalThis` so the
/// pulp-import-design CLI (which emits static `registerShortcut` calls
/// into the generated ui.js) can also write into it and clash detection
/// works across both layers in the same JS engine.
export function getRegistry(): Map<string, Registration[]> {
    const g = globalThis as GlobalWithRegistry;
    if (!g.__pulpShortcutRegistry__) g.__pulpShortcutRegistry__ = new Map();
    return g.__pulpShortcutRegistry__;
}

function getDispatcherSet(): Set<string> {
    const g = globalThis as GlobalWithRegistry;
    if (!g.__pulpShortcutDispatchers__) g.__pulpShortcutDispatchers__ = new Set();
    return g.__pulpShortcutDispatchers__;
}

/// Ensure exactly one C++-side `registerShortcut` is in flight for the
/// given (keyCode, modMask). Subsequent calls for the same chord are
/// no-ops on the C++ side because the dispatcher name is stable.
function ensureDispatcher(parsed: ParsedShortcut): void {
    const dispatcherName = `__pulpShortcutDispatcher_${parsed.keyCode}_${parsed.modMask}__`;
    const set = getDispatcherSet();
    if (set.has(dispatcherName)) return;

    const g = globalThis as GlobalWithRegistry;
    g[dispatcherName] = () => {
        const list = getRegistry().get(parsed.canonical);
        if (!list || list.length === 0) return;
        // Latest-registered wins. React useEffect cleanup runs in
        // reverse mount order, so this matches the "active component
        // owns the shortcut" intuition.
        const top = list[list.length - 1];
        try { top.handler(); }
        catch (e) { console.error(`[useShortcut] handler for "${top.spec}" threw:`, e); }
    };

    const registerShortcut = (globalThis as GlobalWithRegistry).registerShortcut as
        | ((k: number, m: number, n: string) => void)
        | undefined;
    if (typeof registerShortcut === 'function') {
        registerShortcut(parsed.keyCode, parsed.modMask, dispatcherName);
    }
    set.add(dispatcherName);
}

/// Register a handler for the given shortcut spec. Returns an
/// unregister fn. Useful for non-React call sites (the import-design
/// runtime injection path) and as the primitive `useShortcut` builds
/// on. `source` is a free-form label that appears in clash warnings —
/// pass something the user can grep for (`'spectr-mode-switch'`, the
/// component's display name, etc.).
export function registerShortcut(
    spec: string,
    handler: () => void,
    source: string = 'unknown',
): () => void {
    const parsed = parseShortcut(spec);
    const reg = getRegistry();
    const list = reg.get(parsed.canonical) ?? [];

    // Clash detection: anyone already on this chord with a DIFFERENT
    // source gets a console.warn. Same-source re-registration is a
    // re-mount and stays quiet.
    if (list.length > 0) {
        const conflicting = list.filter(r => r.source !== source);
        if (conflicting.length > 0) {
            const sources = conflicting.map(r => r.source).join(', ');
            console.warn(
                `[useShortcut] shortcut clash on "${parsed.canonical}": ` +
                `new registration from "${source}" overlays existing ` +
                `${conflicting.length} registration(s) from [${sources}]. ` +
                `Latest registration wins on fire.`,
            );
        }
    }

    const entry: Registration = { spec, canonical: parsed.canonical, handler, source };
    list.push(entry);
    reg.set(parsed.canonical, list);

    ensureDispatcher(parsed);

    return function unregister() {
        const cur = reg.get(parsed.canonical);
        if (!cur) return;
        const idx = cur.lastIndexOf(entry);
        if (idx >= 0) cur.splice(idx, 1);
        if (cur.length === 0) reg.delete(parsed.canonical);
    };
}

/// React hook: register a keyboard shortcut for the lifetime of the
/// component. `spec` is parsed once per change; pass dependencies
/// inside `deps` so the handler captures the right closure (same
/// semantics as React.useEffect).
///
/// Example:
///   useShortcut('cmd+,', () => setSettingsOpen(true));
///   useShortcut('escape', () => setDropdownOpen(false), [setDropdownOpen]);
export function useShortcut(
    spec: string,
    handler: () => void,
    deps: ReadonlyArray<unknown> = [],
    source?: string,
): void {
    useEffect(
        () => {
            const lbl = source ?? `useShortcut(${spec})`;
            return registerShortcut(spec, handler, lbl);
        },
        // ESLint may want `handler` here; intentionally omitted so
        // the caller controls re-registration through `deps`.
        // eslint-disable-next-line react-hooks/exhaustive-deps
        [spec, source, ...deps],
    );
}

// ── Test/diagnostic helpers ────────────────────────────────────────

/// Snapshot the current registry. Intended for tests/diagnostics —
/// not part of the supported runtime surface.
export function _debugSnapshotRegistry(): Array<{
    canonical: string;
    count: number;
    sources: string[];
}> {
    const out: Array<{ canonical: string; count: number; sources: string[] }> = [];
    for (const [canonical, list] of getRegistry().entries()) {
        out.push({ canonical, count: list.length, sources: list.map(r => r.source) });
    }
    return out;
}

/// Reset the registry — tests only.
export function _debugResetRegistry(): void {
    const g = globalThis as GlobalWithRegistry;
    g.__pulpShortcutRegistry__ = new Map();
    g.__pulpShortcutDispatchers__ = new Set();
}
