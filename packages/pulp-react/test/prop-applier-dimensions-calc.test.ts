// Verify the @pulp/react prop-applier resolves CSS math functions
// (`calc()` / `min()` / `max()` / `clamp()`) on dimension props before
// they reach the bridge.
//
// The bridge parses a dimension string as `<number><unit>`, so a math
// function arrives as an unparseable token and is discarded without a
// diagnostic — the element silently keeps `auto`. An auto-height flex
// column then hugs its non-flexible children, and any `flex: 1;
// minHeight: 0` child inside it resolves to zero height and is clipped
// away entirely.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;
let priorResolve: unknown;
let priorRoot: unknown;

// The CSS length resolver and root-size accessor are realm globals
// supplied by the host, not module imports. Stand them up here so the
// applier exercises the same lookup it performs at runtime.
function installLengthGlobals(rootW: number, rootH: number) {
    const g = globalThis as unknown as Record<string, unknown>;
    priorResolve = g.resolveCSSLength;
    priorRoot = g.getRootSize;
    {
        g.resolveCSSLength = (str: string, ctx?: { viewportW?: number; viewportH?: number }) => {
            const s = String(str).trim();
            const inner = s.slice(s.indexOf('(') + 1, s.lastIndexOf(')'));
            const terms = inner.split(',').map((t) => t.trim());
            const units = new Set(terms.map((t) => t.replace(/^[-0-9.]+/, '')));
            const nums = terms.map((t) => parseFloat(t));
            if (units.size === 1) {
                const unit = [...units][0];
                const v = s.startsWith('max(') ? Math.max(...nums) : Math.min(...nums);
                return { value: v, unit: unit === '' ? 'px' : unit };
            }
            const px = terms.map((t, i) => {
                const u = t.replace(/^[-0-9.]+/, '');
                if (u === 'vh') return nums[i] / 100 * (ctx?.viewportH ?? 600);
                if (u === 'vw') return nums[i] / 100 * (ctx?.viewportW ?? 800);
                return nums[i];
            });
            return { value: s.startsWith('max(') ? Math.max(...px) : Math.min(...px), unit: 'px' };
        };
    }
    g.getRootSize = () => ({ width: rootW, height: rootH });
}

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
    installLengthGlobals(1320, 860);
});
afterEach(() => {
    bridge.uninstall();
    const g = globalThis as unknown as Record<string, unknown>;
    if (priorResolve === undefined) delete g.resolveCSSLength; else g.resolveCSSLength = priorResolve;
    if (priorRoot === undefined) delete g.getRootSize; else g.getRootSize = priorRoot;
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

function flexArg(b: MockBridge, slot: string) {
    const calls = b.calls.filter((c) => c.fn === 'setFlex' && c.args[1] === slot);
    expect(calls).toHaveLength(1);
    return calls[0].args[2];
}

describe('prop-applier dimension CSS math functions', () => {
    it.each([
        ['width',     'width'],
        ['height',    'height'],
        ['minWidth',  'min_width'],
        ['minHeight', 'min_height'],
        ['maxWidth',  'max_width'],
        ['maxHeight', 'max_height'],
        ['flexBasis', 'flex_basis'],
    ])('%s resolves a mixed-unit min() against the live viewport', (jsxKey, slot) => {
        applyChangedProps(makeInstance(), {}, { [jsxKey]: 'min(92vh, 1500px)' });
        // 92vh of an 860px viewport is 791.2, which is the smaller term.
        expect(flexArg(bridge, slot)).toBeCloseTo(791.2, 3);
    });

    it('a single-unit expression keeps its unit so the bridge resolves it', () => {
        applyChangedProps(makeInstance(), {}, { height: 'max(40vh, 90vh)' });
        expect(flexArg(bridge, 'height')).toBe('90vh');
    });

    it('a percent-only expression stays on the bridge percent path', () => {
        applyChangedProps(makeInstance(), {}, { width: 'min(50%, 80%)' });
        expect(flexArg(bridge, 'width')).toBe('50%');
    });

    it('clamp() resolves to a finite px number, never a raw token', () => {
        applyChangedProps(makeInstance(), {}, { height: 'clamp(100px, 50vh, 200px)' });
        const v = flexArg(bridge, 'height');
        expect(typeof v).toBe('number');
        expect(Number.isFinite(v as number)).toBe(true);
    });

    it.each([
        ['a plain number',   240,     240],
        ['a percent string', '50%',   '50%'],
        ['the auto keyword', 'auto',  'auto'],
        ['a px string',      '120px', '120px'],
    ])('leaves %s untouched', (_label, input, expected) => {
        applyChangedProps(makeInstance(), {}, { height: input });
        expect(flexArg(bridge, 'height')).toEqual(expected);
    });
});
