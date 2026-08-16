import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { PulpHostConfig } from '../src/host-config.js';

describe('raw SVG viewport materialization', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => bridge.uninstall());

    it('applies the ancestor svg viewBox to the native path, not its container', () => {
        const container = { rootId: 'root', nextId: 0 } as never;
        const svg = PulpHostConfig.createInstance(
            'svg' as never,
            { id: 'peak-icon', width: 22, height: 16, viewBox: '0 0 24 24' } as never,
            container, {} as never, null as never,
        );
        const path = PulpHostConfig.createInstance(
            'path' as never,
            { id: 'peak-path', d: 'M 3 20 L 3 14', stroke: '#fff' } as never,
            container, {} as never, null as never,
        );

        PulpHostConfig.appendInitialChild!(svg, path);
        PulpHostConfig.appendChildToContainer!(container, svg);

        expect(bridge.calls.filter(c => c.fn === 'setSvgViewBox').map(c => c.args))
            .toEqual([['peak-path', 24, 24]]);
    });

    it('propagates through svg groups and preserves a primitive override', () => {
        const container = { rootId: 'root', nextId: 0 } as never;
        const svg = PulpHostConfig.createInstance(
            'svg' as never,
            { id: 'icon', viewBox: '0 0 24 16' } as never,
            container, {} as never, null as never,
        );
        const group = PulpHostConfig.createInstance(
            'g' as never, { id: 'group' } as never,
            container, {} as never, null as never,
        );
        const inheritedPath = PulpHostConfig.createInstance(
            'path' as never, { id: 'inherited', d: 'M0 0' } as never,
            container, {} as never, null as never,
        );
        const explicitPath = PulpHostConfig.createInstance(
            'path' as never,
            { id: 'explicit', d: 'M0 0', viewBox: '0 0 12 12' } as never,
            container, {} as never, null as never,
        );

        PulpHostConfig.appendInitialChild!(group, inheritedPath);
        PulpHostConfig.appendInitialChild!(group, explicitPath);
        PulpHostConfig.appendInitialChild!(svg, group);
        PulpHostConfig.appendChildToContainer!(container, svg);

        expect(bridge.calls.filter(c => c.fn === 'setSvgViewBox').map(c => c.args))
            .toEqual([['inherited', 24, 16], ['explicit', 12, 12]]);
    });

    it('forwards lowercase rect and line geometry without treating it as layout', () => {
        const container = { rootId: 'root', nextId: 0 } as never;
        const svg = PulpHostConfig.createInstance(
            'svg' as never, { id: 'preview', width: 56, height: 22 } as never,
            container, {} as never, null as never,
        );
        const rect = PulpHostConfig.createInstance(
            'rect' as never,
            { id: 'bar', x: 12.5, y: 4, width: 3.5, height: 14 } as never,
            container, {} as never, null as never,
        );
        const line = PulpHostConfig.createInstance(
            'line' as never,
            { id: 'zero', x1: 0, y1: 11, x2: 56, y2: 11 } as never,
            container, {} as never, null as never,
        );

        PulpHostConfig.appendInitialChild!(svg, rect);
        PulpHostConfig.appendInitialChild!(svg, line);
        PulpHostConfig.appendChildToContainer!(container, svg);

        expect(bridge.calls.filter(c => c.fn === 'setSvgRect').map(c => c.args))
            .toEqual([['bar', 12.5, 4, 3.5, 14]]);
        expect(bridge.calls.filter(c => c.fn === 'setSvgLine').map(c => c.args))
            .toEqual([['zero', 0, 11, 56, 11]]);
        expect(bridge.calls.some(c => c.fn === 'setWidth' && c.args[0] === 'bar'))
            .toBe(false);
        expect(bridge.calls.some(c => c.fn === 'setHeight' && c.args[0] === 'bar'))
            .toBe(false);
    });
});
