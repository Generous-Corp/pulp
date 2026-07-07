import { describe, expect, it, vi } from 'vitest';
import { createContext, createElement, useContext } from 'react';
import type { ReactElement } from 'react';

import { createMockBridge } from '../src/bridge.js';
import type { MockBridge } from '../src/bridge.js';
import { PulpHostConfig } from '../src/host-config.js';
import { Label, VirtualList } from '../src/intrinsics.js';
import { createRoot, render, unmount } from '../src/index.js';
import { applyAllProps, applyChangedProps } from '../src/prop-applier.js';
import type { PulpContainer, PulpInstance } from '../src/types.js';

function instance(id: string, props: Record<string, unknown>): PulpInstance {
    return { id, type: 'VirtualList', props } as PulpInstance;
}

function renderWithBridge(element: ReactElement, rootId = 'test-root'): { bridge: MockBridge; root: PulpContainer } {
    const bridge = createMockBridge();
    bridge.install();
    const root = createRoot(rootId);
    render(element, root);
    return { bridge, root };
}

function cleanupBridge(bridge: MockBridge, root: PulpContainer): void {
    try {
        unmount(root);
    } finally {
        bridge.uninstall();
    }
}

function bridgeEvent(bridge: MockBridge, id: string, eventName: string): (...args: unknown[]) => void {
    const handler = bridge.calls.find(
        (call) => call.fn === 'on' && call.args[0] === id && call.args[1] === eventName,
    )?.args[2];
    expect(typeof handler).toBe('function');
    return handler as (...args: unknown[]) => void;
}

describe('@pulp/react VirtualList intrinsic', () => {
    it('adapts bridge selection and activation events to typed callback values', () => {
        const onChange = vi.fn();
        const onActivate = vi.fn();
        const { bridge, root } = renderWithBridge(createElement(VirtualList, {
            id: 'vl_events',
            rowCount: 100,
            rowHeight: 24,
            renderRow: () => null,
            onChange,
            onActivate,
        }));
        try {
            bridgeEvent(bridge, 'vl_events', 'change')([2, 4, 'ignored']);
            bridgeEvent(bridge, 'vl_events', 'activate')(42);
        } finally {
            cleanupBridge(bridge, root);
        }

        expect(onChange).toHaveBeenCalledWith([2, 4]);
        expect(onActivate).toHaveBeenCalledWith(42);
    });

    it('applies row metrics through the virtual-list bridge setters', () => {
        const { bridge, root } = renderWithBridge(createElement(VirtualList, {
            id: 'vl',
            rowCount: 50000,
            rowHeight: 28,
            overscan: 4,
            renderRow: () => null,
        }));
        try {
            const bindRegistration = bridge.calls.findIndex(
                (c) => c.fn === 'on' && c.args[0] === 'vl' && c.args[1] === 'bindrow',
            );
            const rowCountSetter = bridge.calls.findIndex((c) => c.fn === 'setVirtualListRowCount');
            expect(bindRegistration).toBeGreaterThanOrEqual(0);
            expect(rowCountSetter).toBeGreaterThan(bindRegistration);
            expect(bridge.calls).toContainEqual({ fn: 'setVirtualListRowCount', args: ['vl', 50000] });
            expect(bridge.calls).toContainEqual({ fn: 'setListRowHeight', args: ['vl', 28] });
            expect(bridge.calls).toContainEqual({ fn: 'setVirtualListOverscan', args: ['vl', 4] });
            expect(bridge.calls).toContainEqual({ fn: 'refreshVirtualListRows', args: ['vl'] });
        } finally {
            cleanupBridge(bridge, root);
        }
    });

    it('applies virtual-list metrics before scroll commands regardless of prop order', () => {
        const bridge = createMockBridge();
        bridge.install();
        try {
            applyAllProps(instance('vl_order', {
                onBindRow: () => {},
                scrollToRow: 100,
                rowCount: 1000,
                rowHeight: 10,
                renderRow: () => null,
            }));
        } finally {
            bridge.uninstall();
        }

        const rowCount = bridge.calls.findIndex((c) => c.fn === 'setVirtualListRowCount');
        const rowHeight = bridge.calls.findIndex((c) => c.fn === 'setListRowHeight');
        const scroll = bridge.calls.findIndex((c) => c.fn === 'scrollVirtualListToRow');
        expect(rowCount).toBeGreaterThanOrEqual(0);
        expect(rowHeight).toBeGreaterThan(rowCount);
        expect(scroll).toBeGreaterThan(rowHeight);
    });

    it('applies changed virtual-list metrics before changed scroll commands', () => {
        const bridge = createMockBridge();
        bridge.install();
        try {
            applyChangedProps(instance('vl_commit', {}), {}, {
                scrollToRow: 100,
                rowCount: 1000,
                rowHeight: 10,
            });
        } finally {
            bridge.uninstall();
        }

        const rowCount = bridge.calls.findIndex((c) => c.fn === 'setVirtualListRowCount');
        const rowHeight = bridge.calls.findIndex((c) => c.fn === 'setListRowHeight');
        const scroll = bridge.calls.findIndex((c) => c.fn === 'scrollVirtualListToRow');
        expect(rowCount).toBeGreaterThanOrEqual(0);
        expect(rowHeight).toBeGreaterThan(rowCount);
        expect(scroll).toBeGreaterThan(rowHeight);
    });

    it('clears native selection when selected is nulled or removed', () => {
        const bridge = createMockBridge();
        bridge.install();
        try {
            applyChangedProps(instance('vl_clear_null', {}), { selected: 3 }, { selected: null });
            applyChangedProps(instance('vl_clear_removed', {}), { selected: 4 }, {});
        } finally {
            bridge.uninstall();
        }

        expect(bridge.calls).toContainEqual({
            fn: 'setVirtualListSelected',
            args: ['vl_clear_null', -1],
        });
        expect(bridge.calls).toContainEqual({
            fn: 'setVirtualListSelected',
            args: ['vl_clear_removed', -1],
        });
    });

    it('reapplies selected after virtual-list selection mode changes', () => {
        const bridge = createMockBridge();
        bridge.install();
        try {
            applyChangedProps(
                instance('vl_mode', {}),
                { selectionMode: 'none', selected: 5 },
                { selectionMode: 'single', selected: 5 },
            );
        } finally {
            bridge.uninstall();
        }

        const mode = bridge.calls.findIndex((c) => c.fn === 'setVirtualListSelectionMode');
        const selected = bridge.calls.findIndex((c) => c.fn === 'setVirtualListSelected');
        expect(mode).toBeGreaterThanOrEqual(0);
        expect(selected).toBeGreaterThan(mode);
        expect(bridge.calls).toContainEqual({
            fn: 'setVirtualListSelectionMode',
            args: ['vl_mode', 'single'],
        });
        expect(bridge.calls).toContainEqual({
            fn: 'setVirtualListSelected',
            args: ['vl_mode', 5],
        });
    });

    it('resets removed virtual-list selection mode before replaying selected', () => {
        const bridge = createMockBridge();
        bridge.install();
        try {
            applyChangedProps(
                instance('vl_mode_removed', {}),
                { selectionMode: 'none', selected: 5 },
                { selected: 5 },
            );
        } finally {
            bridge.uninstall();
        }

        const mode = bridge.calls.findIndex((c) => c.fn === 'setVirtualListSelectionMode');
        const selected = bridge.calls.findIndex((c) => c.fn === 'setVirtualListSelected');
        expect(mode).toBeGreaterThanOrEqual(0);
        expect(selected).toBeGreaterThan(mode);
        expect(bridge.calls).toContainEqual({
            fn: 'setVirtualListSelectionMode',
            args: ['vl_mode_removed', 'single'],
        });
        expect(bridge.calls).toContainEqual({
            fn: 'setVirtualListSelected',
            args: ['vl_mode_removed', 5],
        });
    });

    it('unmounts cached row roots when the native row slot is released', () => {
        const { bridge, root } = renderWithBridge(createElement(VirtualList, {
            id: 'vl_release',
            rowCount: 10,
            rowHeight: 24,
            renderRow: () => null,
        }));
        try {
            const bindRow = bridgeEvent(bridge, 'vl_release', 'bindrow');
            const releaseRow = bridgeEvent(bridge, 'vl_release', 'releaserow');

            bridge.reset();
            bindRow({ rowId: 'vl__row_0', index: 3 });
            expect(bridge.calls).toContainEqual({
                fn: 'createRow',
                args: ['vl__row_0__pr_1', 'vl__row_0'],
            });

            bridge.reset();
            releaseRow({ rowId: 'vl__row_0' });
            expect(bridge.calls).toContainEqual({
                fn: 'removeWidget',
                args: ['vl__row_0__pr_1'],
            });

            bridge.reset();
            bindRow({ rowId: 'vl__row_0', index: 4 });
            expect(bridge.calls).toContainEqual({
                fn: 'createRow',
                args: ['vl__row_0__pr_1', 'vl__row_0'],
            });
        } finally {
            cleanupBridge(bridge, root);
        }
    });

    it('runs user release callbacks without shadowing internal row cleanup', () => {
        const onReleaseRow = vi.fn();
        const { bridge, root } = renderWithBridge(createElement(VirtualList, {
            id: 'vl_release_user',
            rowCount: 10,
            rowHeight: 24,
            renderRow: () => null,
            onReleaseRow,
        }));
        try {
            const bindRow = bridgeEvent(bridge, 'vl_release_user', 'bindrow');
            const releaseRow = bridgeEvent(bridge, 'vl_release_user', 'releaserow');

            bridge.reset();
            bindRow({ rowId: 'vl_release_user__row_0', index: 3 });
            expect(bridge.calls).toContainEqual({
                fn: 'createRow',
                args: ['vl_release_user__row_0__pr_1', 'vl_release_user__row_0'],
            });

            bridge.reset();
            releaseRow({ rowId: 'vl_release_user__row_0' });
            expect(onReleaseRow).toHaveBeenCalledTimes(1);
            expect(bridge.calls).toContainEqual({
                fn: 'removeWidget',
                args: ['vl_release_user__row_0__pr_1'],
            });
        } finally {
            cleanupBridge(bridge, root);
        }
    });

    it('does not flush global layout synchronously while binding row roots', () => {
        const { bridge, root } = renderWithBridge(createElement(VirtualList, {
            id: 'vl_layout',
            rowCount: 10,
            rowHeight: 24,
            renderRow: () => null,
        }));
        try {
            const bindRow = bridgeEvent(bridge, 'vl_layout', 'bindrow');

            bridge.reset();
            bindRow({ rowId: 'vl_layout__row_0', index: 3 });

            expect(bridge.calls.some((call) => call.fn === 'layout')).toBe(false);
        } finally {
            cleanupBridge(bridge, root);
        }
    });

    it('remounts row content when a recycled slot is rebound to a different index', () => {
        const { bridge, root } = renderWithBridge(createElement(VirtualList, {
            id: 'vl_state',
            rowCount: 100,
            rowHeight: 24,
            renderRow: (index: number) => index,
        }));
        try {
            const bindRow = bridgeEvent(bridge, 'vl_state', 'bindrow');

            bridge.reset();
            bindRow({ rowId: 'vl_state__row_0', index: 3 });
            const firstRow = bridge.calls.find(
                (call) => call.fn === 'createRow' && call.args[1] === 'vl_state__row_0',
            )?.args[0];
            expect(typeof firstRow).toBe('string');

            bridge.reset();
            bindRow({ rowId: 'vl_state__row_0', index: 400 });

            expect(bridge.calls).toContainEqual({
                fn: 'removeWidget',
                args: [firstRow],
            });
            expect(bridge.calls.some(
                (call) => call.fn === 'createRow' && call.args[1] === 'vl_state__row_0',
            )).toBe(true);
            expect(bridge.calls.some(
                (call) => call.fn === 'setText' && call.args[1] === '400',
            )).toBe(true);
        } finally {
            cleanupBridge(bridge, root);
        }
    });

    it('preserves React context for row content rendered into recycled slots', () => {
        const Theme = createContext('default');
        function ThemedRow(): ReactElement {
            return createElement(Label, null, useContext(Theme));
        }

        const { bridge, root } = renderWithBridge(createElement(
            Theme.Provider,
            { value: 'provided' },
            createElement(VirtualList, {
                id: 'vl_context',
                rowCount: 1,
                rowHeight: 24,
                renderRow: () => createElement(ThemedRow),
            }),
        ));
        try {
            const bindRow = bridgeEvent(bridge, 'vl_context', 'bindrow');

            bridge.reset();
            bindRow({ rowId: 'vl_context__row_0', index: 0 });

            const rowId = bridge.calls.find(
                (call) => call.fn === 'createRow' && call.args[1] === 'vl_context__row_0',
            )?.args[0];
            expect(typeof rowId).toBe('string');
            expect(bridge.calls).toContainEqual({
                fn: 'createLabel',
                args: ['vl_context__row_0__pr_1', 'provided', rowId],
            });
        } finally {
            cleanupBridge(bridge, root);
        }
    });

    it('scopes auto-generated child IDs for row-root containers', () => {
        const container = createRoot('vl__row_0', 'vl__row_0__pr_');
        const child = PulpHostConfig.createInstance(
            'Row' as never,
            {} as never,
            container as never,
            {} as never,
            null as never,
        );

        expect(child.id).toBe('vl__row_0__pr_1');
    });
});
