import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { call, callRequired } from '../src/prop-applier-internal.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';

let bridge: MockBridge;
beforeEach(() => { bridge = createMockBridge(); bridge.install(); });
afterEach(() => { bridge.uninstall(); });

describe('bridge verb absence', () => {
    // The asymmetry this pair documents: `call` degrades in silence so that
    // capability-gated groups can be withheld, which means a verb missing for
    // any OTHER reason also vanishes without a trace. `callRequired` is the
    // opt-out for verbs whose absence is a bug.
    it('call stays silent for a verb the host does not install', () => {
        expect(() => call('thisVerbDoesNotExist', 'id', 1)).not.toThrow();
    });

    it('callRequired names the missing verb instead', () => {
        expect(() => callRequired('thisVerbDoesNotExist', 'id', 1))
            .toThrowError(/thisVerbDoesNotExist/);
    });

    it('callRequired forwards normally when the verb is installed', () => {
        callRequired('scrollTo', 's', 0, 42);
        const calls = bridge.calls.filter((c) => c.fn === 'scrollTo');
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['s', 0, 42]);
    });
});
