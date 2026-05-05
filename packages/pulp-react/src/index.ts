// @pulp/react — react-reconciler host for pulp::view::WidgetBridge.
//
// Public API:
//   import { render, unmount, View, Row, Spectrum, Button, ... } from '@pulp/react';
//
//   render(<MyApp />, rootContainer)  // or just render(<MyApp />) for the default root
//   unmount(rootContainer)
//
// Plus all the intrinsics from intrinsics.ts.

import createReactReconciler from 'react-reconciler';
import { LegacyRoot } from 'react-reconciler/constants.js';
import type { ReactElement } from 'react';
import type { OpaqueRoot } from 'react-reconciler';

import { PulpHostConfig } from './host-config.js';
import type { PulpContainer } from './types.js';

// ── Reconciler ─────────────────────────────────────────────────────
// react-reconciler returns a factory; we keep a single shared instance
// so DevTools registration only happens once per JS engine load.
const reconciler = createReactReconciler(PulpHostConfig as unknown as Parameters<typeof createReactReconciler>[0]);

// Optional DevTools hookup (no-op if devtools not present).
try {
    reconciler.injectIntoDevTools({
        bundleType: 0,             // 0 = production, 1 = development
        version: '0.0.1',
        rendererPackageName: '@pulp/react',
    });
} catch { /* no devtools — ignore */ }

// ── Container management ───────────────────────────────────────────
interface RootRecord {
    container: PulpContainer;
    fiberRoot: OpaqueRoot;
}

const rootsByContainer = new WeakMap<PulpContainer, RootRecord>();

/// Create a new container rooted at the given bridge widget id.
/// Plugin authors call this once at startup with the id of the
/// outermost widget Pulp gives them ('' is the convention for the
/// implicit root — see widget_bridge.cpp's root_ handling).
export function createRoot(rootId: string = ''): PulpContainer {
    return { rootId, nextId: 0 };
}

/// Render a React element into the given container.
/// If no container is passed, creates one rooted at '' (the bridge's
/// implicit root) on first call and reuses it thereafter.
let defaultContainer: PulpContainer | null = null;

export function render(element: ReactElement, container?: PulpContainer): PulpContainer {
    const c = container ?? (defaultContainer ??= createRoot(''));
    let rec = rootsByContainer.get(c);
    if (!rec) {
        const fiberRoot = reconciler.createContainer(
            c,
            // LegacyRoot = synchronous mode. Matches the v0 architecture
            // doc ("Concurrent mode: deferred for v0") and means each
            // render() returns after the bridge calls have all been
            // emitted — no microtask gap, which is what tests and
            // AOT-bundled plugin code both expect.
            LegacyRoot,
            null,
            false,
            null,
            '@pulp/react',
            (err: Error) => { console.error('[@pulp/react] recoverable error:', err); },
            null,
        );
        rec = { container: c, fiberRoot };
        rootsByContainer.set(c, rec);
    }
    reconciler.updateContainer(element, rec.fiberRoot, null, null);
    return c;
}

/// Unmount and clear the container.
export function unmount(container: PulpContainer): void {
    const rec = rootsByContainer.get(container);
    if (!rec) return;
    reconciler.updateContainer(null, rec.fiberRoot, null, null);
    rootsByContainer.delete(container);
    if (defaultContainer === container) defaultContainer = null;
}

// ── Re-export intrinsics + types ───────────────────────────────────
export * from './intrinsics.js';
export type {
    ViewProps, RowProps, ColProps, PanelProps, ScrollViewProps, ModalProps,
    LabelProps, ButtonProps, TextEditorProps,
    KnobProps, FaderProps, SpectrumProps, WaveformProps, MeterProps,
    ProgressProps, XYPadProps, CheckboxProps, ToggleProps, ComboProps,
    ListBoxProps, CanvasProps, ImageProps, IconProps, SvgPathProps,
    SvgRectProps, SvgLineProps,
    FlexDirection, FlexAlign, FlexAlignSelf, FlexJustify,
    FlexProps, StyleProps, BaseProps,
    IntrinsicElementMap, IntrinsicElementName,
    PulpContainer,
} from './types.js';

// ── Re-export the mock bridge for downstream tests ─────────────────
export { createMockBridge } from './bridge.js';
export type { MockBridge, MockBridgeCall } from './bridge.js';
