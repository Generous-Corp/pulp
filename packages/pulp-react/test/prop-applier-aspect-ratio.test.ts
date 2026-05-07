// pulp #1434 batch 5 — verify the @pulp/react prop-applier routes
// `aspectRatio` through `setFlex(id, "aspect_ratio", value)`. Mirrors RN's
// flex-prop surface so design-tool exports (Figma fixed-ratio frames,
// v0/Claude Design hero images, RN image cards) reach the bridge.

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

describe('prop-applier aspectRatio (pulp #1434)', () => {
    it('aspectRatio number routes through setFlex with snake-case key', () => {
        applyChangedProps(makeInstance(), {}, { aspectRatio: 1.5 });
        const call = bridge.calls.find(
            (c) => c.fn === 'setFlex' && c.args[1] === 'aspect_ratio'
        );
        expect(call).toBeDefined();
        expect(call?.args).toEqual(['k', 'aspect_ratio', 1.5]);
    });

    it('aspectRatio 16/9 ratio (computed by caller) flows through unchanged', () => {
        const ratio = 16 / 9;
        applyChangedProps(makeInstance(), {}, { aspectRatio: ratio });
        const call = bridge.calls.find(
            (c) => c.fn === 'setFlex' && c.args[1] === 'aspect_ratio'
        );
        expect(call).toBeDefined();
        expect(call?.args[2]).toBeCloseTo(ratio, 5);
    });

    it('aspectRatio 0 routes through (bridge interprets as clear)', () => {
        applyChangedProps(makeInstance(), {}, { aspectRatio: 0 });
        const call = bridge.calls.find(
            (c) => c.fn === 'setFlex' && c.args[1] === 'aspect_ratio'
        );
        expect(call).toBeDefined();
        expect(call?.args[2]).toBe(0);
    });

    it('aspectRatio undefined is a no-op (no setFlex call)', () => {
        applyChangedProps(makeInstance(), {}, { aspectRatio: undefined });
        const call = bridge.calls.find(
            (c) => c.fn === 'setFlex' && c.args[1] === 'aspect_ratio'
        );
        expect(call).toBeUndefined();
    });

    it('does not collide with width / height routing', () => {
        applyChangedProps(makeInstance(), {}, {
            width: 100,
            aspectRatio: 1.5,
        });
        const calls = bridge.calls.filter((c) => c.fn === 'setFlex');
        const widthCall = calls.find((c) => c.args[1] === 'width');
        const arCall = calls.find((c) => c.args[1] === 'aspect_ratio');
        expect(widthCall?.args).toEqual(['k', 'width', 100]);
        expect(arCall?.args).toEqual(['k', 'aspect_ratio', 1.5]);
    });
});
