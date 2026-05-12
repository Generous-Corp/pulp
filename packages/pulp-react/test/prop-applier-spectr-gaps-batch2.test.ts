// 6 React-applier gaps surfaced by the runtime-import coverage sub-agent
// (planning/runtime-import-spectr-coverage-2026-05-11.md). Each prop is
// marked `supported` in compat.json (wired via web-compat-style-decl.js)
// but the @pulp/react applier was missing the `case`, so JSX `style={{…}}`
// silently dropped: outline (3 Spectr occurrences), overflowX/Y (3),
// whiteSpace (2), textOverflow (1), backdropFilter (12), backgroundImage (1).

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
});

function makeInstance(id: string = 'k'): PulpInstance {
    return {
        id,
        type: 'View' as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

const callsOf = (b: MockBridge, fn: string) => b.calls.filter((c) => c.fn === fn);

describe('prop-applier Spectr-gap batch 2 (6 React-applier gaps)', () => {
    it('outline shorthand fans out to setOutlineWidth/Style/Color', () => {
        applyChangedProps(makeInstance(), {}, { outline: '2px solid #ff0000' });
        expect(callsOf(bridge, 'setOutlineWidth')[0].args).toEqual(['k', 2]);
        expect(callsOf(bridge, 'setOutlineStyle')[0].args).toEqual(['k', 'solid']);
        expect(callsOf(bridge, 'setOutlineColor')[0].args).toEqual(['k', '#ff0000']);
    });

    it('overflowX aliases to setOverflow', () => {
        applyChangedProps(makeInstance(), {}, { overflowX: 'scroll' });
        const c = callsOf(bridge, 'setOverflow');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 'scroll']);
    });

    it('overflowY aliases to setOverflow', () => {
        applyChangedProps(makeInstance(), {}, { overflowY: 'hidden' });
        const c = callsOf(bridge, 'setOverflow');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 'hidden']);
    });

    it('whiteSpace dispatches setWhiteSpace', () => {
        applyChangedProps(makeInstance(), {}, { whiteSpace: 'nowrap' });
        const c = callsOf(bridge, 'setWhiteSpace');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 'nowrap']);
    });

    it('textOverflow dispatches setTextOverflow', () => {
        applyChangedProps(makeInstance(), {}, { textOverflow: 'ellipsis' });
        const c = callsOf(bridge, 'setTextOverflow');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 'ellipsis']);
    });

    it('backdropFilter blur(Npx) parses to numeric setter', () => {
        applyChangedProps(makeInstance(), {}, { backdropFilter: 'blur(20px)' });
        const c = callsOf(bridge, 'setBackdropFilter');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 20]);
    });

    it('backdropFilter `none` clears slot (0)', () => {
        applyChangedProps(makeInstance(), {}, { backdropFilter: 'none' });
        const c = callsOf(bridge, 'setBackdropFilter');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 0]);
    });

    it('backdropFilter unitless number parses', () => {
        applyChangedProps(makeInstance(), {}, { backdropFilter: 'blur(8)' });
        const c = callsOf(bridge, 'setBackdropFilter');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['k', 8]);
    });

    it('backgroundImage routes gradient through setBackgroundGradient', () => {
        applyChangedProps(makeInstance(), {}, {
            backgroundImage: 'linear-gradient(90deg, red, blue)',
        });
        // Codex P1 on #1831: setBackground only parses color tokens, so
        // gradient strings must route to setBackgroundGradient.
        const sg = callsOf(bridge, 'setBackgroundGradient');
        expect(sg).toHaveLength(1);
        expect(sg[0].args[1]).toMatch(/^linear-gradient/);
        // And NOT to setBackground (would clobber the color slot).
        expect(callsOf(bridge, 'setBackground')).toHaveLength(0);
    });

    it('backgroundImage url(...) is dropped quietly (no image bridge yet)', () => {
        applyChangedProps(makeInstance(), {}, {
            backgroundImage: 'url(/path/to/img.png)',
        });
        expect(callsOf(bridge, 'setBackground')).toHaveLength(0);
        expect(callsOf(bridge, 'setBackgroundGradient')).toHaveLength(0);
    });

    it('backgroundImage radial-gradient routes to setBackgroundGradient', () => {
        applyChangedProps(makeInstance(), {}, {
            backgroundImage: 'radial-gradient(circle, red, blue)',
        });
        expect(callsOf(bridge, 'setBackgroundGradient')).toHaveLength(1);
    });

    it('background shorthand with gradient routes to setBackgroundGradient', () => {
        // Same fix as backgroundImage — `background: linear-gradient(...)`
        // was previously sent to setBackground (color-only) producing a
        // bogus white background. Caught visually in Spectr's chip strip.
        applyChangedProps(makeInstance(), {}, {
            background: 'linear-gradient(to bottom, #111, #222)',
        });
        expect(callsOf(bridge, 'setBackgroundGradient')).toHaveLength(1);
        expect(callsOf(bridge, 'setBackground')).toHaveLength(0);
    });

    it('background shorthand with plain color still routes to setBackground', () => {
        applyChangedProps(makeInstance(), {}, { background: '#1a1a2e' });
        expect(callsOf(bridge, 'setBackground')).toHaveLength(1);
        expect(callsOf(bridge, 'setBackgroundGradient')).toHaveLength(0);
    });

    it('background `transparent` still routes to setBackground', () => {
        applyChangedProps(makeInstance(), {}, { background: 'transparent' });
        expect(callsOf(bridge, 'setBackground')).toHaveLength(1);
        expect(callsOf(bridge, 'setBackground')[0].args[1]).toBe('transparent');
    });

    it('outline shorthand with rgba color preserves the full color string', () => {
        applyChangedProps(makeInstance(), {},
            { outline: '1px solid rgba(0, 0, 0, 0.5)' });
        const cc = callsOf(bridge, 'setOutlineColor')[0];
        expect(cc.args[1]).toBe('rgba(0, 0, 0, 0.5)');
    });
});
