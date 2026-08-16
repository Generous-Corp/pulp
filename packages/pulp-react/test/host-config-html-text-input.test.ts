import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { PulpHostConfig } from '../src/host-config.js';

describe('HTML text input materialization', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => bridge.uninstall());

    it('forwards placeholder text to the native editor', () => {
        const container = { rootId: 'root', nextId: 0 } as never;
        const input = PulpHostConfig.createInstance(
            'input' as never,
            { id: 'search', type: 'text', value: '', placeholder: 'search…' } as never,
            container, {} as never, null as never,
        );

        PulpHostConfig.appendChildToContainer!(container, input);

        expect(bridge.calls.find(c => c.fn === 'createTextEditor')?.args)
            .toEqual(['search', 'root']);
        expect(bridge.calls.find(c => c.fn === 'setPlaceholder')?.args)
            .toEqual(['search', 'search…']);
    });
});
