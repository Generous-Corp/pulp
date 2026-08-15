// Lowercase HTML buttons imported through Chromium/DesignIR must retain DOM
// styling and captured typography. They therefore lower to a generic native
// box plus a full-box Label child; explicit Pulp <Button> remains TextButton.

import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { PulpHostConfig } from '../src/host-config.js';

describe('HTML button materialization', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => bridge.uninstall());

    function lower(type: string, children: unknown) {
        const container = { rootId: 'root', nextId: 0 } as never;
        const instance = PulpHostConfig.createInstance(
            type as never, { id: 'action', children } as never,
            container, {} as never, null as never,
        );
        PulpHostConfig.appendChildToContainer!(container, instance);
        return instance as unknown as Record<string, unknown>;
    }

    it('keeps an imported lowercase button as a styled box with full-box text', () => {
        const instance = lower('button', 'PRECISION');
        expect(bridge.calls.some(c => c.fn === 'createButton')).toBe(false);
        expect(bridge.calls.find(c => c.fn === 'createRow')?.args)
            .toEqual(['action', 'root']);
        expect(bridge.calls.filter(c => c.fn === 'setFlex').map(c => c.args))
            .toEqual([
                ['action', 'align_items', 'center'],
                ['action', 'justify_content', 'center'],
            ]);
        expect(bridge.calls.find(c => c.fn === 'createLabel')?.args)
            .toEqual(['action__text', 'PRECISION', 'action']);
        expect(bridge.calls.filter(c => c.fn === 'setPosition' ||
            c.fn === 'setTop' || c.fn === 'setRight' ||
            c.fn === 'setBottom' || c.fn === 'setLeft').map(c => c.fn))
            .toEqual(['setPosition', 'setTop', 'setRight', 'setBottom', 'setLeft']);
        expect(bridge.calls.find(c => c.fn === 'setPointerEvents')?.args)
            .toEqual(['action__text', 'none']);
        expect(instance.textTargetId).toBe('action__text');
    });

    it('treats React conditional sentinels plus text as one owning caption', () => {
        const children = [false, null, 'HARMONIC ', undefined, 'SERIES'];
        expect(PulpHostConfig.shouldSetTextContent!('button' as never, {
            children,
        } as never)).toBe(true);

        lower('button', children);
        expect(bridge.calls.filter(c => c.fn === 'createLabel')).toHaveLength(1);
        expect(bridge.calls.find(c => c.fn === 'createLabel')?.args)
            .toEqual(['action__text', 'HARMONIC SERIES', 'action']);
    });

    it('still materializes genuine nested button markup', () => {
        const element = { type: 'span', props: { children: 'icon' } };
        expect(PulpHostConfig.shouldSetTextContent!('button' as never, {
            children: [element, 'FLAT'],
        } as never)).toBe(false);

        lower('button', [element, 'FLAT']);
        expect(bridge.calls.find(c => c.fn === 'createRow')?.args)
            .toEqual(['action', 'root']);
        expect(bridge.calls.filter(c => c.fn === 'setFlex').map(c => c.args))
            .toEqual([
                ['action', 'align_items', 'center'],
                ['action', 'justify_content', 'center'],
            ]);
    });

    it('leaves explicit native Pulp Button on the stock widget path', () => {
        lower('Button', 'PRECISION');
        expect(bridge.calls.find(c => c.fn === 'createButton')?.args)
            .toEqual(['action', 'PRECISION', 'root']);
        expect(bridge.calls.some(c => c.fn === 'createLabel')).toBe(false);
    });
});
