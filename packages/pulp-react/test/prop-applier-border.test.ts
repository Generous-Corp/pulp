// pulp #1027 (audit PR #1166 finding #4) — verify the prop-applier routes
// borderColor / borderWidth / borderRadius (and per-side flat props) to the
// per-attribute bridge setters that preserve unset siblings, not the unified
// setBorder(id, color, width, radius) that clobbers everything.

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

function makeInstance(id: string = 'k', type: string = 'View'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

describe('prop-applier border setters route to per-attribute bridge fns', () => {
    it('borderColor calls setBorderColor (not setBorder)', () => {
        applyChangedProps(makeInstance(), {}, { borderColor: '#ff0000' });
        const names = bridge.calls.map((c) => c.fn);
        expect(names).toContain('setBorderColor');
        expect(names).not.toContain('setBorder');
        const call = bridge.calls.find((c) => c.fn === 'setBorderColor');
        expect(call?.args).toEqual(['k', '#ff0000']);
    });

    it('borderWidth calls setBorderWidth (not setBorder)', () => {
        applyChangedProps(makeInstance(), {}, { borderWidth: 3 });
        const names = bridge.calls.map((c) => c.fn);
        expect(names).toContain('setBorderWidth');
        expect(names).not.toContain('setBorder');
        const call = bridge.calls.find((c) => c.fn === 'setBorderWidth');
        expect(call?.args).toEqual(['k', 3]);
    });

    it('borderRadius calls setBorderRadius (not setBorder)', () => {
        applyChangedProps(makeInstance(), {}, { borderRadius: 8 });
        const names = bridge.calls.map((c) => c.fn);
        expect(names).toContain('setBorderRadius');
        expect(names).not.toContain('setBorder');
        const call = bridge.calls.find((c) => c.fn === 'setBorderRadius');
        expect(call?.args).toEqual(['k', 8]);
    });

    it('audit failing case: setting only borderRadius then only borderColor never emits setBorder', () => {
        // First commit: just borderRadius.
        applyChangedProps(makeInstance(), {}, { borderRadius: 8 });
        // Second commit on top of first: add borderColor while radius stays unchanged.
        applyChangedProps(makeInstance(), { borderRadius: 8 }, { borderRadius: 8, borderColor: 'red' });
        const names = bridge.calls.map((c) => c.fn);
        // The unified setBorder must NEVER be invoked from these flat props,
        // because it would clobber the other slots.
        expect(names).not.toContain('setBorder');
        expect(names).toContain('setBorderRadius');
        expect(names).toContain('setBorderColor');
    });

    it('per-side flat props route to per-side setters', () => {
        applyChangedProps(makeInstance(), {}, {
            borderTopColor: '#101010',
            borderRightWidth: 2,
            borderBottomLeftRadius: 5,
        });
        const names = bridge.calls.map((c) => c.fn);
        expect(names).toContain('setBorderTopColor');
        expect(names).toContain('setBorderRightWidth');
        expect(names).toContain('setBorderBottomLeftRadius');
        expect(names).not.toContain('setBorderSide');
        expect(names).not.toContain('setBorder');
    });

    it('object-shape `border` prop still routes to unified setBorder', () => {
        // The shorthand object form sets all three slots intentionally and
        // should keep emitting the unified setter for parity with how
        // CSSStyleDeclaration's `border:` shorthand still atomically sets
        // width + color (radius is preserved by the JS shim, not here).
        applyChangedProps(makeInstance(), {}, {
            border: { color: '#222', width: 1, radius: 4 },
        });
        const names = bridge.calls.map((c) => c.fn);
        expect(names).toContain('setBorder');
    });
});
