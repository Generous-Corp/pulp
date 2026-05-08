// Wave 2 rn — verify the @pulp/react prop-applier closes the 17 cheap
// value-coverage gaps the harness was flagging on the rn surface:
//
//   • length-value strings on `padding` / `margin` shorthands fan out
//     to the per-edge bridge keys (which already accept percent + auto)
//   • `lineHeight` unitless multiplier resolves with current fontSize
//   • `boxShadow` multi-shadow comma-separated lists dispatch one
//     setBoxShadow per parsed shadow (paren-depth-respecting split)
//   • `borderRadius` (and per-corner variants) accept the RN Fabric
//     elliptical `{ x, y }` form (degraded to averaged uniform)
//   • `viewBox` accepts the SVG-spec `'min-x min-y w h'` string form
//   • `fontWeight` numeric strings ('100'..'900') keep flowing through
//   • `cursor: 'auto'` and `textDecorationLine: 'underline line-through'`
//     compound forms keep round-tripping verbatim
//
// Each [wave2-rn] band test asserts on the mock-bridge call shape so
// the harness's prop-applier dispatch arc gets covered without spinning
// up the full Pulp runtime.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps, applyAllProps } from '../src/prop-applier.js';
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

function makeInstance(id: string = 'k', type: string = 'View', props: Record<string, unknown> = {}): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props,
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

function callsFor(b: MockBridge, fn: string) {
    return b.calls.filter((c) => c.fn === fn);
}

function setFlexCallsKey(b: MockBridge, key: string) {
    return b.calls.filter((c) => c.fn === 'setFlex' && c.args[1] === key);
}

describe('[wave2-rn] padding shorthand string forms', () => {
    it("padding: '5%' fans out to four per-edge percent calls", () => {
        applyChangedProps(makeInstance(), {}, { padding: '5%' });
        // Numeric shorthand is bypassed; string fan-out hits per-edge keys.
        expect(callsFor(bridge, 'setFlex').filter(c => c.args[1] === 'padding')).toHaveLength(0);
        expect(setFlexCallsKey(bridge, 'padding_top')).toEqual([{ fn: 'setFlex', args: ['k', 'padding_top', '5%'] }]);
        expect(setFlexCallsKey(bridge, 'padding_right')).toEqual([{ fn: 'setFlex', args: ['k', 'padding_right', '5%'] }]);
        expect(setFlexCallsKey(bridge, 'padding_bottom')).toEqual([{ fn: 'setFlex', args: ['k', 'padding_bottom', '5%'] }]);
        expect(setFlexCallsKey(bridge, 'padding_left')).toEqual([{ fn: 'setFlex', args: ['k', 'padding_left', '5%'] }]);
    });

    it("padding: '10px 20px' expands to top/bottom + left/right", () => {
        applyChangedProps(makeInstance(), {}, { padding: '10px 20px' });
        expect(setFlexCallsKey(bridge, 'padding_top')[0].args[2]).toBe(10);
        expect(setFlexCallsKey(bridge, 'padding_right')[0].args[2]).toBe(20);
        expect(setFlexCallsKey(bridge, 'padding_bottom')[0].args[2]).toBe(10);
        expect(setFlexCallsKey(bridge, 'padding_left')[0].args[2]).toBe(20);
    });

    it("numeric padding still uses the shorthand bridge key (no regression)", () => {
        applyChangedProps(makeInstance(), {}, { padding: 12 });
        const calls = setFlexCallsKey(bridge, 'padding');
        expect(calls).toEqual([{ fn: 'setFlex', args: ['k', 'padding', 12] }]);
        // No per-edge fan-out for numeric input.
        expect(setFlexCallsKey(bridge, 'padding_top')).toHaveLength(0);
    });
});

describe('[wave2-rn] margin shorthand string forms', () => {
    it("margin: 'auto' fans out to four per-edge auto calls", () => {
        applyChangedProps(makeInstance(), {}, { margin: 'auto' });
        expect(setFlexCallsKey(bridge, 'margin_top')[0].args[2]).toBe('auto');
        expect(setFlexCallsKey(bridge, 'margin_right')[0].args[2]).toBe('auto');
        expect(setFlexCallsKey(bridge, 'margin_bottom')[0].args[2]).toBe('auto');
        expect(setFlexCallsKey(bridge, 'margin_left')[0].args[2]).toBe('auto');
    });

    it("margin: '5%' fans out as percent strings", () => {
        applyChangedProps(makeInstance(), {}, { margin: '5%' });
        expect(setFlexCallsKey(bridge, 'margin_top')[0].args[2]).toBe('5%');
        expect(setFlexCallsKey(bridge, 'margin_left')[0].args[2]).toBe('5%');
    });

    it("margin: '0 auto' centers (top/bottom 0, left/right auto)", () => {
        applyChangedProps(makeInstance(), {}, { margin: '0 auto' });
        expect(setFlexCallsKey(bridge, 'margin_top')[0].args[2]).toBe(0);
        expect(setFlexCallsKey(bridge, 'margin_right')[0].args[2]).toBe('auto');
        expect(setFlexCallsKey(bridge, 'margin_bottom')[0].args[2]).toBe(0);
        expect(setFlexCallsKey(bridge, 'margin_left')[0].args[2]).toBe('auto');
    });

    it("numeric margin still uses the shorthand bridge key (no regression)", () => {
        applyChangedProps(makeInstance(), {}, { margin: 8 });
        expect(setFlexCallsKey(bridge, 'margin')).toEqual([{ fn: 'setFlex', args: ['k', 'margin', 8] }]);
    });
});

describe('[wave2-rn] width/height percent forwarding (rn.2 — already wired, regression coverage)', () => {
    it("width: '50%' flows to setFlex(width, '50%')", () => {
        applyChangedProps(makeInstance(), {}, { width: '50%' });
        expect(setFlexCallsKey(bridge, 'width')).toEqual([{ fn: 'setFlex', args: ['k', 'width', '50%'] }]);
    });
});

describe('[wave2-rn] fontWeight numeric weights', () => {
    function fwCall(): { fn: string; args: unknown[] } | undefined {
        return bridge.calls.find((c) => c.fn === 'setFontWeight');
    }
    it("'100' resolves to 100", () => {
        applyChangedProps(makeInstance('k', 'Label'), {}, { fontWeight: '100' });
        expect(fwCall()?.args).toEqual(['k', 100]);
    });
    it("'500' resolves to 500", () => {
        applyChangedProps(makeInstance('k', 'Label'), {}, { fontWeight: '500' });
        expect(fwCall()?.args).toEqual(['k', 500]);
    });
    it("'700' resolves to 700", () => {
        applyChangedProps(makeInstance('k', 'Label'), {}, { fontWeight: '700' });
        expect(fwCall()?.args).toEqual(['k', 700]);
    });
    it("'900' resolves to 900", () => {
        applyChangedProps(makeInstance('k', 'Label'), {}, { fontWeight: '900' });
        expect(fwCall()?.args).toEqual(['k', 900]);
    });
});

describe('[wave2-rn] lineHeight unitless multiplier', () => {
    function lhCall(): { fn: string; args: unknown[] } | undefined {
        return bridge.calls.find((c) => c.fn === 'setLineHeight');
    }

    it("lineHeight: 1.5 with fontSize: 16 resolves to 24", () => {
        const inst = makeInstance('k', 'Label', { fontSize: 16, lineHeight: 1.5 });
        applyAllProps(inst);
        expect(lhCall()?.args).toEqual(['k', 24]);
    });

    it("lineHeight: 1.5 with no fontSize defaults via 14 → 21", () => {
        const inst = makeInstance('k', 'Label', { lineHeight: 1.5 });
        applyAllProps(inst);
        expect(lhCall()?.args).toEqual(['k', 21]);
    });

    it("lineHeight: 24 (large value) flows through as absolute pixels", () => {
        applyChangedProps(makeInstance('k', 'Label'), {}, { lineHeight: 24 });
        expect(lhCall()?.args).toEqual(['k', 24]);
    });

    it("lineHeight: '1.25' (numeric string) treated as multiplier", () => {
        const inst = makeInstance('k', 'Label', { fontSize: 20, lineHeight: '1.25' });
        applyAllProps(inst);
        expect(lhCall()?.args).toEqual(['k', 25]);
    });

    it("lineHeight: '24px' strips suffix and treats as absolute", () => {
        applyChangedProps(makeInstance('k', 'Label'), {}, { lineHeight: '24px' });
        expect(lhCall()?.args).toEqual(['k', 24]);
    });
});

describe('[wave2-rn] cursor pass-through (auto + custom)', () => {
    it("cursor: 'auto' forwards verbatim", () => {
        applyChangedProps(makeInstance(), {}, { cursor: 'auto' });
        expect(bridge.calls.find(c => c.fn === 'setCursor')?.args).toEqual(['k', 'auto']);
    });
});

describe('[wave2-rn] textDecorationLine compound', () => {
    it("'underline line-through' forwards verbatim to setTextDecoration", () => {
        applyChangedProps(makeInstance('k', 'Label'), {}, { textDecorationLine: 'underline line-through' });
        const c = bridge.calls.find(x => x.fn === 'setTextDecoration');
        expect(c?.args).toEqual(['k', 'underline line-through']);
    });
});

describe('[wave2-rn] boxShadow multi-shadow', () => {
    function shadowCalls() {
        return bridge.calls.filter((c) => c.fn === 'setBoxShadow');
    }

    it("two shadows separated by comma dispatch two setBoxShadow calls", () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: '2px 2px 4px black, 0px 0px 8px red',
        });
        const calls = shadowCalls();
        expect(calls).toHaveLength(2);
        expect(calls[0].args).toEqual(['k', 2, 2, 4, 0, 'black', false]);
        expect(calls[1].args).toEqual(['k', 0, 0, 8, 0, 'red', false]);
    });

    it("commas inside rgba(...) literals don't split a single shadow", () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: '2px 4px 8px rgba(0,0,0,0.3), 0px 0px 4px rgba(255,0,0,0.5)',
        });
        const calls = shadowCalls();
        expect(calls).toHaveLength(2);
        expect(calls[0].args[5]).toBe('rgba(0,0,0,0.3)');
        expect(calls[1].args[5]).toBe('rgba(255,0,0,0.5)');
    });

    it("inset keyword on one shadow doesn't leak to the next", () => {
        applyChangedProps(makeInstance(), {}, {
            boxShadow: 'inset 1px 2px 3px black, 4px 5px 6px red',
        });
        const calls = shadowCalls();
        expect(calls).toHaveLength(2);
        expect(calls[0].args[6]).toBe(true);
        expect(calls[1].args[6]).toBe(false);
    });

    it("single-shadow input still emits a single call (no regression)", () => {
        applyChangedProps(makeInstance(), {}, { boxShadow: '2px 4px 8px rgba(0,0,0,0.3)' });
        expect(shadowCalls()).toHaveLength(1);
    });
});

describe('[wave2-rn] borderRadius elliptical x/y', () => {
    it("borderRadius: { x: 10, y: 6 } degrades to averaged uniform 8", () => {
        applyChangedProps(makeInstance(), {}, { borderRadius: { x: 10, y: 6 } });
        expect(bridge.calls.find(c => c.fn === 'setBorderRadius')?.args).toEqual(['k', 8]);
    });

    it("borderTopLeftRadius: { x: 12, y: 4 } uses averaged uniform 8", () => {
        applyChangedProps(makeInstance(), {}, { borderTopLeftRadius: { x: 12, y: 4 } });
        expect(bridge.calls.find(c => c.fn === 'setBorderTopLeftRadius')?.args).toEqual(['k', 8]);
    });

    it("borderRadius: 12 (numeric) flows through unchanged (no regression)", () => {
        applyChangedProps(makeInstance(), {}, { borderRadius: 12 });
        expect(bridge.calls.find(c => c.fn === 'setBorderRadius')?.args).toEqual(['k', 12]);
    });
});

describe('[wave2-rn] viewBox SVG-style string form', () => {
    function vbCall(): { fn: string; args: unknown[] } | undefined {
        return bridge.calls.find((c) => c.fn === 'setSvgViewBox');
    }

    it("'0 0 24 24' extracts (24, 24) as width/height", () => {
        applyChangedProps(makeInstance('k', 'SvgPath'), {}, { viewBox: '0 0 24 24' });
        expect(vbCall()?.args).toEqual(['k', 24, 24]);
    });

    it("'-10 -10 100 50' picks up the trailing two tokens", () => {
        applyChangedProps(makeInstance('k', 'SvgPath'), {}, { viewBox: '-10 -10 100 50' });
        expect(vbCall()?.args).toEqual(['k', 100, 50]);
    });

    it("comma-separated form still parses", () => {
        applyChangedProps(makeInstance('k', 'SvgPath'), {}, { viewBox: '0,0,32,32' });
        expect(vbCall()?.args).toEqual(['k', 32, 32]);
    });

    it("two-token string '40 20' becomes (40, 20)", () => {
        applyChangedProps(makeInstance('k', 'SvgPath'), {}, { viewBox: '40 20' });
        expect(vbCall()?.args).toEqual(['k', 40, 20]);
    });

    it("array form still works (no regression)", () => {
        applyChangedProps(makeInstance('k', 'SvgPath'), {}, { viewBox: [16, 16] as [number, number] });
        expect(vbCall()?.args).toEqual(['k', 16, 16]);
    });
});
