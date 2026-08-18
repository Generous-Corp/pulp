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
    commitUpdate(instance('a'), null, 'view', {}, {}, null);
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
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        mutate();
        resetAfterCommit?.({});   // structural commit: applies
        resetAfterCommit?.({});   // state-only commit: must not
        resetAfterCommit?.({});   // still nothing mutated

        expect(applications).toBe(1);
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
});
