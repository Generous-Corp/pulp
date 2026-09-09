// ═══════════════════════════════════════════════════════════════════════════════
// CSSStyleDeclaration
// ═══════════════════════════════════════════════════════════════════════════════

function CSSStyleDeclaration(el) {
    this._el = el;
    this._props = {};
    // auto-overlay heuristic state. Tracks whether
    // we've called `claimOverlay` for this element via the CSS-shape
    // detector so we can release exactly once on the inverse transition
    // (position -> static/relative, z-index -> below threshold,
    // data-overlay -> not "true"). The C++ release_overlay is idempotent
    // and the @pulp/react `overlay` prop path uses the same bridge calls,
    // so the two paths converge on `View::active_overlay_` without
    // double-claim/double-release surprises.
    this._autoOverlayClaimed = false;
    // Raw string last APPLIED per property, used to skip a write that would
    // reproduce the state the widget is already in. Keyed on the raw
    // (pre-var-resolution) string because that is what the caller supplies and
    // what deterministically produces the applied value for a given theme.
    // Only ever written after a property actually reaches the bridge, so a
    // value stored while the widget did not exist can never suppress a later
    // real apply.
    this._applied = {};
}

// z-index threshold above which an absolutely
// positioned element is treated as a popover/overlay candidate. Web
// authors typically use values like 1000 / 9999 for popovers and 1-3
// for stacking-context shuffles within layouts; 10 is comfortably
// above the in-flow stacking range and well below conventional
// popover values, so it is a conservative gate against false
// positives (decorative absolutely-positioned badges with z-index 1
// must NOT auto-claim because a claim hijacks click routing).
var _PULP_AUTO_OVERLAY_Z_INDEX_THRESHOLD = 10;

// re-evaluate the auto-overlay heuristic for
// this element. Called whenever `position`, `zIndex`, or the
// `data-overlay` hint changes. Conservative by design: opt-in only
// when the CSS shape strongly signals a popover (position:absolute +
// high z-index) OR the author explicitly hints `data-overlay="true"`.
// Mirrors what the @pulp/react prop-applier does for `<View overlay>`
// — both paths call `claimOverlay` / `releaseOverlay` on the same
// bridge so the single `View::active_overlay_` slot stays consistent.
CSSStyleDeclaration.prototype._reevaluateOverlay = function() {
    var el = this._el;
    if (!el || !el._nativeCreated) return;

    // 1. Explicit hint wins (HTML data-overlay="true" or CSS data-overlay
    //    style; both surface through Element._dataset.overlay).
    var hint = el._dataset && el._dataset.overlay;
    var hinted = (hint === "true" || hint === true);

    // 2. CSS shape: position:absolute + z-index above threshold. We
    //    require BOTH — `position:absolute` alone catches tooltips,
    //    decorations, and absolutely-laid-out cards that should NOT
    //    steal clicks. A high z-index alone (with position:relative or
    //    static) doesn't reorder hit-testing in the same popover sense.
    var posResolved = _resolveVar(String(this._props.position || ""));
    var zRaw = this._props.zIndex;
    var zResolved = (zRaw == null || zRaw === "") ? "" : _resolveVar(String(zRaw));
    var zVal = parseInt(zResolved, 10);
    if (isNaN(zVal)) zVal = 0;
    var shapeClaim = (posResolved === "absolute" &&
                      zVal >= _PULP_AUTO_OVERLAY_Z_INDEX_THRESHOLD);

    var shouldClaim = hinted || shapeClaim;

    if (shouldClaim && !this._autoOverlayClaimed) {
        if (typeof claimOverlay === "function") claimOverlay(el._id);
        this._autoOverlayClaimed = true;
    } else if (!shouldClaim && this._autoOverlayClaimed) {
        if (typeof releaseOverlay === "function") releaseOverlay(el._id);
        this._autoOverlayClaimed = false;
    }
};

// Drop the applied-value cache so the next write for each property reaches the
// bridge again. Required wherever widget state changes underneath a surviving
// Element: the native widget is recreated (the cache would describe a widget
// that no longer exists) or another code path writes a slot the cache claims.
CSSStyleDeclaration.prototype._invalidateApplied = function() {
    this._applied = {};
};

// Same, addressed by element, for the non-style call sites that mutate widget
// state directly (show/close/hidden/media-attribute replay). Tolerates an
// element with no style declaration yet.
function __invalidateStyleCache__(el) {
    if (el && el.style && el.style._applied) el.style._applied = {};
}

// Flush all stored properties to the bridge
CSSStyleDeclaration.prototype._flushAll = function() {
    for (var key in this._props) {
        this._applyProperty(key, this._props[key]);
    }
};

// Apply a single CSS property to the bridge.
//
// The former monolithic per-property `switch` is split into per-domain
// handler modules (web-compat-style-decl-layout / -paint /
// -typography / -transform / -misc), mirroring the @pulp/react
// prop-applier split. `_applyProperty` below is now a thin dispatcher
// that calls each `_applyXProp` handler in sequence until one claims
// the key. Every CSS property belongs to exactly one domain, so the
// call order does not change which handler runs — the behavior is
// byte-identical to the pre-split source-ordered switch. The handler
// functions are plain hoisted function declarations defined in the
// sibling preludes, which embed AFTER this file.
CSSStyleDeclaration.prototype._applyProperty = function(key, value) {
    var id = this._el._id;
    if (!this._el._nativeCreated) return;

    var raw = (value === null || value === undefined) ? "" : String(value);
    // typeof, not `!== undefined`: _DEDUP_GROUP is a plain object, so an
    // inherited Object.prototype name would otherwise read as a group index.
    var g = _DEDUP_GROUP[key];
    var group = (typeof g === "number") ? g : undefined;

    // Skip a write that reproduces the value already applied. Stylesheet
    // re-application rewrites every matched declaration on every pass, so
    // during an interaction the overwhelming majority of writes are this case;
    // each one otherwise pays var() resolution, per-property parsing, and a
    // bridge crossing to set a value the widget already holds.
    //
    // A var() value is never cached: it resolves against live theme tokens, so
    // the same raw string can legitimately produce a different applied value
    // after a token changes, with no write to observe.
    var cacheable = (group !== undefined && raw.indexOf("var(") < 0);
    if (cacheable && this._applied[key] === raw) return;

    var resolved = _resolveVar(raw);

    // Several CSS properties reach one piece of widget state -- visibility and
    // opacity both drive setOpacity, shorthands expand over their longhands --
    // so a cached value is only trustworthy while no property sharing its
    // state has been applied since. _DEDUP_GROUP names those sharers; it is
    // derived from the handlers' own bridge calls by
    // tools/scripts/style_dedup_table.py rather than maintained by hand, and a
    // handler that starts writing a different slot fails that checker rather
    // than silently rendering wrong. A property absent from the table (a
    // shorthand that expands over a computed edge, or anything the derivation
    // could not classify) is never cached and drops the whole cache when it
    // applies, so being missing costs speed and not correctness.
    if (group === undefined) {
        this._applied = {};
    } else {
        var members = _DEDUP_MEMBERS[group];
        for (var m = 0; m < members.length; m++) {
            if (members[m] !== key) delete this._applied[members[m]];
        }
        if (cacheable) this._applied[key] = raw;
        else delete this._applied[key];
    }

    // Try each domain handler in turn. Each returns true once it has
    // claimed (and applied) the key. Unknown keys fall through every
    // handler and are silently ignored — same as the pre-split switch
    // which had no `default` arm.
    if (_applyLayoutProp(this, id, key, resolved, value)) return;
    if (_applyPaintProp(this, id, key, resolved, value)) return;
    if (_applyTypographyProp(this, id, key, resolved, value)) return;
    if (_applyTransformProp(this, id, key, resolved, value)) return;
    if (_applyMiscProp(this, id, key, resolved, value)) return;
};

// Convert CSS flex alignment names to Pulp bridge names
