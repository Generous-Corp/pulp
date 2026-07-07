// Routing-parity coverage for the @pulp/react widget intrinsics.
//
// This sweep verifies routing BREADTH: every widget intrinsic, both in
// its CAPITALIZED canonical form (e.g. `Knob`) AND its lowercase HTML-
// style alias (e.g. `knob`), lowers to the correct native createX bridge
// call — NOT to the generic `createCol` container fallback. A lowercase
// alias that fell through to `createCol` produced a plain container where
// the author expected a live widget (the bug this sweep guards against).
//
// Interaction DEPTH (drag / onChange actually firing, value round-trips)
// is intentionally NOT covered here — that is the job of the C++
// behavioral tests against the real widgets. This file only proves the
// JSX type → native createX wiring is complete and symmetric.

import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';

// One canonical table: widget intrinsic → expected bridge createX call.
// The capitalized form is the source of truth; the lowercase alias must
// route to the identical createX.
const WIDGET_ROUTING: ReadonlyArray<readonly [capitalized: string, expectedCreate: string]> = [
    ['Knob', 'createKnob'],
    ['Fader', 'createFader'],
    ['Toggle', 'createToggle'],
    ['Combo', 'createCombo'],
    ['Checkbox', 'createCheckbox'],
    ['Spectrum', 'createSpectrum'],
    ['Waveform', 'createWaveform'],
    ['Meter', 'createMeter'],
    ['XYPad', 'createXYPad'],
    ['ListBox', 'createListBox'],
    ['VirtualList', 'createVirtualList'],
    ['Icon', 'createIcon'],
    // Ink & Signal design-system widgets.
    ['Badge', 'createBadge'],
    ['Stepper', 'createStepper'],
    ['Pan', 'createPan'],
];

describe('@pulp/react widget routing parity', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    // The host config's createInstance does NOT call the bridge — it
    // defers attachment to appendChildToContainer (the bridge root is
    // always on-bridge, so that append is the trigger that flushes the
    // createX call). Drive the same public lifecycle the SvgRect/SvgLine
    // attachment tests use.
    async function lowerOne(type: string, props: Record<string, unknown> = {}): Promise<MockBridge['calls']> {
        bridge.reset();
        const hc = await import('../src/host-config.js');
        const container = { rootId: 'root', nextId: 0 } as never;
        const child = hc.PulpHostConfig.createInstance(
            type as never,
            props as never,
            container,
            {} as never,
            null as never,
        );
        hc.PulpHostConfig.appendChildToContainer(container, child);
        return bridge.calls;
    }

    for (const [capitalized, expectedCreate] of WIDGET_ROUTING) {
        const lowercase = capitalized.toLowerCase();

        it(`capitalized <${capitalized}> routes to ${expectedCreate}`, async () => {
            const calls = await lowerOne(capitalized);
            const created = calls.filter((c) => c.fn === expectedCreate);
            expect(created.length).toBe(1);
            // Must NOT fall through to the generic container fallback.
            expect(calls.some((c) => c.fn === 'createCol')).toBe(false);
        });

        it(`lowercase <${lowercase}> alias routes to ${expectedCreate}`, async () => {
            const calls = await lowerOne(lowercase);
            const created = calls.filter((c) => c.fn === expectedCreate);
            expect(created.length).toBe(1);
            // The regression this sweep guards: a lowercase widget alias
            // silently lowering to createCol (a plain container).
            expect(calls.some((c) => c.fn === 'createCol')).toBe(false);
        });
    }

    it('passes Icon name through as the native icon type', async () => {
        bridge.reset();
        const hc = await import('../src/host-config.js');
        const container = { rootId: 'root', nextId: 0 } as never;
        const child = hc.PulpHostConfig.createInstance(
            'Icon' as never,
            { name: 'search' } as never,
            container,
            {} as never,
            null as never,
        );

        hc.PulpHostConfig.appendChildToContainer(container, child);

        const created = bridge.calls.find((c) => c.fn === 'createIcon');
        expect(created?.args.slice(1)).toEqual(['search', 'root']);
    });

    it('passes Meter orientation before the parent id', async () => {
        let calls = await lowerOne('Meter');
        let created = calls.find((c) => c.fn === 'createMeter');
        expect(created?.args.slice(1)).toEqual(['vertical', 'root']);

        calls = await lowerOne('meter', { orientation: 'horizontal' });
        created = calls.find((c) => c.fn === 'createMeter');
        expect(created?.args.slice(1)).toEqual(['horizontal', 'root']);
    });
});
