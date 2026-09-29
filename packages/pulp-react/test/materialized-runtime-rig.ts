// A headless materialized-import panel: the generated importer runtime entry
// evaluated in this realm against a fake element registry, driven by the real
// @pulp/react host config. Every native bridge call either half makes lands in
// one recorder, so a test can count exactly what a React commit costs and read
// back the last value each native setter received.
//
// The entry is a module whose only imports are React and the native renderer.
// Those lines are stripped and stubbed; everything else runs verbatim, so the
// cases exercise the shipped source rather than a re-implementation of it.

import { readFileSync, readdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { PulpHostConfig } from '../src/host-config.js';
// @ts-expect-error -- plain ESM helper outside this package's TS project.
import { buildMaterializedRuntimeEntry } from '../../../tools/import-design/jsx-runtime/materialized_runtime_entry.mjs';

const here = dirname(fileURLToPath(import.meta.url));
type Host = Record<string, unknown>;
const host = globalThis as unknown as Host;

// Every verb the prop applier or the importer runtime can call. Collected from
// the sources so a new setter is recorded rather than silently skipped (the
// prop applier treats a missing bridge function as "capability absent").
function bridgeVerbs(): string[] {
    const verbs = new Set<string>();
    const srcDir = resolve(here, '..', 'src');
    for (const name of readdirSync(srcDir)) {
        if (!name.endsWith('.ts')) continue;
        const text = readFileSync(resolve(srcDir, name), 'utf8');
        for (const m of text.matchAll(/call\('([A-Za-z]+)'/g)) verbs.add(m[1]);
    }
    for (const verb of ['setPosition', 'setLeft', 'setTop', 'setFlex',
        'setOpacity', 'setTextColor', 'setSvgFill', 'setSvgStroke',
        'setSvgStrokeWidth', 'setFontFamily', 'setFontSize', 'setFontWeight',
        'setFontStyle', 'setLetterSpacing', 'setCapturedLineBoxes',
        'clearCapturedLineBoxes', 'createCol', 'setVisible', 'setPointerEvents',
        'setZIndex', 'setTransformOrigin', 'setTransform', 'setBackground',
        'layout']) verbs.add(verb);
    return [...verbs];
}

export interface BridgeCall { verb: string; args: unknown[] }

export interface RigNode {
    tagName: string;
    __pulpId: string;
    id: string;
    textContent: string;
    parentElement: RigNode | null;
    __pulpTextTargetId?: string;
    getAttribute(name: string): string | null;
    setAttribute(name: string, value: unknown): void;
    removeAttribute(name: string): void;
    readonly _children: RigNode[];
}

export interface Rig {
    calls: BridgeCall[];
    childReads: () => number;
    epoch: () => number | undefined;
    node: (id: string) => RigNode;
    /** Last value a verb received for an id (argument 1), or undefined. */
    last: (verb: string, id: string) => unknown;
    count: (verb?: string) => number;
    reset: () => void;
    commit: (id: string, type: string,
             oldProps: Record<string, unknown>,
             newProps: Record<string, unknown>) => void;
    teardown: () => void;
    entry: Host;
}

function element(tag: string, id: string, text: string,
                 onChildRead: () => void): RigNode & { kids: RigNode[] } {
    const attrs = new Map<string, string>();
    const kids: RigNode[] = [];
    const node = {
        tagName: tag.toUpperCase(), __pulpId: id, id, textContent: text,
        parentElement: null as RigNode | null, kids,
        getAttribute: (name: string) => attrs.has(name) ? attrs.get(name)! : null,
        setAttribute: (name: string, value: unknown) => { attrs.set(name, String(value)); },
        removeAttribute: (name: string) => { attrs.delete(name); },
    };
    Object.defineProperty(node, '_children', {
        get() { onChildRead(); return kids; }, enumerable: false,
    });
    return node as unknown as RigNode & { kids: RigNode[] };
}

export const ROWS = 24;
// Captured paint the metadata pass writes; distinct from anything React sets.
export const CAPTURED_ICON = { color: 'rgb(200, 200, 200)', fill: 'rgb(10, 20, 30)',
    stroke: 'rgb(40, 50, 60)', opacity: 0.75 };
export const CAPTURED_OWNED_BUTTON_COLOR = 'rgb(1, 2, 3)';
export const OWNED_BUTTON_ROW = 3;

export interface RigOptions {
    /** Extra captured states (their metadata defaults to the home document). */
    states?: Array<{ id: string; match: { selector: string; ancestor?: string } }>;
    /** Duplicate text bindings resolving to the same Label. */
    duplicateTextBindings?: number;
}

// A panel of ROWS rows; each row is a button (label child), a span, and an svg
// icon. Layout bindings cover every element, text bindings every label, and
// paint bindings every icon plus one button whose colour the capture owns.
export function createRig(options: RigOptions = {}): Rig {
    const calls: BridgeCall[] = [];
    let childReads = 0;
    const onChildRead = () => { ++childReads; };
    const verbs = bridgeVerbs();
    const installed: string[] = [];
    const saved = new Map<string, unknown>();
    const install = (name: string, value: unknown) => {
        if (!saved.has(name)) saved.set(name, host[name]);
        host[name] = value;
        installed.push(name);
    };
    for (const verb of verbs) {
        install(verb, (...args: unknown[]) => { calls.push({ verb, args }); });
    }
    install('getLayoutBoxMetrics', (...args: unknown[]) => {
        calls.push({ verb: 'getLayoutBoxMetrics', args });
        return null;
    });
    install('getRootSize', () => ({ width: 800, height: 600 }));

    const nodes = new Map<string, RigNode & { kids: RigNode[] }>();
    const add = (tag: string, id: string, text: string,
                 parent: (RigNode & { kids: RigNode[] }) | null) => {
        const node = element(tag, id, text, onChildRead);
        if (parent) { node.parentElement = parent; parent.kids.push(node); }
        nodes.set(id, node);
        return node;
    };
    const root = add('div', 'root', '', null);
    const layout: unknown[] = [];
    const text: unknown[] = [];
    const paint: unknown[] = [];
    const box = { left: 1, top: 2, width: 30, height: 10 };
    const typography = (t: string) => ({
        text: t, boxes: [{ left: 0, top: 0, width: 20, height: 10 }],
        basis: { width: 20, resolved_face: 'Inter', requested: {
            font_family: 'Inter', font_size: 12, font_weight: 400,
            font_slant: 0, letter_spacing: 0 } },
    });
    layout.push({ index: 0, tag: 'div', path: [{ index: 0, tag: 'div' }], box });
    for (let r = 0; r < ROWS; ++r) {
        const row = add('div', `row${r}`, '', root);
        const button = add('button', `btn${r}`, `Btn ${r}`, row);
        button.__pulpTextTargetId = `btn${r}__text`;
        add('span', `label${r}`, `Label ${r}`, row);
        const svg = add('svg', `svg${r}`, '', row);
        add('path', `icon${r}`, '', svg);
        const rowPath = [{ index: 0, tag: 'div' }, { index: r, tag: 'div' }];
        layout.push({ index: layout.length, tag: 'div', path: rowPath, box });
        layout.push({ index: layout.length, tag: 'button',
            path: [...rowPath, { index: 0, tag: 'button' }], box });
        layout.push({ index: layout.length, tag: 'span',
            path: [...rowPath, { index: 1, tag: 'span' }], box });
        layout.push({ index: layout.length, tag: 'svg',
            path: [...rowPath, { index: 2, tag: 'svg' }], box });
        text.push({ ...typography(`Btn ${r}`),
            path: [...rowPath, { index: 0, tag: 'button' }] });
        text.push({ ...typography(`Label ${r}`),
            path: [...rowPath, { index: 1, tag: 'span' }] });
        paint.push({ index: paint.length, tag: 'path',
            path: [...rowPath, { index: 2, tag: 'svg' }, { index: 0, tag: 'path' }],
            paint: { opacity: CAPTURED_ICON.opacity, color: CAPTURED_ICON.color,
                fill: CAPTURED_ICON.fill, stroke: CAPTURED_ICON.stroke,
                stroke_width: 1, stroke_dasharray: 'none' } });
        if (r === OWNED_BUTTON_ROW) {
            paint.push({ index: paint.length, tag: 'button',
                path: [...rowPath, { index: 0, tag: 'button' }],
                paint: { opacity: 1, color: CAPTURED_OWNED_BUTTON_COLOR,
                    fill: 'none', stroke: 'none', stroke_width: 0,
                    stroke_dasharray: 'none' } });
        }
    }
    for (let d = 0; d < (options.duplicateTextBindings ?? 0); ++d) {
        text.push({ ...typography('Label 0'),
            path: [{ index: 0, tag: 'div' }, { index: 0, tag: 'div' },
                { index: 1, tag: 'span' }] });
    }
    const metadata = { layout_bindings: layout, text_bindings: text,
        paint_bindings: paint };
    const states = (options.states ?? []).map(state => ({ ...state, metadata }));

    const registry = new Map<string, RigNode>(nodes);
    install('__pulpReactDomRegistry__', registry);
    install('__pulpRuntimeImport__', () => {});
    install('App', () => null);

    const source = (buildMaterializedRuntimeEntry({
        capturedCssVariables: {}, presentationTime: 0, requestedState: '',
        textBindings: text, layoutBindings: layout, paintBindings: paint,
        runtimeDocumentAsset: null, sidecar: null, productPrelude: '',
        surfaceBackground: null, authoredLeft: 0, authoredTop: 0,
        authoredWidth: 800, authoredHeight: 600, authoredTransform: null,
        visualAuthority: 'native', stateAtlas: states, visualWidth: 800,
        visualHeight: 600, canvasBindings: [], behaviorCanvasAnchors: [],
        capturedPaintAuthorityAnchors: [],
    }) as string)
        .replace(/^import .*$/gm, '')
        // The entry pins performance.now to its replay clock. Keep that off
        // the test runner's own global.
        .replace('g.performance = {', 'g.__pulpRigPerformance__ = {');
    for (const name of ['__pulpCssVars', '__pulpNativeBridgeFunctions__', 'React',
        'ReactDOM', '__pulpRetainedCanvasFrames__', '__pulpLogicalCanvasScale__',
        '__pulpCapturedPresentationTime__', '__pulpCapturedReplayClock__',
        '__pulpAnimationFrameTimestamp__', '__pulpRequestedMaterializedState__',
        '__pulpRigPerformance__', '__pulpApplyMaterializedImportMetadata__',
        '__pulpFindMaterializedElement__', '__pulpActivateMaterializedElement__',
        '__pulpRequestedMaterializedActivation__', '__pulpRefreshMaterializedState__',
        '__pulpApplyMaterializedVisualAuthority__', '__pulpBindMaterializedCanvases__',
        '__pulpMaterializedMetadataDiagnostics__',
        '__pulpMaterializedScopedApplyDiagnostics__',
        '__pulpReconcileMaterializedPaint__', '__pulpMaterializedSelectorAttributes__',
        '__pulpReactDomRegistryValues__']) {
        if (!saved.has(name)) saved.set(name, host[name]);
        installed.push(name);
    }
    const run = new Function('React', 'createPulpRoot', 'renderPulp', 'unmountPulp',
        source);
    run({ createElement: () => null }, () => ({}), () => {}, () => {});

    // Drain the arrival of the hooks: the first commit after they appear is
    // deliberately a full application.
    (PulpHostConfig.resetAfterCommit as (c: unknown) => void)({});

    const instances = new Map<string, Record<string, unknown>>();
    const instanceFor = (id: string, type: string) => {
        let inst = instances.get(id);
        if (!inst) {
            const dom = nodes.get(id);
            inst = { id, type, props: {}, childIds: [], onBridge: true,
                pendingChildren: [], _dom: dom ?? null,
                textTargetId: type === 'button' ? `${id}__text` : undefined };
            instances.set(id, inst);
        }
        return inst;
    };

    return {
        calls,
        childReads: () => childReads,
        // The epoch is the host config's own counter; it outlives a rig.
        epoch: () => host.__pulpMaterializedTreeEpoch__ as number | undefined,
        node: (id: string) => {
            const node = nodes.get(id);
            if (!node) throw new Error(`no rig node ${id}`);
            return node;
        },
        last: (verb: string, id: string) => {
            for (let i = calls.length - 1; i >= 0; --i) {
                const call = calls[i];
                if (call.verb === verb && String(call.args[0]) === id)
                    return call.args[1];
            }
            return undefined;
        },
        count: (verb?: string) => verb
            ? calls.filter(call => call.verb === verb).length : calls.length,
        reset: () => { calls.length = 0; childReads = 0; },
        commit: (id, type, oldProps, newProps) => {
            (PulpHostConfig.commitUpdate as (...args: unknown[]) => void)(
                instanceFor(id, type), null, type, oldProps, newProps, null);
            (PulpHostConfig.resetAfterCommit as (c: unknown) => void)({});
        },
        teardown: () => {
            for (const name of installed) {
                const previous = saved.get(name);
                if (previous === undefined) delete host[name];
                else host[name] = previous;
            }
        },
        entry: host,
    };
}
