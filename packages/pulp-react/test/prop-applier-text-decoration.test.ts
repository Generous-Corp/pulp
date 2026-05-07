// pulp #1547 — RN textDecoration cluster + textAlignVertical.
// Bridge had setTextDecoration / setTextDecorationColor /
// setTextDecorationStyle registered since #1434; the gap was the
// @pulp/react JSX dispatch. textAlignVertical (Android-only in RN)
// maps to alignItems on the owning View — closest semantic in
// pulp's flex-only model.

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

function callOf(b: MockBridge, fn: string) {
    return b.calls.find((c) => c.fn === fn);
}

describe('rn textDecoration cluster (pulp #1547)', () => {
    it('textDecorationLine forwards to setTextDecoration', () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'underline' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'underline']);
    });

    it('textDecorationLine accepts none (clear)', () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'none' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'none']);
    });

    // Codex post-merge audit (PR #1564, finding 3197006008):
    // The bridge setter only recognizes single tokens (underline /
    // line-through / overline / none) and falls back to none for
    // anything else. The prop-applier normalizes RN's compound multi-
    // line forms by picking the first recognized single token so the
    // value renders something instead of silently clearing the
    // decoration. Tests below pin every RN-spec compound form.
    it("textDecorationLine 'underline line-through' normalizes to 'underline'", () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'underline line-through' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'underline']);
    });

    it("textDecorationLine 'underline overline' normalizes to 'underline'", () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'underline overline' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'underline']);
    });

    it("textDecorationLine 'overline line-through' normalizes to 'overline'", () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'overline line-through' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'overline']);
    });

    it("textDecorationLine 'underline overline line-through' normalizes to 'underline'", () => {
        applyChangedProps(
            makeInstance(),
            {},
            { textDecorationLine: 'underline overline line-through' },
        );
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'underline']);
    });

    it("textDecorationLine unrecognized value falls back to 'none'", () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'wibble' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'none']);
    });

    it('textDecorationColor forwards a hex string', () => {
        applyChangedProps(makeInstance(), {}, { textDecorationColor: '#ff0000' });
        expect(callOf(bridge, 'setTextDecorationColor')?.args).toEqual(['k', '#ff0000']);
    });

    it('textDecorationStyle forwards each spec keyword', () => {
        for (const v of ['solid', 'double', 'dotted', 'dashed', 'wavy'] as const) {
            const b = createMockBridge();
            b.install();
            applyChangedProps(makeInstance(), {}, { textDecorationStyle: v });
            expect(callOf(b, 'setTextDecorationStyle')?.args).toEqual(['k', v]);
            b.uninstall();
        }
    });

    it('all three longhands coexist on the same instance', () => {
        applyChangedProps(makeInstance(), {}, {
            textDecorationLine: 'line-through',
            textDecorationColor: '#00ff00',
            textDecorationStyle: 'wavy',
        });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'line-through']);
        expect(callOf(bridge, 'setTextDecorationColor')?.args).toEqual(['k', '#00ff00']);
        expect(callOf(bridge, 'setTextDecorationStyle')?.args).toEqual(['k', 'wavy']);
    });
});

describe('rn textAlignVertical (pulp #1547)', () => {
    it("'top' maps to alignItems flex-start", () => {
        applyChangedProps(makeInstance(), {}, { textAlignVertical: 'top' });
        expect(callOf(bridge, 'setFlex')?.args).toEqual(['k', 'align_items', 'flex-start']);
    });

    it("'bottom' maps to alignItems flex-end", () => {
        applyChangedProps(makeInstance(), {}, { textAlignVertical: 'bottom' });
        expect(callOf(bridge, 'setFlex')?.args).toEqual(['k', 'align_items', 'flex-end']);
    });

    it("'center' maps to alignItems center", () => {
        applyChangedProps(makeInstance(), {}, { textAlignVertical: 'center' });
        expect(callOf(bridge, 'setFlex')?.args).toEqual(['k', 'align_items', 'center']);
    });

    it("'auto' maps to alignItems auto (Yoga-default inherit)", () => {
        applyChangedProps(makeInstance(), {}, { textAlignVertical: 'auto' });
        expect(callOf(bridge, 'setFlex')?.args).toEqual(['k', 'align_items', 'auto']);
    });
});
