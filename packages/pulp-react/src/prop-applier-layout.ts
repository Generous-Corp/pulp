// prop-applier-layout — Yoga flex / grid / box-model / positioning
// props (P5-NEW-A split of the former monolithic applyOne switch).
//
// `applyLayoutProp(id, key, value, props)` returns true if it handled
// the key, false otherwise. Behavior is byte-identical to the matching
// cases in the pre-split prop-applier switch — same bridge calls in the
// same order.

import { call } from './prop-applier-internal.js';

// Wave 2 rn — coerce a single CSS-length-like token to the value shape
// the bridge's per-edge setFlex keys expect. Numbers pass through
// numerically; percent strings (`'5%'`) pass through verbatim (the
// bridge detects the suffix and routes to Yoga's percent path); plain
// numeric strings become numbers; everything else falls back to 0.
// Used by the padding shorthand fan-out where the bridge's `padding`
// shorthand key only accepts a number.
function _coerceLen(tok: string | number): number | string {
    if (typeof tok === 'number') return tok;
    const s = String(tok).trim();
    if (s.endsWith('%')) return s; // forwarded verbatim → bridge percent path
    const n = parseFloat(s);
    return Number.isFinite(n) ? n : 0;
}

// Wave 2 rn — like `_coerceLen` but ALSO honors the `'auto'` keyword
// (Yoga's YGNodeStyleSetMarginAuto — used for centering with
// `marginLeft: 'auto'` + `marginRight: 'auto'`). Padding has no auto
// equivalent in Yoga, so the regular `_coerceLen` is used there.
function _coerceMarginLen(tok: string | number): number | string {
    if (typeof tok === 'number') return tok;
    const s = String(tok).trim();
    if (s === 'auto') return 'auto';
    if (s.endsWith('%')) return s;
    const n = parseFloat(s);
    return Number.isFinite(n) ? n : 0;
}

/// Apply a layout/flex/grid/positioning prop. Returns true if handled.
export function applyLayoutProp(
    id: string,
    key: string,
    value: unknown,
    props?: Record<string, unknown>,
): boolean {
    switch (key) {
        // Flex / layout — all forwarded through setFlex
        // pulp #1434 (rn NOT-IMPL bundle 1) — `direction` is overloaded:
        //   • RN (and CSS spec) sense — writing direction: 'ltr' / 'rtl' /
        //     'inherit' (RN spec also accepts 'auto' on iOS-classic). The
        //     New Architecture surfaces this cross-platform.
        //   • pulp historical sense — flexDirection alias: 'row' / 'col' /
        //     'row-reverse' / 'column' / 'column-reverse'. Existing test
        //     at prop-applier-direction.test.ts:60 pins this behavior.
        // Disambiguate on value: writing-direction keywords route to
        // setDirection; everything else falls through to setFlex(direction)
        // for backward compat. `writingDirection` is preferred for new code
        // (case below) but RN code commonly emits `direction: 'rtl'`.
        case 'direction': {
            const sval = String(value).trim().toLowerCase();
            if (sval === 'ltr' || sval === 'rtl' || sval === 'inherit' || sval === 'auto') {
                call('setDirection', id, sval);
                return true;
            }
            call('setFlex', id, 'direction', value as string);
            return true;
        }
        // pulp #108 — RN/React-style `flexDirection` (camelCase) is the
        // canonical key in JSX. Without this case the prop fell through
        // as unknown, leaving Yoga's column default in place and
        // collapsing CSS-imported flex rows into vertical stacks. Maps
        // to the same setFlex(direction, …) dispatch as the `direction`
        // case for the flex-direction subset of values. Normalizes
        // `col` / `col-reverse` aliases to `column` / `column-reverse`
        // for the bridge's expected vocabulary.
        case 'flexDirection': {
            const sval = String(value).trim().toLowerCase();
            const normalized = sval === 'col' ? 'column'
                : sval === 'col-reverse' ? 'column-reverse'
                : sval;
            call('setFlex', id, 'direction', normalized);
            return true;
        }
        case 'gap':             call('setFlex', id, 'gap', value as number); return true;
        case 'rowGap':          call('setFlex', id, 'row_gap', value as number); return true;
        case 'columnGap':       call('setFlex', id, 'column_gap', value as number); return true;
        // Wave 2 rn — `padding` shorthand accepts string forms (`'5%'`,
        // `'10px 20px'`, etc.). The bridge `padding` shorthand key only
        // takes a numeric value, so we fan out string values to the
        // four per-edge keys (which DO accept `number | string` via
        // setFlex(padding_top/...) and route through Yoga's
        // YGNodeStyleSetPaddingPercent for `'5%'`). Numeric values
        // continue to flow through the original shorthand path so we
        // preserve the single-bridge-call shape for the common case.
        case 'padding': {
            if (typeof value === 'string') {
                const tokens = value.trim().split(/\s+/);
                const t = _coerceLen(tokens[0] ?? 0);
                const r = _coerceLen(tokens[1] ?? tokens[0] ?? 0);
                const b = _coerceLen(tokens[2] ?? tokens[0] ?? 0);
                const l = _coerceLen(tokens[3] ?? tokens[1] ?? tokens[0] ?? 0);
                call('setFlex', id, 'padding_top',    t);
                call('setFlex', id, 'padding_right',  r);
                call('setFlex', id, 'padding_bottom', b);
                call('setFlex', id, 'padding_left',   l);
                return true;
            }
            call('setFlex', id, 'padding', value as number);
            return true;
        }
        // pulp #1434 (cross-surface mega-batch) — per-edge padding accepts
        // either a number (px) or a percent string ('5%' → percent of
        // parent main-axis size). Yoga padding does NOT support 'auto'.
        case 'paddingTop':      call('setFlex', id, 'padding_top',    value as number | string); return true;
        case 'paddingRight':    call('setFlex', id, 'padding_right',  value as number | string); return true;
        case 'paddingBottom':   call('setFlex', id, 'padding_bottom', value as number | string); return true;
        case 'paddingLeft':     call('setFlex', id, 'padding_left',   value as number | string); return true;
        // Wave 2 rn — `margin` shorthand accepts string forms (`'5%'`,
        // `'auto'`, `'10px auto'`, etc.). The bridge `margin` shorthand
        // key only takes a numeric value, so we fan out string values
        // to the four per-edge keys (which DO accept `number | string`
        // including the `'auto'` keyword via Yoga's
        // YGNodeStyleSetMarginAuto for centering). Numeric values
        // continue through the original shorthand path so the common
        // single-call case is preserved.
        case 'margin': {
            if (typeof value === 'string') {
                const tokens = value.trim().split(/\s+/);
                const t = _coerceMarginLen(tokens[0] ?? 0);
                const r = _coerceMarginLen(tokens[1] ?? tokens[0] ?? 0);
                const b = _coerceMarginLen(tokens[2] ?? tokens[0] ?? 0);
                const l = _coerceMarginLen(tokens[3] ?? tokens[1] ?? tokens[0] ?? 0);
                call('setFlex', id, 'margin_top',    t);
                call('setFlex', id, 'margin_right',  r);
                call('setFlex', id, 'margin_bottom', b);
                call('setFlex', id, 'margin_left',   l);
                return true;
            }
            call('setFlex', id, 'margin', value as number);
            return true;
        }
        // pulp #1434 (cross-surface mega-batch) — per-edge margin accepts
        // a number (px), percent string ('5%'), or the keyword 'auto'
        // (Yoga's YGNodeStyleSetMarginAuto — used for centering with
        // marginLeft:'auto' + marginRight:'auto').
        case 'marginTop':       call('setFlex', id, 'margin_top',    value as number | string); return true;
        case 'marginRight':     call('setFlex', id, 'margin_right',  value as number | string); return true;
        case 'marginBottom':    call('setFlex', id, 'margin_bottom', value as number | string); return true;
        case 'marginLeft':      call('setFlex', id, 'margin_left',   value as number | string); return true;
        // pulp #1434 batch 4 — React Native shorthand aliases. RN code
        // commonly writes `style={{ marginHorizontal: 8 }}` and expects
        // it to fan out to marginLeft + marginRight on the underlying
        // layout. Same pattern for marginVertical / paddingHorizontal /
        // paddingVertical. We dispatch to the existing per-edge bridge
        // setters so the value reaches the same FlexStyle slot whether
        // it arrived through this alias or through the explicit edge
        // prop. No FlexStyle field change required.
        // pulp #1434 cross-surface mega-batch — RN aliases now forward
        // percent strings (and 'auto' for margin) through the per-edge
        // fan-out. The per-edge keys `margin_*` / `padding_*` accept
        // number | string at the bridge boundary.
        case 'marginHorizontal':
            call('setFlex', id, 'margin_left',  value as number | string);
            call('setFlex', id, 'margin_right', value as number | string);
            return true;
        case 'marginVertical':
            call('setFlex', id, 'margin_top',    value as number | string);
            call('setFlex', id, 'margin_bottom', value as number | string);
            return true;
        case 'paddingHorizontal':
            call('setFlex', id, 'padding_left',  value as number | string);
            call('setFlex', id, 'padding_right', value as number | string);
            return true;
        case 'paddingVertical':
            call('setFlex', id, 'padding_top',    value as number | string);
            call('setFlex', id, 'padding_bottom', value as number | string);
            return true;
        case 'flexGrow':        call('setFlex', id, 'flex_grow', value as number); return true;
        case 'flexShrink':      call('setFlex', id, 'flex_shrink', value as number); return true;
        // pulp #1434 (#1518) — RN-style `flex: <number>` shorthand.
        // RN spec: `flex: positive` → `{flexGrow: n, flexShrink: 1, flexBasis: 0}`;
        // `flex: 0` → no growth / no shrink at intrinsic basis;
        // `flex: -1` (or any negative) → no growth, can shrink at auto basis.
        // CSS spec is more nuanced (bare number is `flex: <n> 1 0`), but RN's
        // narrow contract is what consumers passing JSX `flex={1}` expect;
        // our adapter is RN-flavored so we honor RN semantics.
        case 'flex': {
            const n = value as number;
            if (typeof n !== 'number' || !Number.isFinite(n)) return true;
            if (n > 0) {
                call('setFlex', id, 'flex_grow', n);
                call('setFlex', id, 'flex_shrink', 1);
                call('setFlex', id, 'flex_basis', 0);
            } else if (n === 0) {
                call('setFlex', id, 'flex_grow', 0);
                call('setFlex', id, 'flex_shrink', 0);
                call('setFlex', id, 'flex_basis', 'auto');
            } else {
                call('setFlex', id, 'flex_grow', 0);
                call('setFlex', id, 'flex_shrink', 1);
                call('setFlex', id, 'flex_basis', 'auto');
            }
            return true;
        }
        // pulp #1434 (rn batch C) — dimension keys forward
        // `number | string` so the bridge sees `'50%'` / `'auto'`
        // verbatim. Numeric values still flow through unchanged.
        // The bridge's setFlex case for each key inspects the third
        // arg as a string and detects '%' / 'auto' suffix; otherwise
        // it falls back to the numeric path.
        case 'flexBasis':       call('setFlex', id, 'flex_basis', value as number | string); return true;
        // pulp #1434 Triage #14 — flexWrap accepts boolean (legacy
        // true/false) or the CSS keyword strings (`"wrap"` /
        // `"wrap-reverse"` / `"nowrap"`). Forward strings verbatim
        // so the bridge can route wrap-reverse through Yoga's
        // YGWrapWrapReverse.
        case 'flexWrap': {
            if (typeof value === 'string') {
                call('setFlex', id, 'flex_wrap', value);
                return true;
            }
            call('setFlex', id, 'flex_wrap', value ? 1 : 0);
            return true;
        }
        case 'order':           call('setFlex', id, 'order', value as number); return true;
        case 'width':           call('setFlex', id, 'width', value as number | string); return true;
        case 'height':          call('setFlex', id, 'height', value as number | string); return true;
        case 'minWidth':        call('setFlex', id, 'min_width', value as number | string); return true;
        case 'minHeight':       call('setFlex', id, 'min_height', value as number | string); return true;
        case 'maxWidth':        call('setFlex', id, 'max_width', value as number | string); return true;
        case 'maxHeight':       call('setFlex', id, 'max_height', value as number | string); return true;
        case 'alignItems':      call('setFlex', id, 'align_items', value as string); return true;
        case 'alignSelf':       call('setFlex', id, 'align_self', value as string); return true;
        // pulp #1434 (sub-agent #12 follow-up) — multi-line flex
        // cross-axis distribution. Yoga supports it natively via
        // YGNodeStyleSetAlignContent; the bridge accepts both bare
        // (`start`/`end`) and prefixed (`flex-start`/`flex-end`)
        // spellings plus the space-* values.
        case 'alignContent':    call('setFlex', id, 'align_content', value as string); return true;
        case 'justifyContent':  call('setFlex', id, 'justify_content', value as string); return true;
        // pulp #1434 — aspectRatio routes through setFlex like the other
        // flex props. Accepts a finite positive number (RN-style); strings
        // ("16/9", "auto") are NOT accepted at the JSX surface — those
        // belong to the CSS shim path (web-compat-style-decl.js). A value
        // of 0 / NaN / undefined clears the slot on the bridge side.
        case 'aspectRatio':     call('setFlex', id, 'aspect_ratio', value as number); return true;

        // pulp #1434 (rn batch — Triage #12) — `display: 'flex' | 'none'`.
        // RN exports + Figma / v0 / Claude Design HTML routinely emit
        // `style={{ display: 'flex' }}` (the implicit default in pulp,
        // but the prop-applier shouldn't drop it as unknown) or
        // `style={{ display: 'none' }}` to hide a subtree. The yoga
        // surface wired this for the CSS shim in #1422; this branch
        // makes the same path reachable from RN-flavored JSX without a
        // round-trip through the el.style proxy.
        //
        // 'none'  → setVisible(id, false). View::visible() is the
        //           canonical "skip render + don't lay out" signal.
        // 'flex'  → setVisible(id, true). Yoga's flex layout is pulp's
        //           default; explicit 'flex' just confirms it.
        // Anything else (block / inline-block / inline-flex / grid)
        // is silently ignored at this layer — the CSS shim handles
        // those for the el.style path; for RN consumers, the typical
        // emission is just 'flex' / 'none'.
        case 'display': {
            const sval = String(value);
            if (sval === 'none') { call('setVisible', id, false); return true; }
            if (sval === 'flex') {
                call('setVisible', id, true);
                // pulp #1894 — CSS web-compat: `display: flex` defaults to
                // `flex-direction: row`. Pulp's Yoga config defaults to
                // FlexDirection::column (RN convention), so when neither
                // flexDirection nor flexFlow-with-direction is also being
                // set in the same prop batch we must explicitly emit row.
                // Mirrors the CSS shim path at web-compat-style-decl.js
                // (display:flex handler). Without this fallback the
                // flat-prop path used by `style={{ display: 'flex' }}`
                // JSX silently collapses every flex container to a
                // vertical stack on import — first seen in Spectr's
                // editor toolbar post-#1859 re-validation.
                if (props) {
                    // pulp #1898 (Codex review P2) — `direction` is a
                    // third flex-direction alias in this prop-applier
                    // (see the `case 'direction'` block above: a value
                    // of 'row' / 'column' / 'row-reverse' / 'column-reverse'
                    // routes to setFlex(direction); writing-direction
                    // keywords like 'ltr' / 'rtl' / 'inherit' / 'auto'
                    // route to setDirection instead). The default-row
                    // suppression must therefore also honor a caller-
                    // supplied `direction: 'column'`, otherwise the
                    // explicit column is overwritten by the default row
                    // when both `display: 'flex'` and `direction:` land
                    // in the same prop batch. Restrict the check to
                    // flex-axis values so `direction: 'rtl'` (writing
                    // direction, not flex) still picks up the default.
                    const hasDirectionFlexValue = (() => {
                        if (!Object.prototype.hasOwnProperty.call(props, 'direction')) return false;
                        const dv = (props as Record<string, unknown>)['direction'];
                        if (typeof dv !== 'string') return false;
                        const norm = dv.trim().toLowerCase();
                        return norm === 'row' || norm === 'column'
                            || norm === 'row-reverse' || norm === 'column-reverse'
                            || norm === 'col' || norm === 'col-reverse';
                    })();
                    const hasFlexDirection =
                        Object.prototype.hasOwnProperty.call(props, 'flexDirection') ||
                        Object.prototype.hasOwnProperty.call(props, 'flex-direction') ||
                        hasDirectionFlexValue;
                    const ff = props.flexFlow;
                    const flexFlowHasDirection =
                        typeof ff === 'string' && /\b(row|column)\b/.test(ff);
                    if (!hasFlexDirection && !flexFlowHasDirection) {
                        call('setFlex', id, 'direction', 'row');
                    }
                }
                return true;
            }
            return true; // unknown display value — leave View at current visibility
        }

        // pulp #1387 gap #1 — overflow was reachable via the DOM-lite
        // path (web-compat-style-decl.js routes 'overflow' to setOverflow)
        // but missing from the @pulp/react prop-applier, so JSX consumers
        // setting `style={{ overflow: 'hidden' }}` silently dropped it.
        // Spectr's dropdowns hit this — `width: 230 + overflow: hidden`
        // on the dropdown row was being discarded, so the row grew to
        // intrinsic content width and overflowed the container.
        // Accepts the CSS keyword strings ('hidden' / 'visible' /
        // 'scroll' / 'auto'); bridge maps to View::Overflow enum.
        case 'overflow':     call('setOverflow', id, value as string); return true;
        // CSS per-axis overflow. Pulp's View::Overflow is a single-axis
        // paint-clip flag (same surface as `overflow`); both axes alias
        // to the same setter. Already-supported in web-compat-style-decl.js
        // via the same routing — wire the React-applier path so JSX
        // `style={{ overflowY: 'scroll' }}` (common in scroll containers)
        // doesn't silently drop.
        case 'overflowX':
        case 'overflowY':    call('setOverflow', id, value as string); return true;

        // pulp #1516 — CSS box-sizing. Yoga 3.x honors the spec via
        // YGNodeStyleSetBoxSizing; consumers passing JSX
        // `boxSizing: 'border-box'` get the standard
        // "padding+border are inside declared dimensions" behavior.
        // Web designs almost universally reset to `border-box`.
        case 'boxSizing':    call('setBoxSizing', id, value as string); return true;

        // pulp #1434 rn logical-edge bundle (sub-agent #27 finding) —
        // RN's CSS-spec-equivalent logical-flow props. LTR-only fast
        // path: Start → Left, End → Right, inset shorthand expands to
        // top/right/bottom/left, insetBlock → top+bottom,
        // insetInline → left+right (LTR). True RTL bidi requires a
        // future direction system — tracked as a separate big project.
        // The 11 entries here close the missing-on-rn gap with the
        // honest LTR-only caveat documented in the catalog.
        case 'marginStart': {
            call('setFlex', id, 'margin_left', value as number | string);
            return true;
        }
        case 'marginEnd': {
            call('setFlex', id, 'margin_right', value as number | string);
            return true;
        }
        case 'paddingStart': {
            call('setFlex', id, 'padding_left', value as number | string);
            return true;
        }
        case 'paddingEnd': {
            call('setFlex', id, 'padding_right', value as number | string);
            return true;
        }
        case 'start': {
            call('setLeft', id, value as number | string);
            return true;
        }
        case 'end': {
            call('setRight', id, value as number | string);
            return true;
        }
        // CSS `inset` shorthand: 1 / 2 / 3 / 4 values fan out to
        // top / right / bottom / left (CSS spec — same expansion as
        // `margin` / `padding` shorthands). Numeric or percent strings.
        case 'inset': {
            const v = value as number | string;
            if (typeof v === 'number') {
                call('setTop',    id, v);
                call('setRight',  id, v);
                call('setBottom', id, v);
                call('setLeft',   id, v);
                return true;
            }
            const tokens = String(v).trim().split(/\s+/);
            const t = tokens[0] ?? 0;
            const r = tokens[1] ?? t;
            const b = tokens[2] ?? t;
            const l = tokens[3] ?? r;
            // Each token may be a number or a percent string — forward
            // verbatim so the bridge can route through Yoga's
            // YGNodeStyleSetPositionPercent path for percent values.
            const coerce = (tok: string | number) => {
                if (typeof tok === 'number') return tok;
                if (tok.endsWith('%')) return tok;
                const n = parseFloat(tok);
                return Number.isFinite(n) ? n : tok;
            };
            call('setTop',    id, coerce(t));
            call('setRight',  id, coerce(r));
            call('setBottom', id, coerce(b));
            call('setLeft',   id, coerce(l));
            return true;
        }
        case 'insetBlock': {
            // CSS insetBlock shorthand → top + bottom.
            const v = value as number | string;
            call('setTop',    id, v);
            call('setBottom', id, v);
            return true;
        }
        case 'insetInline': {
            // CSS insetInline shorthand → left + right (LTR).
            const v = value as number | string;
            call('setLeft',  id, v);
            call('setRight', id, v);
            return true;
        }

        // pulp #1434 Phase A2-2 — CSS Grid surface. Forwards each
        // property verbatim to setGrid; the C++ bridge handles
        // template-track parsing, named-area parsing, and the
        // grid-area shorthand (named token vs `row / col / row / col`
        // numeric form).
        case 'gridTemplateColumns': call('setGrid', id, 'template_columns', value as string); return true;
        case 'gridTemplateRows':    call('setGrid', id, 'template_rows',    value as string); return true;
        case 'gridTemplateAreas':   call('setGrid', id, 'template_areas',   value as string); return true;
        case 'gridAutoColumns':     call('setGrid', id, 'auto_columns',     value as string); return true;
        case 'gridAutoRows':        call('setGrid', id, 'auto_rows',        value as string); return true;
        case 'gridAutoFlow':        call('setGrid', id, 'auto_flow',        value as string); return true;
        case 'gridArea':            call('setGrid', id, 'grid_area',        value as string); return true;
        case 'gridColumn': {
            const parts = String(value).split('/').map((s) => parseInt(s.trim(), 10));
            if (parts[0]) call('setGrid', id, 'column_start', parts[0]);
            if (parts[1]) call('setGrid', id, 'column_end', parts[1]);
            return true;
        }
        case 'gridRow': {
            const parts = String(value).split('/').map((s) => parseInt(s.trim(), 10));
            if (parts[0]) call('setGrid', id, 'row_start', parts[0]);
            if (parts[1]) call('setGrid', id, 'row_end', parts[1]);
            return true;
        }
        case 'gridColumnStart': call('setGrid', id, 'column_start', value as number); return true;
        case 'gridColumnEnd':   call('setGrid', id, 'column_end',   value as number); return true;
        case 'gridRowStart':    call('setGrid', id, 'row_start',    value as number); return true;
        case 'gridRowEnd':      call('setGrid', id, 'row_end',      value as number); return true;
        case 'gridGap':         call('setGrid', id, 'gap',          value as number); return true;
        case 'gridColumnGap':   call('setGrid', id, 'column_gap',   value as number); return true;
        case 'gridRowGap':      call('setGrid', id, 'row_gap',      value as number); return true;

        // CSS-style positioning (pulp #779 follow-up; matches setPosition
        // + setTop/setLeft/setRight/setBottom on the bridge).
        // pulp #1434 batch 6 — top/right/bottom/left accept either a number
        // ('50' → px) or a percent string ('50%' → percent of parent).
        // Mirrors PR #1426 (width/height percent) for the four View
        // positional fields. Figma absolute-positioned overlays, v0.dev
        // hero anchors, and Claude Design sticky elements all emit
        // `top:'50%'` etc. routinely; without percent forwarding the
        // layout collapses to numeric 0 silently. The bridge inspects
        // arg index 1 as a string, detects the '%' suffix, and routes to
        // Yoga's YGNodeStyleSetPositionPercent path via View::top_unit_.
        case 'position':     call('setPosition', id, value as string); return true;
        case 'top':          call('setTop', id, value as number | string); return true;
        case 'left':         call('setLeft', id, value as number | string); return true;
        case 'right':        call('setRight', id, value as number | string); return true;
        case 'bottom':       call('setBottom', id, value as number | string); return true;
        case 'zIndex':       call('setZIndex', id, value as number); return true;

        default:
            return false;
    }
}
