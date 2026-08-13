// getPublicInstance returns a DOM-shim Element bound to the native widget
// id. Imported React bundles call DOM-style methods on ref.current (e.g.
// canvasRef.current.getContext('2d'), wrapRef.current.getBoundingClientRect()).
// Without this shim, ref.current is a plain Instance descriptor, methods like
// .getContext do not exist, and bundle effects can enter a re-render loop.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { PulpHostConfig } from '../src/host-config.js';

describe('getPublicInstance returns DOM-shim Element', () => {
    const g = globalThis as Record<string, unknown>;
    let savedElement: unknown;

    beforeEach(() => {
        savedElement = g.Element;
    });
    afterEach(() => {
        g.Element = savedElement;
    });

    it('returns _dom if present (the Element shim path)', () => {
        const fn = PulpHostConfig.getPublicInstance!;
        const fakeShim = { __pulpId: 'k', getContext: () => null };
        const inst = {
            id: 'k', type: 'canvas', props: {}, childIds: [],
            onBridge: true, pendingChildren: [],
            _dom: fakeShim,
        } as unknown as Parameters<typeof fn>[0];
        const result = fn(inst);
        expect(result).toBe(fakeShim);
    });

    it('falls back to Instance when _dom is absent (pure-JS test path)', () => {
        const fn = PulpHostConfig.getPublicInstance!;
        const inst = {
            id: 'k', type: 'View', props: {}, childIds: [],
            onBridge: true, pendingChildren: [],
        } as unknown as Parameters<typeof fn>[0];
        // No _dom field — function should return the instance itself.
        const result = fn(inst);
        expect(result).toBe(inst);
    });

    it('createInstance installs _dom when global Element constructor available', () => {
        // Stub a minimal Element ctor with the shape host-config expects.
        const calls: Array<{ tag: string; nativeId: string }> = [];
        g.Element = function (this: Record<string, unknown>, tag: string, nativeId: string) {
            calls.push({ tag, nativeId });
            this.tag = tag;
            this.nativeId = nativeId;
        };
        const create = PulpHostConfig.createInstance!;
        const inst = create(
            'canvas',
            { id: 'k1' } as Record<string, unknown>,
            {} as unknown as Parameters<typeof create>[2],
            {} as unknown as Parameters<typeof create>[3],
            null,
        ) as Record<string, unknown>;
        expect(calls).toHaveLength(1);
        expect(calls[0].tag).toBe('canvas');
        expect(calls[0].nativeId).toBe('k1');
        expect(inst._dom).toBeDefined();
        expect((inst._dom as Record<string, unknown>)._nativeCreated).toBe(true);
        expect((inst._dom as Record<string, unknown>).__pulpId).toBe('k1');
        // The public .id property must be set on the shim. Element constructor
        // seeds internal `_id`, but the public `.id` getter is gated on
        // `_userIdSet`, which only flips through the setter. ref.current.id
        // should match the native id.
        expect((inst._dom as Record<string, unknown>).id).toBe('k1');
    });

    it('preserves imported semantic attributes on the public DOM shim', () => {
        const attributes = new Map<string, string>();
        g.Element = function (this: Record<string, unknown>) {
            this.setAttribute = (name: string, value: unknown) => {
                attributes.set(name, String(value));
            };
            this.removeAttribute = (name: string) => attributes.delete(name);
        };
        const create = PulpHostConfig.createInstance!;
        const inst = create(
            'button',
            {
                id: 'settings',
                'data-spectr-settings-open': true,
                'aria-label': 'Settings',
                title: 'Settings',
            } as Record<string, unknown>,
            {} as unknown as Parameters<typeof create>[2],
            {} as unknown as Parameters<typeof create>[3],
            null,
        ) as Record<string, unknown>;
        expect(attributes.get('data-spectr-settings-open')).toBe('');
        expect(attributes.get('aria-label')).toBe('Settings');
        expect(attributes.get('title')).toBe('Settings');

        const commit = PulpHostConfig.commitUpdate!;
        commit(
            inst as unknown as Parameters<typeof commit>[0],
            true,
            'button',
            { 'aria-label': 'Settings', title: 'Settings' },
            { 'aria-label': 'Preferences', 'data-spectr-settings-open': false },
            null,
        );
        expect(attributes.get('aria-label')).toBe('Preferences');
        expect(attributes.has('title')).toBe(false);
        expect(attributes.has('data-spectr-settings-open')).toBe(false);
    });

    it('keeps DOM shim ancestry aligned without rematerializing widgets', () => {
        const create = PulpHostConfig.createInstance!;
        g.Element = function (this: Record<string, unknown>) {
            this._children = [];
            this._parentElement = null;
            this.setAttribute = () => {};
            this.removeAttribute = () => {};
        };
        const root = { rootId: 'root', nextId: 0 } as unknown as
            Parameters<typeof create>[2];
        const parent = create('div', { id: 'menu-root' }, root, {}, null) as
            Record<string, unknown>;
        const child = create('div', { id: 'menu-options' }, root, {}, null) as
            Record<string, unknown>;
        const parentDom = parent._dom as Record<string, unknown>;
        const childDom = child._dom as Record<string, unknown>;

        PulpHostConfig.appendInitialChild!(
            parent as unknown as Parameters<NonNullable<typeof PulpHostConfig.appendInitialChild>>[0],
            child as unknown as Parameters<NonNullable<typeof PulpHostConfig.appendInitialChild>>[1],
        );
        expect(childDom._parentElement).toBe(parentDom);
        expect(parentDom._children).toEqual([childDom]);

        PulpHostConfig.removeChild!(
            parent as unknown as Parameters<NonNullable<typeof PulpHostConfig.removeChild>>[0],
            child as unknown as Parameters<NonNullable<typeof PulpHostConfig.removeChild>>[1],
        );
        expect(childDom._parentElement).toBeNull();
        expect(parentDom._children).toEqual([]);
    });

    it('createInstance survives when global Element is missing (test sandbox)', () => {
        delete g.Element;
        const create = PulpHostConfig.createInstance!;
        const inst = create(
            'div',
            { id: 'k2' } as Record<string, unknown>,
            {} as unknown as Parameters<typeof create>[2],
            {} as unknown as Parameters<typeof create>[3],
            null,
        ) as Record<string, unknown>;
        expect(inst._dom).toBeNull();
    });
});
