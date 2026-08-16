// Lowercase HTML range inputs are browser-shaped RangeSliders, not Pulp's
// opinionated audio Fader.  Their native scalar and accent contract must reach
// the bridge so Chromium-authored controls survive materialization.

import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { PulpHostConfig } from '../src/host-config.js';

describe('HTML range materialization', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => bridge.uninstall());

    it('uses RangeSlider and forwards the authored range and accent', () => {
        const container = { rootId: 'root', nextId: 0 } as never;
        const instance = PulpHostConfig.createInstance(
            'input' as never,
            {
                id: 'amount', type: 'range', min: 0, max: 2, step: 0.1,
                value: 1, disabled: true,
                style: { accentColor: '#38bdf8' },
            } as never,
            container, {} as never, null as never,
        );
        PulpHostConfig.appendChildToContainer!(container, instance);

        expect(bridge.calls.find(c => c.fn === 'createRangeSlider')?.args)
            .toEqual(['amount', 'root']);
        expect(bridge.calls.some(c => c.fn === 'createFader')).toBe(false);
        expect(bridge.calls.find(c => c.fn === 'setMin')?.args)
            .toEqual(['amount', 0]);
        expect(bridge.calls.find(c => c.fn === 'setMax')?.args)
            .toEqual(['amount', 2]);
        expect(bridge.calls.find(c => c.fn === 'setStep')?.args)
            .toEqual(['amount', 0.1]);
        expect(bridge.calls.find(c => c.fn === 'setValue')?.args)
            .toEqual(['amount', 1]);
        expect(bridge.calls.find(c => c.fn === 'setAccentColor')?.args)
            .toEqual(['amount', '#38bdf8']);
        expect(bridge.calls.find(c => c.fn === 'setEnabled')?.args)
            .toEqual(['amount', false]);
    });

    it('re-enables a range when disabled changes back to false', () => {
        const container = { rootId: 'root', nextId: 0 } as never;
        const instance = PulpHostConfig.createInstance(
            'input' as never,
            { id: 'amount', type: 'range', value: 0.5, disabled: true } as never,
            container, {} as never, null as never,
        );
        PulpHostConfig.appendChildToContainer!(container, instance);
        bridge.calls.length = 0;

        PulpHostConfig.commitUpdate!(
            instance,
            PulpHostConfig.prepareUpdate!(
                instance, 'input' as never,
                { id: 'amount', type: 'range', value: 0.5, disabled: true } as never,
                { id: 'amount', type: 'range', value: 0.5, disabled: false } as never,
                {} as never,
            )!,
            'input' as never,
            { id: 'amount', type: 'range', value: 0.5, disabled: true } as never,
            { id: 'amount', type: 'range', value: 0.5, disabled: false } as never,
            {} as never,
        );

        expect(bridge.calls.find(c => c.fn === 'setEnabled')?.args)
            .toEqual(['amount', true]);
    });

    it('resolves captured CSS variables before forwarding the accent', () => {
        (globalThis as Record<string, unknown>).__pulpCssVars = {
            accent: '#42d3ff',
        };
        try {
            const container = { rootId: 'root', nextId: 0 } as never;
            const instance = PulpHostConfig.createInstance(
                'input' as never,
                {
                    id: 'amount', type: 'range', value: 1,
                    style: { accentColor: 'var(--accent)' },
                } as never,
                container, {} as never, null as never,
            );
            PulpHostConfig.appendChildToContainer!(container, instance);

            expect(bridge.calls.find(c => c.fn === 'setAccentColor')?.args)
                .toEqual(['amount', '#42d3ff']);
        } finally {
            delete (globalThis as Record<string, unknown>).__pulpCssVars;
        }
    });
});
