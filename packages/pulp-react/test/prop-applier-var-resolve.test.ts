// pulp #1899 (gap #3) — `var(--name)` references in string-valued
// style props must be resolved BEFORE the value reaches the bridge.
// The raw string "var(--mono)" gives Skia's font matcher nothing to
// match against, so the literal flows through silently and the rendered
// text falls back to a proportional sans (Spectr top-bar "faint label"
// symptom, compounded by the opacity-layer LCD AA degradation handled
// on the C++ side).
//
// Resolution tiers (first hit wins): __pulpCssVars registry →
// getStringToken bridge → getMotionToken bridge → explicit fallback →
// original string passthrough.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
    delete (globalThis as Record<string, unknown>).__pulpCssVars;
});

function makeInstance(id = 'l', type: string = 'Label'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

function callsOf(name: string) {
    return bridge.calls.filter((c) => c.fn === name);
}

describe('prop-applier var() resolution (pulp #1899 gap #3)', () => {
    it('fontFamily: var(--mono) resolves via __pulpCssVars registry', () => {
        (globalThis as Record<string, unknown>).__pulpCssVars = {
            mono: 'JetBrains Mono',
        };
        applyChangedProps(makeInstance(), {}, { fontFamily: 'var(--mono)' });
        const calls = callsOf('setFontFamily');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['l', 'JetBrains Mono']);
    });

    it('fontFamily: var(--mono) resolves via getStringToken bridge call', () => {
        // Install a getStringToken stub on globalThis (mirrors what the
        // C++ bridge installs at runtime).
        (globalThis as Record<string, unknown>).getStringToken = (
            name: string,
        ) => (name === 'mono' ? 'JetBrains Mono' : '');
        applyChangedProps(makeInstance(), {}, { fontFamily: 'var(--mono)' });
        const calls = callsOf('setFontFamily');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['l', 'JetBrains Mono']);
        delete (globalThis as Record<string, unknown>).getStringToken;
    });

    it('color: var(--ink) resolves and dispatches setTextColor', () => {
        (globalThis as Record<string, unknown>).__pulpCssVars = {
            ink: '#e8e8e8',
        };
        applyChangedProps(makeInstance(), {}, { color: 'var(--ink)' });
        const calls = callsOf('setTextColor');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['l', '#e8e8e8']);
    });

    it('borderColor: var(--panel-border) resolves through the registry', () => {
        (globalThis as Record<string, unknown>).__pulpCssVars = {
            'panel-border': '#333',
        };
        applyChangedProps(makeInstance(), {}, { borderColor: 'var(--panel-border)' });
        const calls = callsOf('setBorderColor');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['l', '#333']);
    });

    it('falls through to explicit fallback when token is missing', () => {
        // No registry, no bridge stubs — resolver should use the
        // fallback inside the var() expression.
        applyChangedProps(makeInstance(), {}, {
            fontFamily: 'var(--missing, "SF Mono")',
        });
        const calls = callsOf('setFontFamily');
        expect(calls).toHaveLength(1);
        // The fallback is the literal string after the comma, trimmed.
        // Quoted strings keep their quotes — Skia's font matcher strips
        // them (issue-932 commit list test).
        expect(calls[0].args[1]).toBe('"SF Mono"');
    });

    it('passes a non-var literal through unchanged', () => {
        applyChangedProps(makeInstance(), {}, { fontFamily: 'Inter' });
        const calls = callsOf('setFontFamily');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['l', 'Inter']);
    });

    it('resolves nested var(--a, var(--b, default)) via fallback chain', () => {
        (globalThis as Record<string, unknown>).__pulpCssVars = {
            // --a is undefined, --b resolves; the resolver should walk
            // into the nested fallback and pick --b's value.
            b: 'JetBrains Mono',
        };
        applyChangedProps(makeInstance(), {}, {
            fontFamily: 'var(--a, var(--b, monospace))',
        });
        const calls = callsOf('setFontFamily');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['l', 'JetBrains Mono']);
    });

    it('passes the original string through when no tier resolves', () => {
        // No registry, no fallback — emit the input so downstream code
        // can log "unresolved var token". Better than swallowing
        // silently or substituting "".
        applyChangedProps(makeInstance(), {}, { fontFamily: 'var(--ghost)' });
        const calls = callsOf('setFontFamily');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['l', 'var(--ghost)']);
    });

    it('does NOT mangle numeric / object values', () => {
        applyChangedProps(makeInstance(), {}, { fontSize: 14 });
        const calls = callsOf('setFontSize');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['l', 14]);
    });
});
