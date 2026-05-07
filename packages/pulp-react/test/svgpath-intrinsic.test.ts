// Test for pulp #994 — @pulp/react SvgPath intrinsic.
//
// The C++ side has shipped SvgPathWidget + bridge handlers (createSvgPath,
// setSvgPath, setSvgViewBox, setSvgFill, setSvgStroke, setSvgStrokeWidth)
// since v0.61.0 (#965/#991). This wires the React-side intrinsic so JSX
// `<SvgPath d="..." viewBox={[w,h]} fill="#fff" stroke="#000" strokeWidth={1} />`
// reaches the bridge.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import { SvgPath } from '../src/intrinsics.js';
import type { PulpInstance } from '../src/types.js';

function instance(id: string, type: string, props: Record<string, unknown>): PulpInstance {
    return { id, type, props } as PulpInstance;
}

describe('@pulp/react SvgPath intrinsic (pulp #994)', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => {
        bridge.uninstall();
    });

    it('export is a function component', () => {
        expect(typeof SvgPath).toBe('function');
    });

    it('forwards `d` prop to setSvgPath', () => {
        applyAllProps(instance('icon1', 'SvgPath', {
            d: 'M0 0 L10 10 Z',
        }));
        const setPath = bridge.calls.filter((c) => c.fn === 'setSvgPath');
        expect(setPath.length).toBe(1);
        expect(setPath[0].args).toEqual(['icon1', 'M0 0 L10 10 Z']);
    });

    it('forwards `viewBox` array to setSvgViewBox(id, w, h)', () => {
        applyAllProps(instance('icon2', 'SvgPath', {
            viewBox: [24, 24],
        }));
        const setVB = bridge.calls.filter((c) => c.fn === 'setSvgViewBox');
        expect(setVB.length).toBe(1);
        expect(setVB[0].args).toEqual(['icon2', 24, 24]);
    });

    it('drops viewBox when not a length-2 array', () => {
        applyAllProps(instance('icon3', 'SvgPath', {
            viewBox: 'not-an-array' as unknown as [number, number],
        }));
        expect(bridge.calls.filter((c) => c.fn === 'setSvgViewBox').length).toBe(0);
    });

    it('forwards `fill` to setSvgFill', () => {
        applyAllProps(instance('icon4', 'SvgPath', {
            fill: '#ff8800',
        }));
        const setFill = bridge.calls.filter((c) => c.fn === 'setSvgFill');
        expect(setFill.length).toBe(1);
        expect(setFill[0].args).toEqual(['icon4', '#ff8800']);
    });

    it('forwards `fill="none"` to setSvgFill (clears via bridge)', () => {
        applyAllProps(instance('icon5', 'SvgPath', {
            fill: 'none',
        }));
        const setFill = bridge.calls.filter((c) => c.fn === 'setSvgFill');
        expect(setFill[0].args).toEqual(['icon5', 'none']);
    });

    it('forwards `stroke` and `strokeWidth` together', () => {
        applyAllProps(instance('icon6', 'SvgPath', {
            stroke: '#000000',
            strokeWidth: 1.5,
        }));
        const setStroke = bridge.calls.filter((c) => c.fn === 'setSvgStroke');
        const setSW = bridge.calls.filter((c) => c.fn === 'setSvgStrokeWidth');
        expect(setStroke[0].args).toEqual(['icon6', '#000000']);
        expect(setSW[0].args).toEqual(['icon6', 1.5]);
    });

    it('emits all 5 setters when full prop set is applied', () => {
        applyAllProps(instance('icon7', 'SvgPath', {
            d: 'M5 5 L15 15',
            viewBox: [16, 16],
            fill: '#ffffff',
            stroke: '#000000',
            strokeWidth: 2,
        }));
        const fns = bridge.calls.map((c) => c.fn).filter((n) =>
            ['setSvgPath', 'setSvgViewBox', 'setSvgFill', 'setSvgStroke', 'setSvgStrokeWidth'].includes(n)
        );
        expect(fns.sort()).toEqual([
            'setSvgFill', 'setSvgPath', 'setSvgStroke', 'setSvgStrokeWidth', 'setSvgViewBox',
        ]);
    });

    it('commitUpdate replaces only changed props', () => {
        applyChangedProps(
            instance('icon8', 'SvgPath', {}),
            { d: 'M0 0 L1 1', fill: '#aaa' },
            { d: 'M0 0 L1 1', fill: '#bbb' },
        );
        const setPath = bridge.calls.filter((c) => c.fn === 'setSvgPath');
        const setFill = bridge.calls.filter((c) => c.fn === 'setSvgFill');
        expect(setPath.length).toBe(0);  // unchanged
        expect(setFill.length).toBe(1);
        expect(setFill[0].args).toEqual(['icon8', '#bbb']);
    });
});
