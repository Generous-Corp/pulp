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
});

describe('host-config materialized metadata', () => {
    it('re-applies captured metadata after every committed dynamic update', () => {
        const host = globalThis as unknown as Record<string, unknown>;
        let applications = 0;
        host.__pulpApplyMaterializedImportMetadata__ = () => ++applications;

        resetAfterCommit?.({});
        resetAfterCommit?.({});

        expect(applications).toBe(2);
    });

    it('is optional for ordinary native React applications', () => {
        expect(() => resetAfterCommit?.({})).not.toThrow();
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
