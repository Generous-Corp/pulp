// Renderer-neutral import metadata must follow every React commit. This keeps
// captured Chromium line boxes attached after dynamic text/menu/modal changes
// without making the ordinary @pulp/react runtime depend on the importer.

import { afterEach, describe, expect, it } from 'vitest';
import { PulpHostConfig } from '../src/host-config.js';

const resetAfterCommit = PulpHostConfig.resetAfterCommit as
    ((container: unknown) => void) | undefined;

afterEach(() => {
    const host = globalThis as unknown as Record<string, unknown>;
    delete host.__pulpApplyMaterializedImportMetadata__;
    delete host.__pulpRefreshMaterializedState__;
    delete host.getRootSize;
    delete host.layout;
});

// Drive the real host-config mutation methods rather than reaching for a
// test-only reset. The gate's whole correctness claim is about WHICH methods
// mark the tree dirty, so a test that set the flag directly would assert
// nothing about the thing that can actually break.
const instance = (id: string) => ({
    id, type: 'view', props: {}, childIds: [] as string[],
    onBridge: true, pendingChildren: [] as unknown[],
}) as never;

function mutate(): void {
    const commitUpdate = PulpHostConfig.commitUpdate as
        (...args: unknown[]) => void;
    // A width change is geometric, so it always marks the tree dirty. Do not
    // swap this for a paint-only prop: those are deliberately suppressed.
    commitUpdate(instance('a'), null, 'view', { width: 10 }, { width: 20 }, null);
}

function updateText(oldProps: Record<string, unknown>,
                    newProps: Record<string, unknown>): void {
    const commitUpdate = PulpHostConfig.commitUpdate as
        (...args: unknown[]) => void;
    commitUpdate(instance('status'), null, 'span', oldProps, newProps, null);
}

function updateTypedText(type: string,
                         oldProps: Record<string, unknown>,
                         newProps: Record<string, unknown>): void {
    const commitUpdate = PulpHostConfig.commitUpdate as
        (...args: unknown[]) => void;
    commitUpdate(instance('status'), null, type, oldProps, newProps, null);
}

function reorderSiblings(): void {
    const parent = {
        id: 'p', type: 'view', props: {}, childIds: ['x', 'y'],
        onBridge: true, pendingChildren: [],
    } as never;
    const child = {
        id: 'y', type: 'view', props: {}, childIds: [],
        onBridge: true, parentId: 'p', pendingChildren: [],
    } as never;
    const before = {
        id: 'x', type: 'view', props: {}, childIds: [],
        onBridge: true, parentId: 'p', pendingChildren: [],
    } as never;
    const insertBefore = PulpHostConfig.insertBefore as
        (parent: unknown, child: unknown, before: unknown) => void;
    insertBefore(parent, child, before);
}

describe('host-config materialized metadata', () => {
    it('re-applies captured metadata after a commit that mutated the host tree', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        mutate();
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // The negative half, and the reason the gate exists. Without it this is 3,
    // and each of those applications costs the importer roughly two full root
    // layout passes per layout binding.
    it('does not re-apply after a commit that mutated no host node', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        let layouts = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;
        host.layout = () => ++layouts;

        mutate();
        resetAfterCommit?.({});   // structural commit: applies
        resetAfterCommit?.({});   // state-only commit: must not
        resetAfterCommit?.({});   // still nothing mutated

        expect(applications).toBe(1);
        expect(layouts).toBe(1);
    });

    it('does not re-apply metadata for fixed single-line text-only updates', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        let layouts = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;
        host.layout = () => ++layouts;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'BAND 1/64', width: '100%', height: '100%', whiteSpace: 'nowrap' },
            { children: 'BAND 2/64', width: '100%', height: '100%', whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(1);
        expect(layouts).toBe(1);
    });

    // A nowrap label with a fixed line height is pinned vertically just as
    // firmly as one with an explicit height: nowrap makes the native Label
    // single-line, and a positive line height is returned verbatim without
    // consulting the shaper. Demanding an explicit `height` here is what made
    // a one-word status readout re-apply the whole captured document on every
    // pointer move.
    it('does not re-apply for fixed line-height text-only updates', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'BAND 1/64', width: '100%',
              lineHeight: '14px', whiteSpace: 'nowrap' },
            { children: 'BAND 2/64', width: '100%',
              lineHeight: '14px', whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(1);
    });

    // A unitless line height is a font-size multiplier. It is still
    // text-independent, because the gate separately requires every non-text
    // prop -- fontSize included -- to be unchanged.
    it('accepts a unitless line-height multiplier', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'a', width: 240, fontSize: 10,
              lineHeight: 1.4, whiteSpace: 'nowrap' },
            { children: 'b', width: 240, fontSize: 10,
              lineHeight: 1.4, whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(1);
    });

    // Without nowrap the label is multi-line, so longer copy adds lines and
    // grows the box. The vertical pin is not sufficient on its own.
    it('re-applies when a fixed line height wraps', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'a', width: 240, lineHeight: '14px' },
            { children: 'a much longer string', width: 240, lineHeight: '14px' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // A free-width nowrap label grows horizontally with its text, so the
    // captured geometry really is stale and must be re-applied.
    it('re-applies when a fixed line height has no fixed width', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'a', lineHeight: '14px', whiteSpace: 'nowrap' },
            { children: 'bb', lineHeight: '14px', whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // A percentage line height is rejected deliberately. The typography
    // applier parses '50%' as 50px, and this gate must not depend on that.
    it('rejects a percentage line height', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'a', width: 240, lineHeight: '120%', whiteSpace: 'nowrap' },
            { children: 'b', width: 240, lineHeight: '120%', whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // button does not become a native Label: its props land on the owning
    // Row/Panel/TextEditor while the copy goes to a separate caption child, so
    // setLineHeight never reaches the Label that actually holds the glyphs.
    it('re-applies for a delegating button whose text lives in a child label', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateTypedText('button',
            { children: 'a', width: 240, lineHeight: '14px', whiteSpace: 'nowrap' },
            { children: 'b', width: 240, lineHeight: '14px', whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // Button does not become a native Label: its props land on the owning
    // Row/Panel/TextEditor while the copy goes to a separate caption child, so
    // setLineHeight never reaches the Label that actually holds the glyphs.
    it('re-applies for a delegating Button whose text lives in a child label', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateTypedText('Button',
            { children: 'a', width: 240, lineHeight: '14px', whiteSpace: 'nowrap' },
            { children: 'b', width: 240, lineHeight: '14px', whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // TextEditor does not become a native Label: its props land on the owning
    // Row/Panel/TextEditor while the copy goes to a separate caption child, so
    // setLineHeight never reaches the Label that actually holds the glyphs.
    it('re-applies for a delegating TextEditor whose text lives in a child label', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateTypedText('TextEditor',
            { children: 'a', width: 240, lineHeight: '14px', whiteSpace: 'nowrap' },
            { children: 'b', width: 240, lineHeight: '14px', whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // A positive clamp puts the Label back into multi-line mode, where the
    // measured height is lineHeight x min(shaped lines, clamp) -- text-dependent.
    it('re-applies when a line clamp restores multi-line measurement', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'a', width: 240, lineHeight: '14px',
              lineClamp: 2, whiteSpace: 'nowrap' },
            { children: 'b', width: 240, lineHeight: '14px',
              lineClamp: 2, whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // line_height_ <= 0 leaves the shaper in charge of the height.
    it('rejects a zero line height', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'a', width: 240, lineHeight: 0, whiteSpace: 'nowrap' },
            { children: 'b', width: 240, lineHeight: 0, whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // 'normal' is the font's own metric, resolved per string by the shaper.
    it('rejects a normal line height', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'a', width: 240, lineHeight: 'normal', whiteSpace: 'nowrap' },
            { children: 'b', width: 240, lineHeight: 'normal', whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // Neither vertical pin present: the box is free to grow.
    it('re-applies when nothing pins the height', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'a', width: 240, whiteSpace: 'nowrap' },
            { children: 'b', width: 240, whiteSpace: 'nowrap' },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    it('re-applies metadata when changed text can affect geometry', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        let layouts = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;
        host.layout = () => ++layouts;

        mutate();
        resetAfterCommit?.({});
        updateText({ children: 'short' }, { children: 'a longer intrinsic label' });
        resetAfterCommit?.({});

        expect(applications).toBe(2);
        expect(layouts).toBe(2);
    });

    it('re-applies metadata when fixed text changes with another host prop', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        updateText(
            { children: 'BAND 1/64', width: 240, height: 26,
              whiteSpace: 'nowrap', opacity: 0.5 },
            { children: 'BAND 2/64', width: 240, height: 26,
              whiteSpace: 'nowrap', opacity: 1 },
        );
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // insertBefore's same-parent branch reorders childIds and returns WITHOUT
    // reaching attach(), so a mark placed in the attach helper rather than on
    // the method silently misses sibling reordering. That is how React swaps a
    // list row for an inline editor, and skipping the re-apply leaves the
    // replacement without its captured geometry -- invisible from the API, the
    // return values and the rendered tree alike.
    it('re-applies after a same-parent sibling reorder', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});
        expect(applications).toBe(1);

        reorderSiblings();
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    // A host resize never arrives as a React commit, but it moves every metric
    // the captured insets are derived from. This is the case a mutation-only
    // gate fails.
    it('re-applies when the root box changed without a commit', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;
        let size = { width: 400, height: 300 };
        host.getRootSize = () => size;

        mutate();
        resetAfterCommit?.({});   // applies, and records 400x300
        resetAfterCommit?.({});   // nothing changed
        expect(applications).toBe(1);

        size = { width: 800, height: 300 };
        resetAfterCommit?.({});   // root box moved: must apply

        expect(applications).toBe(2);
    });

    // The importer installs its hook during module init today, so this only
    // matters if that ordering ever changes -- but before the gate existed a
    // late hook simply ran on the next commit, and a gate that waits for the
    // next host mutation instead would leave a materialized editor with no
    // captured geometry at all until something happened to move.
    it('applies a hook that is installed after earlier commits', () => {
        const host = globalThis as unknown as Record<string, unknown>;

        mutate();
        resetAfterCommit?.({});   // commits before the importer is present
        resetAfterCommit?.({});

        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;
        resetAfterCommit?.({});   // no mutation, but the hook is new

        expect(applications).toBe(1);
    });

    it('is optional for ordinary native React applications', () => {
        expect(() => resetAfterCommit?.({})).not.toThrow();
    });

    // The importer's captured-state matcher runs a selector per captured state
    // on every commit, and a selector that matches nothing reads the whole
    // registry before answering. The epoch lets it retain that answer until a
    // mutation could have changed it. The boolean gate cannot serve this:
    // resetAfterCommit clears it before the importer runtime ever runs.
    it('publishes a monotonic mutation epoch for the importer runtime', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        mutate();
        const first = host.__pulpMaterializedTreeEpoch__ as number;
        expect(typeof first).toBe('number');

        mutate();
        expect(host.__pulpMaterializedTreeEpoch__ as number).toBeGreaterThan(first);
    });

    it('does not bump the mutation epoch for a paint-only commit', () => {
        // This is the whole point: a pointer drag emits these by the hundred,
        // and each one would otherwise force a full registry rescan per
        // captured state.
        const host = globalThis as unknown as Record<string, unknown>;
        const commitUpdate = PulpHostConfig.commitUpdate as
            (...args: unknown[]) => void;
        mutate();
        const before = host.__pulpMaterializedTreeEpoch__ as number;

        commitUpdate(instance('a'), null, 'view',
            { backgroundColor: '#111', cursor: 'grab' },
            { backgroundColor: '#222', cursor: 'grabbing' }, null);

        expect(host.__pulpMaterializedTreeEpoch__ as number).toBe(before);
    });

    it('bumps the mutation epoch when a structural mutation reorders siblings', () => {
        // insertBefore's same-parent branch returns without reaching attach, so
        // a mark placed in the helper would miss it -- and a captured-state
        // selector that keys off sibling order would answer from a stale miss.
        const host = globalThis as unknown as Record<string, unknown>;
        mutate();
        const before = host.__pulpMaterializedTreeEpoch__ as number;

        reorderSiblings();

        expect(host.__pulpMaterializedTreeEpoch__ as number).toBeGreaterThan(before);
    });

    it('refreshes captured semantic state once per React commit', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let refreshes = 0;
        host.__pulpRefreshMaterializedState__ = () => ++refreshes;
        resetAfterCommit?.({});
        resetAfterCommit?.({});
        expect(refreshes).toBe(2);
    });

    it('publishes mixed-content text renderer targets on the owning DOM node', () => {
        const parent = {
            id: 'button', type: 'button', props: {}, childIds: [],
            onBridge: false, pendingChildren: [],
            _dom: { _children: [] },
        } as never;
        const createText = PulpHostConfig.createTextInstance as
            (text: string, root: unknown, context: unknown, handle: unknown) => unknown;
        const append = PulpHostConfig.appendInitialChild as
            (parent: unknown, child: unknown) => void;
        const text = createText('FLAT', { rootId: '', nextId: 0 }, {}, {});
        append(parent, text);

        const targets = (parent as unknown as { _dom: {
            __pulpAnonymousTextTargets: unknown[];
        } })._dom.__pulpAnonymousTextTargets as Array<{
            id: string; text: string;
        }>;
        expect(targets).toHaveLength(1);
        expect(targets[0].id).toMatch(/^pr_[0-9]+$/);
        expect(targets[0].text).toBe('FLAT');
    });

    it('publishes pure host text on the DOM shim used by materialized metadata', () => {
        class ElementShim {
            _textContent = '';
            _nativeCreated = false;
            __pulpId = '';
            id = '';
            setAttribute() {}
            removeAttribute() {}
            constructor(_tag: string, _id: string) {}
        }
        const host = globalThis as unknown as Record<string, unknown>;
        const oldElement = host.Element;
        host.Element = ElementShim;
        try {
            const create = PulpHostConfig.createInstance as
                (type: string, props: unknown, root: unknown,
                 context: unknown, handle: unknown) => unknown;
            const instance = create('div', { children: 'Theme' },
                { rootId: '', nextId: 0 }, {}, {}) as {
                    _dom: { _textContent: string };
                };
            expect(instance._dom._textContent).toBe('Theme');
        } finally {
            host.Element = oldElement;
        }
    });

    // A pointer crossing a hover target rewrites a tint and a cursor. Without
    // a gate that commit re-applies captured metadata across the whole
    // document, once per pointer sample of a drag.
    //
    // `resetAfterCommit` also re-applies when the hook identity itself
    // changed, so installing a fresh spy arms one unconditional application.
    // Every test here drains that with a priming call before it counts.
    function armSpy(): { count: () => number } {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;
        const resetAfterCommit = PulpHostConfig.resetAfterCommit as
            ((c: unknown) => void) | undefined;
        resetAfterCommit?.({});
        // CONTROL: the priming call must itself have applied. If it did not,
        // the spy was never installed and every count below is vacuous.
        expect(applications).toBe(1);
        applications = 0;
        return { count: () => applications };
    }

    it('does not re-apply metadata for a paint-only commit', () => {
        const spy = armSpy();
        const commitUpdate = PulpHostConfig.commitUpdate as
            (...args: unknown[]) => void;
        const resetAfterCommit = PulpHostConfig.resetAfterCommit as
            ((c: unknown) => void) | undefined;

        commitUpdate(instance('a'), null, 'view',
            { style: { cursor: 'default' } },
            { style: { cursor: 'pointer' } }, null);
        resetAfterCommit?.({});
        expect(spy.count()).toBe(0);

        commitUpdate(instance('a'), null, 'view',
            { background: '#111', boxShadow: 'none' },
            { background: '#222', boxShadow: '0 0 2px #000' }, null);
        resetAfterCommit?.({});
        expect(spy.count()).toBe(0);

        // CONTROL: a geometric change on the same instrument must still
        // re-apply. If this reads 0 the spy is broken, not the gate.
        commitUpdate(instance('a'), null, 'view',
            { width: 10 }, { width: 20 }, null);
        resetAfterCommit?.({});
        expect(spy.count()).toBe(1);
    });

    // prop-applier-paint.ts also owns `border` and `borderWidth`, which carry
    // a width. Module membership must never be mistaken for paint-only.
    it('re-applies metadata when a border width changes', () => {
        const spy = armSpy();
        const commitUpdate = PulpHostConfig.commitUpdate as
            (...args: unknown[]) => void;
        const resetAfterCommit = PulpHostConfig.resetAfterCommit as
            ((c: unknown) => void) | undefined;

        commitUpdate(instance('a'), null, 'view',
            { borderWidth: 1 }, { borderWidth: 4 }, null);
        resetAfterCommit?.({});
        expect(spy.count()).toBe(1);

        // A colour-only border change is safe and must be suppressed.
        commitUpdate(instance('a'), null, 'view',
            { borderColor: '#111' }, { borderColor: '#222' }, null);
        resetAfterCommit?.({});
        expect(spy.count()).toBe(1);
    });

    // `opacity` and `color` are written by the captured-metadata pass itself.
    // Suppressing their commits would leave React's value standing until some
    // later structural commit restored the captured one — a flicker, not a
    // saving. They must take the ordinary path however cheap they look.
    it('re-applies metadata for channels the metadata pass owns', () => {
        const spy = armSpy();
        const commitUpdate = PulpHostConfig.commitUpdate as
            (...args: unknown[]) => void;
        const resetAfterCommit = PulpHostConfig.resetAfterCommit as
            ((c: unknown) => void) | undefined;

        commitUpdate(instance('a'), null, 'view',
            { opacity: 0.5 }, { opacity: 1 }, null);
        resetAfterCommit?.({});
        expect(spy.count()).toBe(1);

        commitUpdate(instance('a'), null, 'view',
            { color: '#111' }, { color: '#222' }, null);
        resetAfterCommit?.({});
        expect(spy.count()).toBe(2);
    });
});
