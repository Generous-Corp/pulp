// Disappearing-style-key resets in applyChangedProps.
//
// When a key falls out of newProps (or its value becomes null/undefined),
// applyChangedProps must hand its native slots back to the widget's starting
// value instead of leaving the previous value painted. This bites the
// conditional-spread idiom that preset chips, pattern rows, and imported
// designs use:
//
//   style={{ ...base, ...(active ? activeStyle : {}) }}
//
// When `active` flips true → false, the spread contributes nothing, the
// active-only keys vanish from newProps, and the bridge would keep painting
// them.
//
// Three things are pinned here:
//   - every key the appliers handle has a decision: a reset rule or a
//     reasoned NO_RESET_NEEDED entry, and the keys whose reset is knowingly
//     inexact are listed by name;
//   - the reset calls themselves, including survivors and host-config
//     creation defaults that share a reset slot being re-applied afterwards;
//   - the bridge-call fixture test_widget_bridge_style_reset.cpp replays to
//     prove the toggled-off render equals a never-styled one.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { applyAllProps, applyChangedProps, normalizeHostProps } from '../src/prop-applier.js';
import { NO_RESET_NEEDED, STYLE_KEY_RULES } from '../src/prop-applier-reset.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

const here = dirname(fileURLToPath(import.meta.url));
const srcDir = resolve(here, '..', 'src');

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
});

function makeInstance(id: string = 'chip', type: string = 'View',
                      creationStyle?: Record<string, unknown>): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
        creationStyle,
    };
}

const callsTo = (fn: string) => bridge.calls.filter((c) => c.fn === fn).map((c) => c.args);

// ── Every handled key, read from the appliers' own source ───────────────

/// The body of `function <name>(` up to its closing brace at column 0.
function functionBody(file: string, name: string): string {
    const text = readFileSync(resolve(srcDir, file), 'utf8');
    const start = text.search(new RegExp(`function ${name}\\(`));
    if (start < 0) throw new Error(`${name} not found in ${file}`);
    const end = text.indexOf('\n}\n', start);
    return text.slice(start, end);
}

/// Prop keys a function dispatches on: `case 'key':` labels and
/// `key === 'name'` comparisons.
function dispatchedKeys(body: string): string[] {
    const keys = new Set<string>();
    for (const m of body.matchAll(/case '([^']+)':/g)) keys.add(m[1]);
    for (const m of body.matchAll(/\bkey === '([^']+)'/g)) keys.add(m[1]);
    return [...keys];
}

const APPLIERS: ReadonlyArray<[string, string]> = [
    ['prop-applier-layout.ts', 'applyLayoutProp'],
    ['prop-applier-paint.ts', 'applyPaintProp'],
    ['prop-applier-typography.ts', 'applyTypographyProp'],
    ['prop-applier-transform.ts', 'applyTransformProp'],
    ['prop-applier-events.ts', 'applyEventProp'],
    ['prop-applier.ts', 'applyOne'],
];

function handledKeys(): string[] {
    const keys = new Set<string>();
    for (const [file, fn] of APPLIERS) {
        for (const key of dispatchedKeys(functionBody(file, fn))) keys.add(key);
    }
    return [...keys].sort();
}

/// Keys with no decision, and decisions for keys nobody handles.
function undecided(handled: readonly string[]): { missing: string[]; stale: string[]; both: string[] } {
    const decided = new Set([...Object.keys(STYLE_KEY_RULES), ...Object.keys(NO_RESET_NEEDED)]);
    const handledSet = new Set(handled);
    return {
        missing: handled.filter((k) => !decided.has(k)),
        stale: [...decided].filter((k) => !handledSet.has(k)).sort(),
        both: Object.keys(STYLE_KEY_RULES).filter((k) => k in NO_RESET_NEEDED),
    };
}

/// Keys whose reset knowingly cannot restore the starting value. Adding one
/// is a decision a reviewer should see, so the list is spelled out.
const KEYS_WITH_GAPS = [
    'border', 'borderBottomColor', 'borderColor', 'borderLeftColor', 'borderRightColor',
    'borderTopColor', 'bottom', 'color', 'end', 'fill', 'fontSize', 'fontWeight', 'inset',
    'insetBlock', 'insetInline', 'left', 'letterSpacing', 'orientation', 'outline',
    'outlineColor', 'outlineOffset', 'outlineStyle', 'outlineWidth', 'right', 'start',
    'stroke', 'strokeGradient', 'text', 'textAlign', 'textColor', 'textDecorationColor', 'top',
    'viewBox',
];

// An old value that makes the reset meaningful for rules that depend on it.
const OLD_VALUE: Record<string, unknown> = { overlay: true, role: 'dialog', 'aria-modal': true };

describe('@pulp/react prop-applier — every handled key has a removal decision', () => {
    const handled = handledKeys();

    it('reads the handled keys from every applier (control)', () => {
        // The extraction is the instrument: if it silently found nothing,
        // every check below would pass vacuously.
        expect(handled.length).toBeGreaterThan(190);
        for (const key of ['padding', 'fontWeight', 'boxShadow', 'transform',
                           'overlay', 'shader', 'src', 'd', 'gridArea']) {
            expect(handled, key).toContain(key);
        }
    });

    it('decides every handled key exactly once and nothing else', () => {
        expect(undecided(handled)).toEqual({ missing: [], stale: [], both: [] });
    });

    it('fails for a handled key that has no decision (planted)', () => {
        const report = undecided([...handled, 'plantedNewStyleKey']);
        expect(report.missing).toEqual(['plantedNewStyleKey']);
    });

    it('gives every NO_RESET_NEEDED entry a reason', () => {
        for (const [key, reason] of Object.entries(NO_RESET_NEEDED)) {
            expect(reason.length, key).toBeGreaterThan(30);
        }
    });

    it('gives every rule slots and a reset or a stated gap', () => {
        for (const [key, rule] of Object.entries(STYLE_KEY_RULES)) {
            expect(rule.slots.length, key).toBeGreaterThan(0);
            expect(Boolean(rule.reset) || Boolean(rule.gap), key).toBe(true);
        }
    });

    it('lists the keys whose reset is knowingly inexact', () => {
        const withGaps = Object.entries(STYLE_KEY_RULES)
            .filter(([, rule]) => rule.gap).map(([key]) => key).sort();
        expect(withGaps).toEqual([...KEYS_WITH_GAPS].sort());
    });

    it('emits a bridge call when any key with a reset disappears', () => {
        for (const [key, rule] of Object.entries(STYLE_KEY_RULES)) {
            if (!rule.reset) continue;
            bridge.reset();
            applyChangedProps(makeInstance('k'), { [key]: OLD_VALUE[key] ?? 'x' }, {});
            expect(bridge.calls.length, `${key} removal emits nothing`).toBeGreaterThan(0);
        }
    });

    it('resets a key whose value became undefined or null', () => {
        applyChangedProps(makeInstance('n'), { fontWeight: 'bold', opacity: 0.5 },
                          { fontWeight: undefined, opacity: null });
        expect(callsTo('setFontWeight')).toEqual([['n', 400]]);
        expect(callsTo('setOpacity')).toEqual([['n', 1]]);
    });
});

describe('@pulp/react prop-applier — disappearing style keys', () => {
    it('clears background and gradient when the key falls out of newProps', () => {
        applyChangedProps(makeInstance('c1'), { background: 'red' }, {});
        expect(callsTo('setBackground')).toEqual([['c1', '']]);
        expect(callsTo('setBackgroundGradient')).toEqual([['c1', '']]);
    });

    it('clears a removed backgroundGradient through both background slots', () => {
        applyChangedProps(makeInstance('c2'),
                          { backgroundGradient: 'linear-gradient(red, blue)' }, {});
        expect(callsTo('setBackgroundGradient')).toEqual([['c2', '']]);
        expect(callsTo('setBackground')).toEqual([['c2', '']]);
    });

    it('collapses a removed border, borderColor or borderWidth to width 0', () => {
        for (const key of ['border', 'borderColor', 'borderWidth']) {
            bridge.reset();
            applyChangedProps(makeInstance('c3'), { [key]: key === 'borderWidth' ? 2 : 'red' }, {});
            expect(callsTo('setBorderWidth'), key).toEqual([['c3', 0]]);
        }
    });

    it('clears each per-side border via setBorderSide(side, 0, transparent)', () => {
        for (const side of ['Top', 'Right', 'Bottom', 'Left'] as const) {
            bridge.reset();
            const key = `border${side}`;
            applyChangedProps(makeInstance('c6'), { [key]: { color: 'red', width: 2 } }, {});
            expect(callsTo('setBorderSide'), key)
                .toEqual([['c6', side.toLowerCase(), 0, 'transparent']]);
        }
    });

    it('asks the typography bridge for the default text colour', () => {
        applyChangedProps(makeInstance('c7', 'Text'), { textColor: '#fff' }, {});
        expect(callsTo('setTextColor')).toEqual([['c7', '']]);
    });

    it('restores the starting value of typography, shadow and transform keys', () => {
        applyChangedProps(makeInstance('t', 'Label'), {
            fontWeight: 'bold', fontSize: 20, letterSpacing: 2, fontStyle: 'italic',
            textDecorationLine: 'underline', textTransform: 'uppercase', lineHeight: 24,
            boxShadow: '0px 2px 4px #000', transform: [{ scale: 1.2 }, { rotate: '10deg' }],
        }, {});
        expect(callsTo('setFontWeight')).toEqual([['t', 400]]);
        expect(callsTo('setFontSize')).toEqual([['t', 14]]);
        expect(callsTo('setLetterSpacing')).toEqual([['t', 0]]);
        expect(callsTo('setFontStyle')).toEqual([['t', 'normal']]);
        expect(callsTo('setTextDecoration')).toEqual([['t', 'none']]);
        expect(callsTo('setTextTransform')).toEqual([['t', 'none']]);
        expect(callsTo('setLineHeight')).toEqual([['t', 0]]);
        expect(callsTo('clearBoxShadow')).toEqual([['t']]);
        expect(callsTo('setScale')).toEqual([['t', 1]]);
        expect(callsTo('setRotation')).toEqual([['t', 0]]);
        expect(callsTo('setTranslate')).toEqual([['t', 0, 0]]);
    });

    it('resets layout slots natively, one slot per written field', () => {
        applyChangedProps(makeInstance('l'), {
            padding: 8, marginTop: 4, width: '50%', flex: 1, gridArea: 'header',
            animationName: 'pulse',
        }, {});
        expect(callsTo('resetStyleProperty')).toEqual([
            ['l', 'padding'], ['l', 'padding_top'], ['l', 'padding_right'],
            ['l', 'padding_bottom'], ['l', 'padding_left'],
            ['l', 'margin_top'], ['l', 'width'],
            ['l', 'flex_grow'], ['l', 'flex_shrink'], ['l', 'flex_basis'],
            ['l', 'grid.grid_area'], ['l', 'animation'],
        ]);
    });

    it('re-applies a surviving key that shares a reset slot', () => {
        // The string shorthand fanned out to all four edges, so its reset
        // clears paddingTop's edge too; paddingTop must win again afterwards.
        applyChangedProps(makeInstance('s'),
                          { padding: '4px 8px', paddingTop: 20 }, { paddingTop: 20 });
        const flex = bridge.calls.filter((c) => c.fn === 'resetStyleProperty' || c.fn === 'setFlex');
        const lastTop = flex.map((c) => c.args).filter((a) => a[1] === 'padding_top').pop();
        expect(lastTop).toEqual(['s', 'padding_top', 20]);
    });

    it('does not re-apply a survivor that shares no slot with the removal', () => {
        applyChangedProps(makeInstance('u'), { background: 'red', opacity: 0.5 }, { opacity: 0.5 });
        expect(callsTo('setOpacity')).toEqual([]);
    });

    it('falls back to a host-config creation default, not the bare widget', () => {
        // A lowercase <button> is created centred; an author override that
        // disappears must land back on centred.
        const button = makeInstance('btn', 'button', { alignItems: 'center', justifyContent: 'center' });
        applyChangedProps(button, { alignItems: 'flex-start' }, {});
        expect(bridge.calls.map((c) => [c.fn, ...c.args])).toEqual([
            ['resetStyleProperty', 'btn', 'align_items'],
            ['setFlex', 'btn', 'align_items', 'center'],
        ]);
    });

    it('lets a surviving author key beat a creation default', () => {
        const button = makeInstance('btn2', 'button', { alignItems: 'center' });
        applyChangedProps(button, { flexDirection: 'column', alignItems: 'flex-end' },
                          { alignItems: 'flex-end' });
        expect(callsTo('setFlex').some((a) => a[2] === 'center')).toBe(false);
    });

    it('releases an overlay claimed through role only when the role claimed it', () => {
        applyChangedProps(makeInstance('m'), { role: 'menu' }, {});
        expect(callsTo('releaseOverlay')).toEqual([['m']]);
        bridge.reset();
        applyChangedProps(makeInstance('b'), { role: 'button' }, {});
        expect(callsTo('releaseOverlay')).toEqual([]);
    });

    it('conditional-spread true → false: active-only background + border clear', () => {
        const baseProps = { padding: 8, borderRadius: 4 };
        const active = { ...baseProps, background: '#3a7', borderColor: '#fff' };
        const inactive = { ...baseProps };
        applyChangedProps(makeInstance('chip-3'), active, inactive);
        expect(callsTo('setBackground')).toEqual([['chip-3', '']]);
        expect(callsTo('setBorderWidth')).toEqual([['chip-3', 0]]);
        // borderRadius is unchanged across old/new → no setBorderRadius call.
        expect(callsTo('setBorderRadius')).toEqual([]);
    });

    it('does not clear when the key stays in newProps with a new value', () => {
        applyChangedProps(makeInstance('c8'), { background: 'red' }, { background: 'blue' });
        expect(callsTo('setBackground')).toEqual([['c8', 'blue']]);
        expect(callsTo('setBackgroundGradient')).toEqual([]);
    });

    it('keeps style-derived layout and type when a render drops its style object', () => {
        const oldN = normalizeHostProps('View', {
            style: { padding: 12, fontWeight: 'bold' }, opacity: 0.5,
        });
        applyChangedProps(makeInstance('d'), oldN, {});
        expect(callsTo('resetStyleProperty')).toEqual([]);
        expect(callsTo('setFontWeight')).toEqual([]);
        // The flat prop beside the style object still resets.
        expect(callsTo('setOpacity')).toEqual([['d', 1]]);
    });
});

// ── The bridge-call fixture behind the native pixel proof ───────────────
//
// test_widget_bridge_style_reset.cpp creates the widgets below, replays these
// calls through the real WidgetBridge and compares renders. Regenerate with
// PULP_UPDATE_STYLE_RESET_FIXTURE=1 after changing a reset; the comparison
// below fails until the checked-in file matches what the applier emits.

const FIXTURE = resolve(here, 'fixtures', 'style-reset-toggle.json');

const BOX_BASE = { width: 200, height: 96, background: '#20242c' };
const BOX_ACTIVE: Record<string, unknown> = {
    padding: 14,
    marginTop: 10,
    alignItems: 'center',
    border: '3px solid #f2f2f2',
    outline: '2px solid #ff00ff',
    boxShadow: '0px 4px 8px rgba(0,0,0,0.6)',
    transform: [{ scale: 1.25 }],
    opacity: 0.6,
};
const LABEL_BASE = { text: 'Preset', textColor: '#e0e0e0' };
const LABEL_ACTIVE: Record<string, unknown> = {
    fontWeight: 'bold',
    fontSize: 22,
    fontStyle: 'italic',
    letterSpacing: 3,
    textDecorationLine: 'underline',
    textTransform: 'uppercase',
};

type Call = [string, ...unknown[]];

function record(run: () => void): Call[] {
    bridge.reset();
    run();
    return bridge.calls.map((c) => [c.fn, ...c.args] as Call);
}

function mount(box: Record<string, unknown>, label: Record<string, unknown>): Call[] {
    return record(() => {
        applyAllProps({ ...makeInstance('box', 'View'), props: box });
        applyAllProps({ ...makeInstance('label', 'Label'), props: label });
    });
}

function deactivate(keepBox: string[], keepLabel: string[]): Call[] {
    const keep = (active: Record<string, unknown>, names: string[]) =>
        Object.fromEntries(names.map((k) => [k, active[k]]));
    return record(() => {
        applyChangedProps(makeInstance('box', 'View'), { ...BOX_BASE, ...BOX_ACTIVE },
                          { ...BOX_BASE, ...keep(BOX_ACTIVE, keepBox) });
        applyChangedProps(makeInstance('label', 'Label'), { ...LABEL_BASE, ...LABEL_ACTIVE },
                          { ...LABEL_BASE, ...keep(LABEL_ACTIVE, keepLabel) });
    });
}

function buildFixture() {
    const leaveOneActive: Record<string, Call[]> = {};
    for (const key of Object.keys(BOX_ACTIVE)) leaveOneActive[`box.${key}`] = deactivate([key], []);
    for (const key of Object.keys(LABEL_ACTIVE)) leaveOneActive[`label.${key}`] = deactivate([], [key]);
    return {
        generator: 'packages/pulp-react/test/prop-applier-disappearing-style.test.ts',
        widgets: [['createCol', 'box', ''], ['createLabel', 'label', 'Preset', 'box']] as Call[],
        neutral: mount(BOX_BASE, LABEL_BASE),
        active: mount({ ...BOX_BASE, ...BOX_ACTIVE }, { ...LABEL_BASE, ...LABEL_ACTIVE }),
        deactivate: deactivate([], []),
        leaveOneActive,
    };
}

/// JSON with one bridge call per line, so a diff shows which call changed.
function formatFixture(fixture: ReturnType<typeof buildFixture>): string {
    const calls = (list: Call[], indent: string) =>
        `[\n${list.map((c) => indent + '  ' + JSON.stringify(c)).join(',\n')}\n${indent}]`;
    const leave = Object.entries(fixture.leaveOneActive)
        .map(([name, list]) => `    ${JSON.stringify(name)}: ${calls(list, '    ')}`).join(',\n');
    return '{\n'
        + `  "generator": ${JSON.stringify(fixture.generator)},\n`
        + `  "widgets": ${calls(fixture.widgets, '  ')},\n`
        + `  "neutral": ${calls(fixture.neutral, '  ')},\n`
        + `  "active": ${calls(fixture.active, '  ')},\n`
        + `  "deactivate": ${calls(fixture.deactivate, '  ')},\n`
        + `  "leaveOneActive": {\n${leave}\n  }\n}\n`;
}

describe('@pulp/react prop-applier — style reset pixel-proof fixture', () => {
    it('matches what the applier emits today', () => {
        const fixture = buildFixture();
        expect(JSON.parse(formatFixture(fixture))).toEqual(fixture);
        const text = formatFixture(fixture);
        if (process.env.PULP_UPDATE_STYLE_RESET_FIXTURE === '1') writeFileSync(FIXTURE, text);
        expect(readFileSync(FIXTURE, 'utf8')).toBe(text);
    });

    it('deactivates every active key in the fixture', () => {
        const fixture = buildFixture();
        expect(fixture.deactivate.length).toBeGreaterThan(Object.keys(BOX_ACTIVE).length);
        // Each leave-one-active run keeps exactly one key, so it emits fewer
        // reset calls than the full deactivation.
        for (const [name, calls] of Object.entries(fixture.leaveOneActive)) {
            expect(calls.length, name).toBeLessThan(fixture.deactivate.length);
        }
    });
});
