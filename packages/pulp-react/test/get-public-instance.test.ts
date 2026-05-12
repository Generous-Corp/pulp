// Pulp #468 — getPublicInstance returns a DOM-shim Element bound to
// the native widget id. Imported React bundles call DOM-style methods
// on ref.current (e.g. canvasRef.current.getContext('2d'),
// wrapRef.current.getBoundingClientRect()). Without this shim, ref.current
// is a plain Instance descriptor → methods like .getContext don't exist
// → bundle's useEffect throws → infinite re-render loop.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { PulpHostConfig } from '../src/host-config.js';

describe('getPublicInstance returns DOM-shim Element (pulp #468)', () => {
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
        // Codex P2 follow-up on #1859: the public .id property must be set
        // on the shim — Element constructor seeds internal `_id` but the
        // public `.id` getter is gated on `_userIdSet`, which only flips
        // through the setter. ref.current.id should match the native id.
        expect((inst._dom as Record<string, unknown>).id).toBe('k1');
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
