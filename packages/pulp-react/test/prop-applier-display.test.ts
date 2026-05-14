// pulp #1434 (Triage #12) — verify the @pulp/react prop-applier
// dispatches `display: 'flex' | 'none'` to the right setVisible call.
//
// RN exports + Figma / v0.dev / Claude Design HTML routinely emit
// `style={{ display: 'flex' }}` (the implicit default in pulp, but
// the prop-applier shouldn't drop it as unknown) or `display: 'none'`
// to hide a subtree. The yoga / CSS-shim side was wired for the
// el.style proxy in #1422; this test guards the parallel RN-flavored
// JSX path so RN consumers don't have to round-trip through el.style
// (which costs a bridge call + DOM-lite proxy walk).

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
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

function makeInstance(id: string = 'k', type: string = 'View'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

function visibleCalls(b: MockBridge) {
    return b.calls.filter((c) => c.fn === 'setVisible');
}

describe('prop-applier display: flex / none (pulp #1434 Triage #12)', () => {
    it("display: 'none' calls setVisible(id, false)", () => {
        applyChangedProps(makeInstance(), {}, { display: 'none' });
        const calls = visibleCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', false]);
    });

    it("display: 'flex' calls setVisible(id, true)", () => {
        applyChangedProps(makeInstance(), {}, { display: 'flex' });
        const calls = visibleCalls(bridge);
        expect(calls).toHaveLength(1);
        expect(calls[0].args).toEqual(['k', true]);
    });

    it('unknown display values are silently dropped', () => {
        applyChangedProps(makeInstance(), {}, { display: 'block' });
        // Other JSX intrinsics (e.g. block / inline-block / grid) flow
        // through the CSS shim only — for RN consumers, the prop-applier
        // intentionally leaves the View at its current visibility.
        expect(visibleCalls(bridge)).toHaveLength(0);
    });

    it('display change from none to flex re-shows the View', () => {
        const inst = makeInstance();
        // First: hide.
        applyChangedProps(inst, {}, { display: 'none' });
        expect(visibleCalls(bridge)).toEqual([
            expect.objectContaining({ args: ['k', false] }),
        ]);

        // Then: re-show via display: 'flex'. applyChangedProps should
        // fire setVisible(true) because display moved from 'none' to
        // 'flex' and the value differs.
        applyChangedProps(inst, { display: 'none' }, { display: 'flex' });
        const calls = visibleCalls(bridge);
        // 1 from the first call + 1 from the change = 2 total.
        expect(calls).toHaveLength(2);
        expect(calls[1].args).toEqual(['k', true]);
    });
});

// pulp #1894 — `display: 'flex'` without an explicit `flexDirection`
// must default to row (CSS spec), not column (Yoga / RN default).
// Without this fallback, every `style={{ display: 'flex' }}` JSX
// container imported via the flat-prop path collapses to a vertical
// stack — first seen in Spectr's editor toolbar post-#1859.
describe('prop-applier display: flex default direction (pulp #1894)', () => {
    function flexDirectionCalls(b: MockBridge) {
        return b.calls.filter(
            (c) => c.fn === 'setFlex' && c.args[1] === 'direction',
        );
    }

    it("display: 'flex' alone emits setFlex(direction, 'row')", () => {
        applyChangedProps(makeInstance(), {}, { display: 'flex' });
        const dir = flexDirectionCalls(bridge);
        expect(dir).toHaveLength(1);
        expect(dir[0].args).toEqual(['k', 'direction', 'row']);
    });

    it('explicit flexDirection: column suppresses the default', () => {
        applyChangedProps(makeInstance(), {}, {
            display: 'flex',
            flexDirection: 'column',
        });
        const dir = flexDirectionCalls(bridge);
        // Only one direction call — the explicit one. No extra 'row'
        // emitted from the display:flex default path.
        expect(dir).toHaveLength(1);
        expect(dir[0].args).toEqual(['k', 'direction', 'column']);
    });

    it('explicit flexDirection: row matches the default (single emit)', () => {
        applyChangedProps(makeInstance(), {}, {
            display: 'flex',
            flexDirection: 'row',
        });
        const dir = flexDirectionCalls(bridge);
        // Should only emit once — the explicit row, NOT a duplicate
        // from the display:flex default. The hasOwnProperty guard skips
        // the default when flexDirection is in the props bag.
        expect(dir).toHaveLength(1);
        expect(dir[0].args).toEqual(['k', 'direction', 'row']);
    });

    it('kebab-case flex-direction also suppresses the default', () => {
        applyChangedProps(makeInstance(), {}, {
            display: 'flex',
            'flex-direction': 'column',
        });
        const dir = flexDirectionCalls(bridge);
        // The kebab-case key path is what CSS-shim emissions use.
        // The default-suppression must check both spellings.
        const fromDisplayDefault = dir.filter(
            (c) => c.args[2] === 'row',
        );
        expect(fromDisplayDefault).toHaveLength(0);
    });

    it('flexFlow with direction token suppresses the default', () => {
        applyChangedProps(makeInstance(), {}, {
            display: 'flex',
            flexFlow: 'column wrap',
        });
        const fromDisplayDefault = flexDirectionCalls(bridge).filter(
            (c) => c.args[2] === 'row',
        );
        expect(fromDisplayDefault).toHaveLength(0);
    });

    it('flexFlow without direction (just "wrap") does NOT suppress default', () => {
        applyChangedProps(makeInstance(), {}, {
            display: 'flex',
            flexFlow: 'wrap',
        });
        const fromDisplayDefault = flexDirectionCalls(bridge).filter(
            (c) => c.args[2] === 'row',
        );
        // CSS spec: when flex-flow has no direction component, direction
        // still defaults to row — same as bare display:flex.
        expect(fromDisplayDefault).toHaveLength(1);
    });

    it("display: 'none' does not emit the default direction", () => {
        applyChangedProps(makeInstance(), {}, { display: 'none' });
        expect(flexDirectionCalls(bridge)).toHaveLength(0);
    });

    // pulp #1898 (Codex review P2) — the prop-applier also accepts
    // `direction` as a flex-direction alias (see the `case 'direction'`
    // block in src/prop-applier.ts). The default-row suppression must
    // honor that alias too, otherwise an explicit `direction: 'column'`
    // gets clobbered by the default-row emit from display:flex.
    it("explicit direction: 'column' alias suppresses the default", () => {
        applyChangedProps(makeInstance(), {}, {
            display: 'flex',
            direction: 'column',
        });
        const dir = flexDirectionCalls(bridge);
        // Only the explicit column should land — no default row.
        const fromDisplayDefault = dir.filter((c) => c.args[2] === 'row');
        expect(fromDisplayDefault).toHaveLength(0);
        const columnCalls = dir.filter((c) => c.args[2] === 'column');
        expect(columnCalls).toHaveLength(1);
    });

    it("direction: 'row-reverse' alias also suppresses the default", () => {
        applyChangedProps(makeInstance(), {}, {
            display: 'flex',
            direction: 'row-reverse',
        });
        const fromDisplayDefault = flexDirectionCalls(bridge).filter(
            (c) => c.args[2] === 'row',
        );
        // The explicit row-reverse must win — no plain 'row' emit from
        // the display:flex default path.
        expect(fromDisplayDefault).toHaveLength(0);
    });

    it("direction: 'rtl' (writing direction, not flex) does NOT suppress default", () => {
        // `direction: 'rtl'` is the CSS writing-direction sense (routes
        // to setDirection, not setFlex). It should NOT be treated as a
        // flex-direction signal — the row default still fires.
        applyChangedProps(makeInstance(), {}, {
            display: 'flex',
            direction: 'rtl',
        });
        const fromDisplayDefault = flexDirectionCalls(bridge).filter(
            (c) => c.args[2] === 'row',
        );
        expect(fromDisplayDefault).toHaveLength(1);
    });
});
