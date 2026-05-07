// pulp #1434 batch 3 — verify the @pulp/react prop-applier translates
// CSS / RN fontWeight keyword forms (`'normal'`, `'bold'`, `'lighter'`,
// `'bolder'`) to numeric weights before reaching the bridge.
//
// Pre-fix: `Number('bold')` returned NaN, the bridge then defaulted to
// 400 — silently mapping bold to normal. This drift is recorded in
// compat.json (`css/fontWeight`) and mirrored on the JS CSS shim
// (`web-compat-style-decl.js`). Both paths must agree so design-tool
// exports (Figma, Stitch, v0, Claude Design) and React-Native style
// objects produce the same Label::font_weight() result.

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

function makeInstance(id: string = 'k', type: string = 'Label'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

function fontWeightCall(): { fn: string; args: unknown[] } | undefined {
    return bridge.calls.find((c) => c.fn === 'setFontWeight');
}

describe("prop-applier fontWeight keyword translation (pulp #1434 batch 3)", () => {
    it("'normal' keyword maps to 400", () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: 'normal' });
        expect(fontWeightCall()?.args).toEqual(['k', 400]);
    });

    it("'bold' keyword maps to 700", () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: 'bold' });
        expect(fontWeightCall()?.args).toEqual(['k', 700]);
    });

    it("'lighter' keyword maps to 300", () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: 'lighter' });
        expect(fontWeightCall()?.args).toEqual(['k', 300]);
    });

    it("'bolder' keyword maps to 700", () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: 'bolder' });
        expect(fontWeightCall()?.args).toEqual(['k', 700]);
    });

    it('numeric values still flow through unchanged (no regression)', () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: 500 });
        expect(fontWeightCall()?.args).toEqual(['k', 500]);
    });

    it('numeric string flows through as a number', () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: '700' });
        expect(fontWeightCall()?.args).toEqual(['k', 700]);
    });

    it('case-insensitive keywords still translate', () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: 'BOLD' });
        expect(fontWeightCall()?.args).toEqual(['k', 700]);
    });

    it('unknown keyword falls back to 400 (defensive default)', () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: 'definitely-not-a-weight' });
        expect(fontWeightCall()?.args).toEqual(['k', 400]);
    });

    it('extra-bold numeric (800) flows through unchanged', () => {
        applyChangedProps(makeInstance(), {}, { fontWeight: 800 });
        expect(fontWeightCall()?.args).toEqual(['k', 800]);
    });
});
