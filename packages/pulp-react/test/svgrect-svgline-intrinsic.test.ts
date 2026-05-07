// Test for pulp #1416 — @pulp/react SvgRect + SvgLine intrinsics.
//
// The C++ side ships SvgRectWidget + SvgLineWidget + bridge handlers
// (createSvgRect / setSvgRect / createSvgLine / setSvgLine, plus the
// shared setSvgFill / setSvgStroke / setSvgStrokeWidth setters made
// polymorphic across all three SVG-primitive widget types). This wires
// the React-side intrinsics so JSX
//   <SvgRect x={10} y={20} width={50} height={30} fill="#f00" />
//   <SvgLine x1={0} y1={0} x2={100} y2={100} stroke="#0f0" />
// reach the bridge.
//
// Closes Spectr [G] (preset manager band-shape thumbnails currently
// blank — MiniPreview renders <svg><rect> per band + <line>, dom-adapter
// maps to <View>, and without these intrinsics the geometry props are
// dropped silently).

import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { SvgLine, SvgRect } from '../src/intrinsics.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

describe('@pulp/react SvgRect intrinsic (pulp #1416)', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    it('export is a function component', () => {
        expect(typeof SvgRect).toBe('function');
    });

    it('forwards x/y/width/height to a single setSvgRect call', () => {
        applyAllProps(instance('bar1', 'SvgRect', {
            x: 10, y: 20, width: 50, height: 30,
        }));
        const setRect = bridge.calls.filter((c) => c.fn === 'setSvgRect');
        // Critical: ONE atomic call carrying the full geometry, not four
        // partial updates that would clobber unset axes back to zero.
        expect(setRect.length).toBe(1);
        expect(setRect[0].args).toEqual(['bar1', 10, 20, 50, 30]);
    });

    it('forwards `fill` to setSvgFill', () => {
        applyAllProps(instance('bar2', 'SvgRect', {
            x: 0, y: 0, width: 10, height: 10,
            fill: '#ff0000',
        }));
        const setFill = bridge.calls.filter((c) => c.fn === 'setSvgFill');
        expect(setFill.length).toBe(1);
        expect(setFill[0].args).toEqual(['bar2', '#ff0000']);
    });

    it('forwards `fill="none"` (clears via bridge)', () => {
        applyAllProps(instance('bar3', 'SvgRect', {
            width: 10, height: 10, fill: 'none',
        }));
        const setFill = bridge.calls.filter((c) => c.fn === 'setSvgFill');
        expect(setFill[0].args).toEqual(['bar3', 'none']);
    });

    it('forwards `stroke` and `strokeWidth` together', () => {
        applyAllProps(instance('bar4', 'SvgRect', {
            width: 10, height: 10,
            stroke: '#000000', strokeWidth: 2,
        }));
        const setStroke = bridge.calls.filter((c) => c.fn === 'setSvgStroke');
        const setSW = bridge.calls.filter((c) => c.fn === 'setSvgStrokeWidth');
        expect(setStroke[0].args).toEqual(['bar4', '#000000']);
        expect(setSW[0].args).toEqual(['bar4', 2]);
    });

    it('emits all setters when full prop set is applied', () => {
        applyAllProps(instance('bar5', 'SvgRect', {
            x: 1, y: 2, width: 30, height: 40,
            fill: '#ffffff', stroke: '#000000', strokeWidth: 1.5,
        }));
        const fns = bridge.calls.map((c) => c.fn).filter((n) =>
            ['setSvgRect', 'setSvgFill', 'setSvgStroke', 'setSvgStrokeWidth'].includes(n)
        );
        expect(fns.sort()).toEqual([
            'setSvgFill', 'setSvgRect', 'setSvgStroke', 'setSvgStrokeWidth',
        ]);
        const setRect = bridge.calls.filter((c) => c.fn === 'setSvgRect');
        expect(setRect[0].args).toEqual(['bar5', 1, 2, 30, 40]);
    });

    it('default-fills missing geometry props with 0', () => {
        // Only width/height set — x/y default to 0 (matches SVG <rect>).
        applyAllProps(instance('bar6', 'SvgRect', {
            width: 100, height: 20,
        }));
        const setRect = bridge.calls.filter((c) => c.fn === 'setSvgRect');
        expect(setRect.length).toBe(1);
        expect(setRect[0].args).toEqual(['bar6', 0, 0, 100, 20]);
    });

    it('does NOT route width/height through setFlex for SvgRect', () => {
        // Critical: width/height on a View intrinsic go through Yoga
        // (setFlex), but on SvgRect they're rect-geometry. Type-aware
        // dispatch must keep them out of the flex pipeline.
        applyAllProps(instance('bar7', 'SvgRect', {
            x: 0, y: 0, width: 50, height: 30,
        }));
        const flex = bridge.calls.filter((c) => c.fn === 'setFlex');
        expect(flex.length).toBe(0);
    });

    it('commitUpdate coalesces a geometry change into one setSvgRect', () => {
        applyChangedProps(
            instance('bar8', 'SvgRect', {}),
            { x: 0, y: 0, width: 10, height: 10, fill: '#aaa' },
            { x: 0, y: 0, width: 20, height: 10, fill: '#aaa' },
        );
        const setRect = bridge.calls.filter((c) => c.fn === 'setSvgRect');
        const setFill = bridge.calls.filter((c) => c.fn === 'setSvgFill');
        expect(setRect.length).toBe(1);
        expect(setRect[0].args).toEqual(['bar8', 0, 0, 20, 10]);
        expect(setFill.length).toBe(0);  // unchanged
    });

    it('commitUpdate does not re-emit setSvgRect when only fill changed', () => {
        applyChangedProps(
            instance('bar9', 'SvgRect', {}),
            { x: 0, y: 0, width: 10, height: 10, fill: '#aaa' },
            { x: 0, y: 0, width: 10, height: 10, fill: '#bbb' },
        );
        const setRect = bridge.calls.filter((c) => c.fn === 'setSvgRect');
        const setFill = bridge.calls.filter((c) => c.fn === 'setSvgFill');
        expect(setRect.length).toBe(0);
        expect(setFill.length).toBe(1);
        expect(setFill[0].args).toEqual(['bar9', '#bbb']);
    });
});

describe('@pulp/react SvgLine intrinsic (pulp #1416)', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    it('export is a function component', () => {
        expect(typeof SvgLine).toBe('function');
    });

    it('forwards x1/y1/x2/y2 to a single setSvgLine call', () => {
        applyAllProps(instance('axis1', 'SvgLine', {
            x1: 0, y1: 0, x2: 100, y2: 100,
        }));
        const setLine = bridge.calls.filter((c) => c.fn === 'setSvgLine');
        expect(setLine.length).toBe(1);
        expect(setLine[0].args).toEqual(['axis1', 0, 0, 100, 100]);
    });

    it('forwards `stroke` to setSvgStroke', () => {
        applyAllProps(instance('axis2', 'SvgLine', {
            x1: 0, y1: 0, x2: 50, y2: 0,
            stroke: '#00ff00',
        }));
        const setStroke = bridge.calls.filter((c) => c.fn === 'setSvgStroke');
        expect(setStroke.length).toBe(1);
        expect(setStroke[0].args).toEqual(['axis2', '#00ff00']);
    });

    it('forwards `strokeWidth`', () => {
        applyAllProps(instance('axis3', 'SvgLine', {
            x1: 0, y1: 0, x2: 10, y2: 0,
            strokeWidth: 2.5,
        }));
        const setSW = bridge.calls.filter((c) => c.fn === 'setSvgStrokeWidth');
        expect(setSW[0].args).toEqual(['axis3', 2.5]);
    });

    it('default-fills missing endpoint props with 0', () => {
        applyAllProps(instance('axis4', 'SvgLine', {
            x2: 100,
        }));
        const setLine = bridge.calls.filter((c) => c.fn === 'setSvgLine');
        expect(setLine.length).toBe(1);
        expect(setLine[0].args).toEqual(['axis4', 0, 0, 100, 0]);
    });

    it('does NOT emit setSvgLine when no geometry prop set', () => {
        applyAllProps(instance('axis5', 'SvgLine', {
            stroke: '#000',
        }));
        const setLine = bridge.calls.filter((c) => c.fn === 'setSvgLine');
        expect(setLine.length).toBe(0);
    });

    it('emits all setters when full prop set is applied', () => {
        applyAllProps(instance('axis6', 'SvgLine', {
            x1: 0, y1: 10, x2: 100, y2: 10,
            stroke: '#0f0', strokeWidth: 1,
        }));
        const fns = bridge.calls.map((c) => c.fn).filter((n) =>
            ['setSvgLine', 'setSvgStroke', 'setSvgStrokeWidth'].includes(n)
        );
        expect(fns.sort()).toEqual([
            'setSvgLine', 'setSvgStroke', 'setSvgStrokeWidth',
        ]);
    });

    it('commitUpdate coalesces an endpoint change into one setSvgLine', () => {
        applyChangedProps(
            instance('axis7', 'SvgLine', {}),
            { x1: 0, y1: 0, x2: 10, y2: 0, stroke: '#000' },
            { x1: 0, y1: 0, x2: 20, y2: 0, stroke: '#000' },
        );
        const setLine = bridge.calls.filter((c) => c.fn === 'setSvgLine');
        const setStroke = bridge.calls.filter((c) => c.fn === 'setSvgStroke');
        expect(setLine.length).toBe(1);
        expect(setLine[0].args).toEqual(['axis7', 0, 0, 20, 0]);
        expect(setStroke.length).toBe(0);
    });
});

describe('@pulp/react SvgRect/SvgLine host-config attachment (pulp #1416)', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    // The host config's createInstance does NOT call the bridge — it
    // defers attachment to appendInitialChild / appendChild (the parent
    // isn't known at createInstance time). So we exercise the full
    // create+attach lifecycle here and assert the bridge sees the
    // correct createX call for the new SVG primitives.
    it('appendChildToContainer routes SvgRect through createSvgRect', async () => {
        const hc = await import('../src/host-config.js');
        const container = { rootId: 'root', nextId: 0 } as never;
        const child = hc.PulpHostConfig.createInstance(
            'SvgRect' as never,
            { x: 0, y: 0, width: 10, height: 10 } as never,
            container,
            {} as never,
            null as never,
        );
        // appendChildToContainer attaches under the bridge root (always
        // on-bridge), so it's the trigger that flushes the createX call.
        hc.PulpHostConfig.appendChildToContainer(container, child);
        const created = bridge.calls.filter((c) => c.fn === 'createSvgRect');
        expect(created.length).toBe(1);
    });

    it('appendChildToContainer routes SvgLine through createSvgLine', async () => {
        const hc = await import('../src/host-config.js');
        const container = { rootId: 'root', nextId: 0 } as never;
        const child = hc.PulpHostConfig.createInstance(
            'SvgLine' as never,
            { x1: 0, y1: 0, x2: 10, y2: 10 } as never,
            container,
            {} as never,
            null as never,
        );
        hc.PulpHostConfig.appendChildToContainer(container, child);
        const created = bridge.calls.filter((c) => c.fn === 'createSvgLine');
        expect(created.length).toBe(1);
    });
});
