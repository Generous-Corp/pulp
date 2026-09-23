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
    // Consume value last sent with that claim, so a change of it (the author
    // adding/removing data-overlay while the CSS shape already claimed)
    // re-claims instead of silently keeping the stale value.
    this._autoOverlayConsume = false;
    // Declared-parent widget id last sent with that claim, for the same
    // reason: an author who moves a lifted submenu from one menu to another
    // must re-claim against the new one rather than keep nesting on the old.
    this._autoOverlayParent = "";
    // Whether this element is currently marked as an overlay TRIGGER (the
    // control that opens a popover, not the popover). Tracked so the bridge
    // call happens only on a transition.
    this._autoOverlayTrigger = false;
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

// Resolve the overlay an author declared this one STACKS ON, as the widget id
// `claimOverlay` speaks, or "" when nothing was declared.
//
// `data-overlay-parent` is read first and names the parent by DOM id. An id
// that resolves to no element is forwarded verbatim so a caller holding a raw
// widget id can use the same attribute; a name that resolves to no widget
// either is treated natively as no declaration at all, so a typo or a stale id
// degrades to the ordinary claim rather than to a surprise.
//
// `aria-owns` states the same relationship from the other end, and is the
// attribute ARIA provides for exactly this case: a parent/child relationship
// the DOM hierarchy cannot represent. Reading it means a document that lifted a
// submenu out of its menu's subtree AND described that for assistive technology
// needs no Pulp-specific attribute. Gated on the element having an `id`,
// because that is what an owner can name -- so the reverse scan never runs for
// the overwhelming majority of overlays, which have no id at all.
function _resolveOverlayParent(el) {
    var declared = el._dataset ? el._dataset.overlayParent : null;
    if ((declared == null || declared === "") && el.getAttribute)
        declared = el.getAttribute("data-overlay-parent");
    if (declared != null && declared !== "") {
        var named = (typeof document !== "undefined" && document.getElementById)
            ? document.getElementById(String(declared)) : null;
        return (named && named !== el && named._id) ? named._id : String(declared);
    }

    var ownId = el.getAttribute ? el.getAttribute("id") : "";
    if (ownId == null || ownId === "") return "";
    if (typeof document === "undefined" || !document.querySelectorAll) return "";
    var owners = document.querySelectorAll("[aria-owns]") || [];
    for (var i = 0; i < owners.length; ++i) {
        var owner = owners[i];
        if (owner === el || !owner.getAttribute) continue;
        var owns = owner.getAttribute("aria-owns");
        if (owns == null || owns === "") continue;
        if (String(owns).trim().split(/\s+/).indexOf(ownId) < 0) continue;
        if (owner._id) return owner._id;
    }
    return "";
}

// re-evaluate the auto-overlay heuristic for
// this element. Called whenever `position`, `zIndex`, or the
// `data-overlay` hint changes. Conservative by design: opt-in only
// when the CSS shape strongly signals a popover (position:absolute +
// high z-index) OR the author explicitly hints `data-overlay="true"`.
// Shares the `claimOverlay` / `releaseOverlay` bridge with the
// @pulp/react prop-applier's `<View overlay>` prop, so the single
// `View::active_overlay_` slot stays consistent across both paths.
// The two do NOT agree on outside-click consumption, deliberately:
// see the `consume` note below.
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

    // 3. ARIA statement. `role="menu"|"listbox"|"tree"|"grid"|"dialog"|
    //    "alertdialog"` and `aria-modal="true"` say "I AM a dismissable
    //    overlay" in the vocabulary a document that cares about assistive
    //    technology has already written it in — the exact counterpart of the
    //    `aria-haspopup` trigger mark read below. A STATEMENT, not an
    //    inference, so it joins the explicit branch: it claims with the same
    //    outside-click consumption `data-overlay="true"` does, because an
    //    author who wrote role="menu" meant a menu, and a menu that lets the
    //    press through keeps painting on whatever the press just changed.
    if (!hinted && el.getAttribute) {
        var roleAttr = el.getAttribute("role");
        if (roleAttr != null) {
            var roleTok = String(roleAttr).toLowerCase();
            hinted = (roleTok === "menu" || roleTok === "listbox" ||
                      roleTok === "tree" || roleTok === "grid" ||
                      roleTok === "dialog" || roleTok === "alertdialog");
        }
        if (!hinted) {
            var modalAttr = el.getAttribute("aria-modal");
            hinted = (modalAttr != null &&
                      String(modalAttr).toLowerCase() === "true");
        }
    }

    var shouldClaim = hinted || shapeClaim;

    // `data-overlay-trigger="true"` marks a control that OPENS an overlay — a
    // dropdown field, a menu button. A press on one while a DIFFERENT overlay
    // is open means "switch menus", so the native dismissal policy delivers
    // that press to the trigger instead of spending it on the close, and the
    // user changes dropdowns in one tap. Never inferred from CSS shape: an
    // inference that marked ordinary content would make clicking away from a
    // menu also operate whatever sits under the click.
    var triggerHint = el._dataset && el._dataset.overlayTrigger;
    var isTrigger = (triggerHint === "true" || triggerHint === true);
    // `aria-haspopup` says the same thing in the vocabulary a document that
    // cares about assistive technology has already written it in, and it is
    // the exact counterpart of the ARIA the overlay side already reads:
    // `role="menu"|"listbox"|"dialog"` and `aria-modal` say "I AM a
    // dismissable overlay", `aria-haspopup` says "I OPEN one". Honouring only
    // half of that pair is what makes a correctly-authored app pay two presses
    // to switch menus. Still a STATEMENT rather than an inference — an author
    // writes it deliberately — so this stays inside the explicit branch and
    // never joins the CSS-shape heuristic. The ARIA token set is
    // true|menu|listbox|tree|grid|dialog; "false" and absent do not mark.
    if (!isTrigger && el.getAttribute) {
        var ariaPopup = el.getAttribute("aria-haspopup");
        if (ariaPopup != null) {
            var token = String(ariaPopup).toLowerCase();
            isTrigger = (token !== "" && token !== "false");
        }
    }
    if (this._autoOverlayTrigger !== isTrigger) {
        if (typeof setOverlayTrigger === "function")
            setOverlayTrigger(el._id, isTrigger);
        this._autoOverlayTrigger = isTrigger;
    }

    // The overlay this one STACKS ON, when the author declared it.
    //
    // `View::claim_overlay()` nests a claim only when it descends from the
    // open overlay, and a submenu placed to escape its menu's box does not: a
    // `position: fixed` panel is emitted as a SIBLING of the menu it belongs
    // to, so the parent-chain walk cannot see the relationship and the menu
    // underneath is dismissed as a rival, taking the submenu's own rows with
    // it. Neither half of the claim above can supply the missing fact --
    // `position: fixed` is what CAUSES the problem and `role="menu"` is true of
    // both panels -- so it has to be declared.
    //
    // Two spellings, both STATEMENTS an author writes deliberately:
    //
    //   * `data-overlay-parent="<id>"` on the submenu, Pulp's own vocabulary
    //     and the direct counterpart of `data-overlay` / `data-overlay-trigger`;
    //   * `aria-owns="<submenu id>"` on the MENU, which is precisely what ARIA
    //     provides for a parent/child relationship the DOM hierarchy cannot
    //     represent -- the same document that already earned the `role="menu"`
    //     claim by describing itself for assistive technology gets this for
    //     free.
    //
    // Deliberately never inferred. The CSS-shape branch does not supply a
    // parent and neither does a bare `role="menu"`, because an inferred parent
    // would be exactly the "nest on whatever happened to be open" bypass the
    // descendant rule exists to prevent.
    var overlayParentId = shouldClaim ? _resolveOverlayParent(el) : "";

    // Consume the dismissing press only on the EXPLICIT author opt-in.
    // `data-overlay="true"` is a direct statement that the element is a
    // popover — the same statement @pulp/react's `<View overlay>` prop makes,
    // and that path passes consume=true, so matching it here closes a fork
    // where identical intent behaved differently depending on which authoring
    // surface expressed it. The CSS-shape branch stays click-through because
    // it is an inference, not a statement: a false positive that consumed
    // would swallow a real click outright, whereas a false positive that
    // clicks through merely closes something that should not have claimed.
    var consume = hinted;

    if (shouldClaim && (!this._autoOverlayClaimed ||
                        this._autoOverlayConsume !== consume ||
                        this._autoOverlayParent !== overlayParentId)) {
        if (typeof claimOverlay === "function")
            claimOverlay(el._id, consume, overlayParentId);
        this._autoOverlayClaimed = true;
        this._autoOverlayConsume = consume;
        this._autoOverlayParent = overlayParentId;
    } else if (!shouldClaim && this._autoOverlayClaimed) {
        if (typeof releaseOverlay === "function") releaseOverlay(el._id);
        this._autoOverlayClaimed = false;
        this._autoOverlayConsume = false;
        this._autoOverlayParent = "";
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
