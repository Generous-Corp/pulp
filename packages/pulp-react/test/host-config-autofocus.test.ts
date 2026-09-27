import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import React, { act } from 'react';
import { createRoot, render, unmount } from '../src/index.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';

const h = React.createElement;

describe('@pulp/react mount-time focus', () => {
    let bridge: MockBridge;
    beforeEach(() => {
        bridge = createMockBridge();
        bridge.install();
    });
    afterEach(() => bridge.uninstall());

    const focusCalls = () => bridge.calls.filter(c => c.fn === 'setFocus').map(c => c.args[0]);

    it('focuses an autoFocus input once on mount, never on re-render', () => {
        const root = createRoot('af-root');
        const tree = (label: string) => h('div', { id: 'panel' },
            h('input', { id: 'name', type: 'text', autoFocus: true, placeholder: label }));
        act(() => { render(tree('first'), root); });
        expect(focusCalls()).toEqual(['name']);

        act(() => { render(tree('second'), root); });
        expect(focusCalls()).toEqual(['name']);
        act(() => unmount(root));
    });

    it('lets only the first autoFocus element of a commit take focus', () => {
        const root = createRoot('af-first-root');
        act(() => {
            render(h('div', { id: 'panel' },
                h('input', { id: 'a', autoFocus: true }),
                h('input', { id: 'b', autoFocus: true })), root);
        });
        expect(focusCalls()).toEqual(['a']);
        act(() => unmount(root));
    });

    it('focuses a mounting dialog\'s first text field when nothing asks for autofocus', () => {
        const root = createRoot('dlg-root');
        const App = ({ open }: { open: boolean }) => h('div', { id: 'app' },
            h('button', { id: 'open' }, 'Open'),
            open ? h('div', { id: 'dlg', role: 'dialog' },
                h('input', { id: 'dlg-check', type: 'checkbox' }),
                h('div', { id: 'dlg-body' },
                    h('input', { id: 'dlg-name', type: 'text' }),
                    h('textarea', { id: 'dlg-notes' }))) : null);
        act(() => { render(h(App, { open: false }), root); });
        expect(focusCalls()).toEqual([]);

        act(() => { render(h(App, { open: true }), root); });
        expect(focusCalls()).toEqual(['dlg-name']);

        // Re-rendering the open dialog does not steal focus back.
        act(() => { render(h(App, { open: true }), root); });
        expect(focusCalls()).toEqual(['dlg-name']);
        act(() => unmount(root));
    });

    it('prefers an explicit autoFocus inside the dialog over the first field', () => {
        const root = createRoot('dlg-af-root');
        act(() => {
            render(h('div', { id: 'dlg', 'aria-modal': true },
                h('input', { id: 'first' }),
                h('input', { id: 'second', autoFocus: true })), root);
        });
        expect(focusCalls()).toEqual(['second']);
        act(() => unmount(root));
    });

    it('prefers an explicit autoFocus mounted after the dialog in the same commit', () => {
        const root = createRoot('dlg-later-root');
        act(() => {
            render(h('div', { id: 'app' },
                h('div', { id: 'dlg', role: 'dialog' }, h('input', { id: 'dlg-field' })),
                h('input', { id: 'outside', autoFocus: true })), root);
        });
        expect(focusCalls()).toEqual(['dlg-field', 'outside']);
        act(() => unmount(root));
    });

    it('honours data-pulp-autofocus="off" on the dialog', () => {
        const root = createRoot('dlg-off-root');
        act(() => {
            render(h('div', { id: 'dlg', role: 'dialog', 'data-pulp-autofocus': 'off' },
                h('input', { id: 'field' })), root);
        });
        expect(focusCalls()).toEqual([]);
        act(() => unmount(root));
    });
});
