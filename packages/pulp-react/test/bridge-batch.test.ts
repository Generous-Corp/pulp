import { describe, expect, it } from 'vitest';
import { beginBridgeBatch, endBridgeBatch, queueBridgeCall } from '../src/bridge-batch.js';

describe('commit-scoped bridge setter batching', () => {
    it('coalesces repeated writes to one imported widget property', () => {
        const calls: unknown[][] = [];
        const setWidth = (...args: unknown[]) => { calls.push(args); };
        beginBridgeBatch();
        queueBridgeCall('setWidth', setWidth, ['button-1', 20]);
        queueBridgeCall('setWidth', setWidth, ['button-1', 24]);
        endBridgeBatch();
        expect(calls).toEqual([['button-1', 24]]);
    });

    it('keeps distinct setters as a negative control', () => {
        const calls: unknown[][] = [];
        const setWidth = (...args: unknown[]) => { calls.push(['width', ...args]); };
        const setHeight = (...args: unknown[]) => { calls.push(['height', ...args]); };
        beginBridgeBatch();
        queueBridgeCall('setWidth', setWidth, ['button-1', 20]);
        queueBridgeCall('setHeight', setHeight, ['button-1', 30]);
        endBridgeBatch();
        expect(calls).toEqual([
            ['width', 'button-1', 20],
            ['height', 'button-1', 30],
        ]);
    });

    it('does not defer read-like bridge calls as a negative control', () => {
        let observed = 0;
        beginBridgeBatch();
        const read = () => { observed++; };
        const queued = queueBridgeCall('getLayoutBoxMetrics', read, ['button-1']);
        expect(queued).toBe(false);
        expect(observed).toBe(0);
        if (!queued) read();
        endBridgeBatch();
        expect(observed).toBe(1);
    });

    it('does not coalesce unknown or ordered setters', () => {
        const calls: unknown[][] = [];
        beginBridgeBatch();
        const setPreset = (...args: unknown[]) => { calls.push(args); };
        expect(queueBridgeCall('setPreset', setPreset, ['button-1', 'A'])).toBe(false);
        expect(queueBridgeCall('setPreset', setPreset, ['button-1', 'B'])).toBe(false);
        setPreset('button-1', 'A');
        setPreset('button-1', 'B');
        endBridgeBatch();
        expect(calls).toEqual([['button-1', 'A'], ['button-1', 'B']]);
    });

    it('drains later calls before surfacing a flush error', () => {
        const calls: string[] = [];
        beginBridgeBatch();
        queueBridgeCall('setWidth', () => { calls.push('first'); throw new Error('boom'); }, ['a', 1]);
        queueBridgeCall('setHeight', () => { calls.push('second'); }, ['b', 2]);
        expect(() => endBridgeBatch()).toThrow('boom');
        expect(calls).toEqual(['first', 'second']);
    });

    it('reports an undefined flush error after draining', () => {
        const calls: string[] = [];
        beginBridgeBatch();
        queueBridgeCall('setWidth', () => { calls.push('first'); throw undefined; }, ['a', 1]);
        queueBridgeCall('setHeight', () => { calls.push('second'); }, ['b', 2]);
        expect(() => endBridgeBatch()).toThrow();
        expect(calls).toEqual(['first', 'second']);
    });

    it('preserves the first flush error when it is undefined', () => {
        const calls: string[] = [];
        beginBridgeBatch();
        queueBridgeCall('setWidth', () => { calls.push('first'); throw undefined; }, ['a', 1]);
        queueBridgeCall('setHeight', () => { calls.push('second'); throw new Error('later'); }, ['b', 2]);
        let didThrow = false;
        let thrown: unknown;
        try { endBridgeBatch(); } catch (error) { didThrow = true; thrown = error; }
        expect(calls).toEqual(['first', 'second']);
        expect(didThrow).toBe(true);
        expect(thrown).toBeUndefined();
    });

    it('keeps independent flex properties for one widget', () => {
        const calls: unknown[][] = [];
        const setFlex = (...args: unknown[]) => { calls.push(args); };
        beginBridgeBatch();
        queueBridgeCall('setFlex', setFlex, ['button-1', 'paddingLeft', 4]);
        queueBridgeCall('setFlex', setFlex, ['button-1', 'paddingRight', 8]);
        endBridgeBatch();
        expect(calls).toEqual([
            ['button-1', 'paddingLeft', 4],
            ['button-1', 'paddingRight', 8],
        ]);
    });
});
