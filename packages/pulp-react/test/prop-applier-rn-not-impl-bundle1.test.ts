// React Native compatibility props covered by the @pulp/react prop applier:
//
//   1. boxSizing                 — routes to setBoxSizing.
//   2. direction                 — value-disambiguated. Writing-direction
//                                  keywords (ltr/rtl/inherit/auto) route
//                                  to setDirection; flexDirection
//                                  aliases ('row' / 'column' / etc.) keep
//                                  routing to setFlex(direction).
//   3. experimental_backgroundImage — RN gradient string aliased to
//                                     setBackgroundGradient.
//   4. fontVariant               — OpenType feature array -> CSV string,
//                                  forwarded to setFontVariant.
//   5. textDecorationLine        — aliased to setTextDecoration.
//   6. textShadowColor           — per-attribute shadow color slot.
//   7. textShadowOffset          — { width, height } → (dx, dy).
//   8. textShadowRadius          — px number forwarded.

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { applyChangedProps } from '../src/prop-applier.js';
import { createMockBridge, type MockBridge } from '../src/bridge.js';
import type { PulpInstance } from '../src/types.js';

let bridge: MockBridge;

beforeEach(() => {
    bridge = createMockBridge();
    bridge.install();
});
afterEach(() => {
    bridge.uninstall();
});

function makeInstance(id: string = 'k', type: string = 'View'): PulpInstance {
    return {
        id,
        type: type as PulpInstance['type'],
        props: {},
        childIds: [],
        onBridge: true,
        pendingChildren: [],
    };
}

function callOf(b: MockBridge, fn: string) {
    return b.calls.find((c) => c.fn === fn);
}

describe('React Native boxSizing forwarding', () => {
    it('boxSizing "border-box" routes to setBoxSizing', () => {
        applyChangedProps(makeInstance(), {}, { boxSizing: 'border-box' });
        expect(callOf(bridge, 'setBoxSizing')?.args).toEqual(['k', 'border-box']);
    });

    it('boxSizing "content-box" routes to setBoxSizing', () => {
        applyChangedProps(makeInstance(), {}, { boxSizing: 'content-box' });
        expect(callOf(bridge, 'setBoxSizing')?.args).toEqual(['k', 'content-box']);
    });
});

describe('React Native direction forwarding', () => {
    it('direction "ltr" routes to setDirection (writing direction)', () => {
        applyChangedProps(makeInstance(), {}, { direction: 'ltr' });
        expect(callOf(bridge, 'setDirection')?.args).toEqual(['k', 'ltr']);
        // Must NOT also route to setFlex(direction) for these keywords.
        expect(
            bridge.calls.find((c) => c.fn === 'setFlex' && c.args[1] === 'direction'),
        ).toBeUndefined();
    });

    it('direction "rtl" routes to setDirection', () => {
        applyChangedProps(makeInstance(), {}, { direction: 'rtl' });
        expect(callOf(bridge, 'setDirection')?.args).toEqual(['k', 'rtl']);
    });

    it('direction "inherit" routes to setDirection', () => {
        applyChangedProps(makeInstance(), {}, { direction: 'inherit' });
        expect(callOf(bridge, 'setDirection')?.args).toEqual(['k', 'inherit']);
    });

    it('direction "auto" routes to setDirection', () => {
        applyChangedProps(makeInstance(), {}, { direction: 'auto' });
        expect(callOf(bridge, 'setDirection')?.args).toEqual(['k', 'auto']);
    });

    it('direction "row" still routes to setFlex(direction) — backward compat with FlexProps alias', () => {
        // Value-disambiguation must preserve the existing FlexProps alias path.
        applyChangedProps(makeInstance(), {}, { direction: 'row' });
        expect(
            bridge.calls.find((c) => c.fn === 'setFlex' && c.args[1] === 'direction')?.args,
        ).toEqual(['k', 'direction', 'row']);
        expect(callOf(bridge, 'setDirection')).toBeUndefined();
    });

    it('direction "column-reverse" still routes to setFlex(direction)', () => {
        applyChangedProps(makeInstance(), {}, { direction: 'column-reverse' });
        expect(
            bridge.calls.find((c) => c.fn === 'setFlex' && c.args[1] === 'direction')?.args,
        ).toEqual(['k', 'direction', 'column-reverse']);
        expect(callOf(bridge, 'setDirection')).toBeUndefined();
    });
});

describe('React Native experimental_backgroundImage forwarding', () => {
    it('linear-gradient string forwards to setBackgroundGradient', () => {
        const grad = 'linear-gradient(to right, #ff0000, #0000ff)';
        applyChangedProps(makeInstance(), {}, { experimental_backgroundImage: grad });
        expect(callOf(bridge, 'setBackgroundGradient')?.args).toEqual(['k', grad]);
    });

    it('radial-gradient string forwards to setBackgroundGradient', () => {
        const grad = 'radial-gradient(circle, #fff, #000)';
        applyChangedProps(makeInstance(), {}, { experimental_backgroundImage: grad });
        expect(callOf(bridge, 'setBackgroundGradient')?.args).toEqual(['k', grad]);
    });
});

describe('React Native fontVariant forwarding', () => {
    it('array of OpenType feature tokens joins to CSV and forwards to setFontVariant', () => {
        applyChangedProps(makeInstance(), {}, {
            fontVariant: ['small-caps', 'tabular-nums'],
        });
        expect(callOf(bridge, 'setFontVariant')?.args).toEqual([
            'k',
            'small-caps,tabular-nums',
        ]);
    });

    it('single string value forwards verbatim', () => {
        applyChangedProps(makeInstance(), {}, { fontVariant: 'small-caps' });
        expect(callOf(bridge, 'setFontVariant')?.args).toEqual(['k', 'small-caps']);
    });
});

describe('React Native textDecorationLine forwarding', () => {
    it('"underline" routes to setTextDecoration', () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'underline' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'underline']);
    });

    it('"line-through" routes to setTextDecoration', () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'line-through' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'line-through']);
    });

    it('"none" routes to setTextDecoration', () => {
        applyChangedProps(makeInstance(), {}, { textDecorationLine: 'none' });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual(['k', 'none']);
    });

    it('compound "underline line-through" forwards verbatim (single-keyword honored, partial)', () => {
        applyChangedProps(makeInstance(), {}, {
            textDecorationLine: 'underline line-through',
        });
        expect(callOf(bridge, 'setTextDecoration')?.args).toEqual([
            'k',
            'underline line-through',
        ]);
    });
});

describe('React Native textShadow prop forwarding', () => {
    it('textShadowColor forwards a CSS-color string', () => {
        applyChangedProps(makeInstance(), {}, { textShadowColor: '#80808080' });
        expect(callOf(bridge, 'setTextShadowColor')?.args).toEqual(['k', '#80808080']);
    });

    it('textShadowOffset {width,height} forwards as (dx, dy) numbers', () => {
        applyChangedProps(makeInstance(), {}, {
            textShadowOffset: { width: 2, height: 4 },
        });
        expect(callOf(bridge, 'setTextShadowOffset')?.args).toEqual(['k', 2, 4]);
    });

    it('textShadowOffset coerces missing axes to 0', () => {
        applyChangedProps(makeInstance(), {}, {
            // RN allows omitting one axis; the prop-applier defaults the
            // missing axis to 0 instead of NaN at the bridge boundary.
            textShadowOffset: { width: 3 } as { width: number; height?: number },
        });
        expect(callOf(bridge, 'setTextShadowOffset')?.args).toEqual(['k', 3, 0]);
    });

    it('textShadowRadius forwards a numeric px value', () => {
        applyChangedProps(makeInstance(), {}, { textShadowRadius: 6 });
        expect(callOf(bridge, 'setTextShadowRadius')?.args).toEqual(['k', 6]);
    });

    it('all three textShadow* props can land in one prop diff (per-attribute slots preserve siblings)', () => {
        applyChangedProps(makeInstance(), {}, {
            textShadowColor: '#000',
            textShadowOffset: { width: 1, height: 1 },
            textShadowRadius: 4,
        });
        expect(callOf(bridge, 'setTextShadowColor')?.args).toEqual(['k', '#000']);
        expect(callOf(bridge, 'setTextShadowOffset')?.args).toEqual(['k', 1, 1]);
        expect(callOf(bridge, 'setTextShadowRadius')?.args).toEqual(['k', 4]);
    });
});
