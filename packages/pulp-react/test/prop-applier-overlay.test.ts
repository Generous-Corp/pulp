// Generalized overlay-click routing.
//
// `<View overlay>` opts a JSX View in as the active click-eligible
// overlay so the platform window host (window_host_mac.mm and siblings)
// short-circuits hit-testing for clicks landing inside the view's
// bounds. Without this, clicks fall through to whatever sibling /
// ancestor pixel sits behind an absolutely-positioned popover.
//
// The prop-applier contract this test pins:
//   - `overlay: true`  → call claimOverlay(id, true)
//   - `overlay: false` → call releaseOverlay(id)
//   - prop removed     → call releaseOverlay(id) on commitUpdate
//   - prop never set   → no claim/release call

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
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

function makeInstance(id: string = 'popover', type: string = 'View'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

describe('@pulp/react prop-applier — overlay routing', () => {
    it('applyAllProps calls claimOverlay when overlay=true', () => {
        applyAllProps({ ...makeInstance('p1'), props: { overlay: true } });
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['p1', true]);
        // releaseOverlay must NOT fire on the mount-claim path.
        expect(bridge.calls.some((c) => c.fn === 'releaseOverlay')).toBe(false);
    });

    it('applyAllProps calls releaseOverlay when overlay=false', () => {
        applyAllProps({ ...makeInstance('p2'), props: { overlay: false } });
        const releases = bridge.calls.filter((c) => c.fn === 'releaseOverlay');
        expect(releases.length).toBe(1);
        expect(releases[0].args).toEqual(['p2']);
        expect(bridge.calls.some((c) => c.fn === 'claimOverlay')).toBe(false);
    });

    it('does not touch overlay APIs when prop is absent', () => {
        applyAllProps({ ...makeInstance('p3'), props: { background: '#000' } });
        expect(bridge.calls.some((c) => c.fn === 'claimOverlay')).toBe(false);
        expect(bridge.calls.some((c) => c.fn === 'releaseOverlay')).toBe(false);
    });

    it('commitUpdate flips overlay false → true via claimOverlay', () => {
        applyChangedProps(
            makeInstance('p4'),
            { overlay: false },
            { overlay: true },
        );
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['p4', true]);
    });

    it('commitUpdate flips overlay true → false via releaseOverlay', () => {
        applyChangedProps(
            makeInstance('p5'),
            { overlay: true },
            { overlay: false },
        );
        const releases = bridge.calls.filter((c) => c.fn === 'releaseOverlay');
        expect(releases.length).toBe(1);
        expect(releases[0].args).toEqual(['p5']);
    });

    it('commitUpdate releases on prop removal (oldProps had overlay=true)', () => {
        // The "prop disappeared" path — equivalent to React unmounting just
        // the overlay attribute without unmounting the whole view.
        applyChangedProps(
            makeInstance('p6'),
            { overlay: true },
            {},
        );
        const releases = bridge.calls.filter((c) => c.fn === 'releaseOverlay');
        expect(releases.length).toBe(1);
        expect(releases[0].args).toEqual(['p6']);
    });

    it('commitUpdate is a no-op when overlay was already truthy and stays truthy', () => {
        applyChangedProps(
            makeInstance('p7'),
            { overlay: true },
            { overlay: true, background: '#fff' },
        );
        // applyChangedProps only re-emits when value !==. Same-true → no
        // additional claim/release; only the background change should fire.
        expect(bridge.calls.some((c) => c.fn === 'claimOverlay')).toBe(false);
        expect(bridge.calls.some((c) => c.fn === 'releaseOverlay')).toBe(false);
    });

    // ────────────────────────────────────────────────────────────────────
    // UX best-practice default: ARIA modal/popup roles auto-claim overlay
    // ────────────────────────────────────────────────────────────────────
    //
    // role="dialog"|"alertdialog"|"menu"|"listbox" and aria-modal="true"
    // semantically describe dismissable overlays. Auto-claim so Esc-
    // dismiss + outside-click routing fire without consumers needing to
    // mirror a position-based heuristic in their own dom-adapter.

    function withProps(id: string, props: Record<string, unknown>): PulpInstance {
        const inst = makeInstance(id);
        inst.props = props;
        return inst;
    }

    it('role="dialog" auto-claims overlay', () => {
        applyAllProps(withProps('m1', { role: 'dialog' }));
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['m1', true]);
    });

    it('role="alertdialog" auto-claims overlay', () => {
        applyAllProps(withProps('m2', { role: 'alertdialog' }));
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['m2', true]);
    });

    it('role="menu" auto-claims overlay (dropdown menu pattern)', () => {
        applyAllProps(withProps('m3', { role: 'menu' }));
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['m3', true]);
    });

    it('role="listbox" auto-claims overlay (combobox/picker pattern)', () => {
        applyAllProps(withProps('m4', { role: 'listbox' }));
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['m4', true]);
    });

    it('role="button" does NOT auto-claim overlay (not a popup role)', () => {
        applyAllProps(withProps('m5', { role: 'button' }));
        expect(bridge.calls.some((c) => c.fn === 'claimOverlay')).toBe(false);
    });

    it('aria-modal="true" auto-claims overlay', () => {
        applyAllProps(withProps('m6', { 'aria-modal': 'true' }));
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['m6', true]);
    });

    it('aria-modal={true} (boolean) auto-claims overlay', () => {
        applyAllProps(withProps('m7', { 'aria-modal': true }));
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        expect(claims.length).toBe(1);
        expect(claims[0].args).toEqual(['m7', true]);
    });

    it('aria-modal="false" does NOT auto-claim', () => {
        applyAllProps(withProps('m8', { 'aria-modal': 'false' }));
        expect(bridge.calls.some((c) => c.fn === 'claimOverlay')).toBe(false);
    });

    it('explicit overlay={false} alongside role still releases (override wins)', () => {
        // role auto-claims, explicit overlay={false} releases.
        // Net: one claim + one release.
        applyAllProps(withProps('m9', { role: 'dialog', overlay: false }));
        expect(bridge.calls.filter((c) => c.fn === 'claimOverlay').length).toBe(1);
        expect(bridge.calls.filter((c) => c.fn === 'releaseOverlay').length).toBe(1);
    });
});

// `aria-haspopup` — the counterpart of the claims above.
//
// The arms above read ARIA to decide which element IS a dismissable overlay,
// and claim it with consume=true so a press outside cannot also operate the
// underlay. `View::overlay_trigger()` is the exception that keeps that rule
// from taxing the press the user meant: a press landing on a marked control
// is delivered to it rather than spent on the dismissal, so switching from one
// dropdown to another costs one press. An app that already described its menus
// for assistive technology has said everything the policy needs; honouring
// only the half that makes presses disappear is what made switching cost two.
describe('@pulp/react prop-applier — aria-haspopup marks an overlay trigger', () => {
    const withProps = (id: string, props: Record<string, unknown>): PulpInstance => ({
        ...makeInstance(id),
        props,
    });

    it('aria-haspopup="menu" marks the control as a trigger', () => {
        applyAllProps(withProps('t1', { 'aria-haspopup': 'menu' }));
        const marks = bridge.calls.filter((c) => c.fn === 'setOverlayTrigger');
        expect(marks.length).toBe(1);
        expect(marks[0].args).toEqual(['t1', true]);
    });

    it.each(['listbox', 'dialog', 'tree', 'grid', 'true'])(
        'aria-haspopup="%s" marks too (the whole ARIA token set)', (token) => {
            applyAllProps(withProps('t2', { 'aria-haspopup': token }));
            const marks = bridge.calls.filter((c) => c.fn === 'setOverlayTrigger');
            expect(marks.length).toBe(1);
            expect(marks[0].args).toEqual(['t2', true]);
        });

    it('aria-haspopup={true} (boolean) marks', () => {
        applyAllProps(withProps('t3', { 'aria-haspopup': true }));
        expect(bridge.calls.filter((c) => c.fn === 'setOverlayTrigger')[0].args)
            .toEqual(['t3', true]);
    });

    it('aria-haspopup="false" UNMARKS rather than marking', () => {
        applyAllProps(withProps('t4', { 'aria-haspopup': 'false' }));
        const marks = bridge.calls.filter((c) => c.fn === 'setOverlayTrigger');
        expect(marks.length).toBe(1);
        expect(marks[0].args).toEqual(['t4', false]);
    });

    it('does not touch the trigger API when the prop is absent', () => {
        // Negative control in the direction that matters: the pass-through is
        // scoped to triggers. If ordinary content were marked, closing a menu
        // would also operate whatever sits under the click.
        applyAllProps(withProps('t5', { background: '#000', role: 'button' }));
        expect(bridge.calls.some((c) => c.fn === 'setOverlayTrigger')).toBe(false);
    });

    it('a popover is claimed, not marked; its trigger is marked, not claimed', () => {
        applyAllProps(withProps('menu', { role: 'menu' }));
        applyAllProps(withProps('button', { 'aria-haspopup': 'menu' }));
        const claims = bridge.calls.filter((c) => c.fn === 'claimOverlay');
        const marks = bridge.calls.filter((c) => c.fn === 'setOverlayTrigger');
        expect(claims.map((c) => c.args)).toEqual([['menu', true]]);
        expect(marks.map((c) => c.args)).toEqual([['button', true]]);
    });

    it('overlayTrigger={true} is the explicit opt-in for non-ARIA documents', () => {
        applyAllProps(withProps('t6', { overlayTrigger: true }));
        expect(bridge.calls.filter((c) => c.fn === 'setOverlayTrigger')[0].args)
            .toEqual(['t6', true]);
    });

    it('overlayTrigger={false} unmarks', () => {
        applyAllProps(withProps('t7', { overlayTrigger: false }));
        expect(bridge.calls.filter((c) => c.fn === 'setOverlayTrigger')[0].args)
            .toEqual(['t7', false]);
    });
});

describe('@pulp/react prop-applier — a lifted submenu declares its parent', () => {
    // `overlay` / `role="menu"` nest a claim only when the claiming view
    // DESCENDS from the open overlay. A submenu placed to escape its menu's box
    // is lifted out of that menu's subtree, so it is a sibling: the native
    // parent-chain test reads it as a rival and dismisses the menu underneath
    // together with the submenu's own rows. `overlayParent` names the overlay
    // the claim belongs to, which is the fact the tree does not carry.

    function makeInstance(id: string): PulpInstance {
        return {
            id,
            type: 'View' as PulpInstance['type'],
            props: {},
            childIds: [],
            onBridge: true,
            pendingChildren: [],
        };
    }
    function withProps(id: string, props: Record<string, unknown>): PulpInstance {
        const inst = makeInstance(id);
        inst.props = props;
        return inst;
    }
    function claimArgs(): unknown[][] {
        return bridge.calls.filter((c) => c.fn === 'claimOverlay').map((c) => c.args);
    }

    it('overlay={true} carries the declared parent', () => {
        applyAllProps(withProps('sub', { overlay: true, overlayParent: 'menu' }));
        expect(claimArgs()).toEqual([['sub', true, 'menu']]);
    });

    it('role="menu" carries the declared parent', () => {
        applyAllProps(withProps('sub', { role: 'menu', overlayParent: 'menu' }));
        expect(claimArgs()).toEqual([['sub', true, 'menu']]);
    });

    it('aria-modal carries the declared parent', () => {
        applyAllProps(withProps('sub', { 'aria-modal': 'true', overlayParent: 'menu' }));
        expect(claimArgs()).toEqual([['sub', true, 'menu']]);
    });

    it('the declaration survives JSX key order', () => {
        // The reason it is read from the whole prop bag rather than from one
        // key: props are applied in insertion order, so a declaration written
        // after `role` would otherwise reach the bridge after the claim it was
        // supposed to qualify — and the menu would already be dismissed.
        applyAllProps(withProps('a', { overlayParent: 'menu', role: 'menu' }));
        applyAllProps(withProps('b', { role: 'menu', overlayParent: 'menu' }));
        expect(claimArgs()).toEqual([['a', true, 'menu'], ['b', true, 'menu']]);
    });

    it('an undeclared claim still emits exactly claimOverlay(id, true)', () => {
        // The control. If the parent argument were always appended, every
        // existing consumer's claim would change shape, and a claim carrying an
        // empty name would be indistinguishable from one carrying a real one.
        applyAllProps(withProps('plain', { role: 'menu' }));
        applyAllProps(withProps('plain2', { overlay: true, overlayParent: '' }));
        applyAllProps(withProps('plain3', { overlay: true, overlayParent: 42 }));
        expect(claimArgs()).toEqual([['plain', true], ['plain2', true], ['plain3', true]]);
    });

    it('overlayParent alone claims nothing', () => {
        // A declaration is not itself a claim: a view that is not an overlay
        // naming one is nothing at all, and must not start routing clicks.
        applyAllProps(withProps('notanoverlay', { overlayParent: 'menu' }));
        expect(claimArgs()).toEqual([]);
        expect(bridge.calls.some((c) => c.fn === 'releaseOverlay')).toBe(false);
    });

    it('re-pointing overlayParent on an update re-claims with the new parent', () => {
        // commitUpdate walks only the keys whose VALUES moved. An author who
        // re-points a submenu at a different menu may change nothing else, so
        // without this arm the native side would keep the old relationship.
        const inst = withProps('sub', { role: 'menu', overlayParent: 'menu-a' });
        applyChangedProps(
            inst,
            { role: 'menu', overlayParent: 'menu-a' },
            { role: 'menu', overlayParent: 'menu-b' },
        );
        expect(claimArgs()).toEqual([['sub', true, 'menu-b']]);
    });
});
