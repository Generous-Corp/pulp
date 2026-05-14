// pulp #71 — TEXT_BEARING types whose children are mixed string/number
// arrays (e.g. <button>{count}{" bands ▾"}</button>) must lower to a
// single setText() call on commitUpdate when the count changes. Pre-fix,
// asText() returned undefined for arrays, so the setText branch in
// commitUpdate silently dropped every text update on a button whose
// label was composed from an interpolated number plus a literal — the
// Spectr "32 bands" trigger froze on its first-render value no matter
// which preset the user picked.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { PulpHostConfig } from '../src/host-config.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
});

const callsOf = (b: MockBridge, fn: string) =>
    b.calls.filter((c) => c.fn === fn);

function makeButton(id: string, children: unknown): PulpInstance {
    return {
        id,
        type: 'button' as PulpInstance['type'],
        props: { children },
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

describe('host-config commitUpdate — array children on TEXT_BEARING (#71)', () => {
    const commitUpdate = PulpHostConfig.commitUpdate as
        | ((instance: PulpInstance, payload: unknown, type: string,
            oldProps: Record<string, unknown>, newProps: Record<string, unknown>,
            handle: unknown) => void)
        | undefined;

    it('hook exists', () => {
        expect(typeof commitUpdate).toBe('function');
    });

    it('emits setText when [number, string] array changes value', () => {
        // Mirrors Spectr's <button>{bandsCount}{" bands ▾"}</button>.
        const inst = makeButton('bands_trigger', [32, ' bands ▾']);
        commitUpdate!(
            inst, null, 'button',
            { children: [32, ' bands ▾'] },
            { children: [56, ' bands ▾'] },
            null,
        );
        const c = callsOf(bridge, 'setText');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['bands_trigger', '56 bands ▾']);
    });

    it('does not re-emit setText when array value is unchanged', () => {
        const inst = makeButton('bands_trigger', [32, ' bands ▾']);
        commitUpdate!(
            inst, null, 'button',
            { children: [32, ' bands ▾'] },
            { children: [32, ' bands ▾'] },
            null,
        );
        expect(callsOf(bridge, 'setText')).toHaveLength(0);
    });

    it('skips null/undefined/boolean entries (React conditional-render sentinels)', () => {
        // <button>{condition && "(beta) "}{count}{" bands"}</button>
        // — when `condition` is false React passes the literal `false`.
        const inst = makeButton('b1', [false, 32, ' bands']);
        commitUpdate!(
            inst, null, 'button',
            { children: [false, 32, ' bands'] },
            { children: [true && '(beta) ', 56, ' bands'] },
            null,
        );
        const c = callsOf(bridge, 'setText');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['b1', '(beta) 56 bands']);
    });

    it('flattens nested arrays of scalars', () => {
        const inst = makeButton('b2', ['a', 'b']);
        commitUpdate!(
            inst, null, 'button',
            { children: ['a', 'b'] },
            { children: [['x', 'y'], 'z'] },
            null,
        );
        const c = callsOf(bridge, 'setText');
        expect(c).toHaveLength(1);
        expect(c[0].args).toEqual(['b2', 'xyz']);
    });

    it('does NOT setText when an array contains a real element (bails — child path will mount it)', () => {
        // If the children array has a non-scalar entry we cannot flatten
        // to text, asText must return undefined so commitUpdate falls
        // through to the child-instance path that React's reconciler
        // already drives. Returning a partial string here would clobber
        // the inner element's rendered text on the bridge.
        const elementLike = { $$typeof: Symbol.for('react.element'), type: 'em' };
        const inst = makeButton('b3', [32, ' bands']);
        commitUpdate!(
            inst, null, 'button',
            { children: [32, ' bands'] },
            { children: [32, ' ', elementLike] },
            null,
        );
        expect(callsOf(bridge, 'setText')).toHaveLength(0);
    });
});
