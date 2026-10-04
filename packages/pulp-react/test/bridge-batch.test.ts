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
});
