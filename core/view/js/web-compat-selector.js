// web-compat-selector.js — CSS selector parsing + matching engine
//
// Extracted verbatim from the legacy web-compat.js bundle. Provides the
// `_parseSelector` / `_matchesSelector` / `_querySelector` / `_querySelectorAll`
// helpers consumed by Element.prototype.closest/matches/querySelector and by
// the StyleSheet rule engine. This bundle is not part of the runtime
// `PULP_JS_PRELUDES` chain (see core/view/CMakeLists.txt); it is read
// verbatim by the harness html adapter (tools/harness/adapters/html.py).

// ═══════════════════════════════════════════════════════════════════════════════
// Selector parsing and matching
// ═══════════════════════════════════════════════════════════════════════════════

function _parseSelector(str) {
    var result = { tag: null, id: null, classes: [], pseudo: null, pseudoArg: null,
                   notSelector: null, parent: null, direct: false, attrSelectors: [] };

    // Extract attribute selectors [attr], [attr="val"], [attr~="val"], [attr^="val"]
    str = str.replace(/\[([^\]]+)\]/g, function(_, inner) {
        var m = inner.match(/^([\w-]+)(?:([~|^$*]?)=["']?([^"'\]]*)["']?)?$/);
        if (m) result.attrSelectors.push({ name: m[1], op: m[2] || "", value: m[3] || "" });
        return "";
    });

    // Handle :not(selector) — extract inner selector
    var notMatch = str.match(/:not\(([^)]+)\)/);
    if (notMatch) {
        result.notSelector = _parseSelector(notMatch[1]);
        str = str.replace(/:not\([^)]+\)/, "");
    }

    // Split pseudo-class (but not :not which was already handled)
    var pseudoIdx = str.indexOf(":");
    var mainPart = str;
    if (pseudoIdx >= 0) {
        var pseudoStr = str.slice(pseudoIdx + 1);
        mainPart = str.slice(0, pseudoIdx);
        // Parse pseudo with optional argument: nth-child(2n+1)
        var pseudoArgMatch = pseudoStr.match(/^([\w-]+)\(([^)]*)\)/);
        if (pseudoArgMatch) {
            result.pseudo = pseudoArgMatch[1];
            result.pseudoArg = pseudoArgMatch[2];
        } else {
            result.pseudo = pseudoStr;
        }
    }

    // Check for descendant/child combinators
    if (mainPart.indexOf(" > ") >= 0) {
        var cp = mainPart.split(" > ");
        result.parent = _parseSelector(cp.slice(0, -1).join(" > "));
        result.direct = true;
        mainPart = cp[cp.length - 1].trim();
    } else if (mainPart.indexOf(" ") >= 0) {
        var sp = mainPart.split(/\s+/);
        result.parent = _parseSelector(sp.slice(0, -1).join(" "));
        result.direct = false;
        mainPart = sp[sp.length - 1].trim();
    }

    // Parse tag, id, classes from main part
    var parts = mainPart.match(/^([a-zA-Z][\w-]*)?([#.][^#.]+)*/);
    if (parts && parts[0]) {
        var tokens = mainPart.match(/([#.][a-zA-Z][\w-]*)|^([a-zA-Z][\w-]*)/g);
        if (tokens) {
            for (var i = 0; i < tokens.length; i++) {
                var t = tokens[i];
                if (t[0] === "#") result.id = t.slice(1);
                else if (t[0] === ".") result.classes.push(t.slice(1));
                else result.tag = t.toLowerCase();
            }
        }
    }

    return result;
}

// Get the index of an element among its parent's children (0-based)
function _childIndex(el) {
    if (!el._parentElement) return 0;
    var siblings = el._parentElement._children;
    for (var i = 0; i < siblings.length; i++) {
        if (siblings[i] === el) return i;
    }
    return 0;
}

// Parse An+B syntax: "odd" -> {a:2,b:1}, "even" -> {a:2,b:0}, "3" -> {a:0,b:3}, "2n+1" -> {a:2,b:1}
function _parseAnB(str) {
    str = str.trim().toLowerCase();
    if (str === "odd") return { a: 2, b: 1 };
    if (str === "even") return { a: 2, b: 0 };
    var m = str.match(/^(-?\d*)n\s*([+-]\s*\d+)?$/);
    if (m) {
        var a = m[1] === "" || m[1] === "+" ? 1 : m[1] === "-" ? -1 : parseInt(m[1]);
        var b = m[2] ? parseInt(m[2].replace(/\s/g, "")) : 0;
        return { a: a, b: b };
    }
    var n = parseInt(str);
    if (!isNaN(n)) return { a: 0, b: n };
    return { a: 0, b: 0 };
}

function _matchesNthChild(index1Based, anb) {
    if (anb.a === 0) return index1Based === anb.b;
    if (anb.a > 0) return (index1Based - anb.b) >= 0 && (index1Based - anb.b) % anb.a === 0;
    // Negative a: matches indices <= b
    return (index1Based - anb.b) <= 0 && (index1Based - anb.b) % anb.a === 0;
}

function _matchesSelector(el, parsed) {
    // Match tag
    if (parsed.tag && el.tagName.toLowerCase() !== parsed.tag) return false;

    // Match id
    if (parsed.id && el.getAttribute("id") !== parsed.id) return false;

    // Match classes
    for (var i = 0; i < parsed.classes.length; i++) {
        if (!el.classList.contains(parsed.classes[i])) return false;
    }

    // Match :not(selector)
    if (parsed.notSelector && _matchesSelector(el, parsed.notSelector)) return false;

    // Match pseudo-classes
    if (parsed.pseudo) {
        var p = parsed.pseudo;
        if (p === "first-child") {
            if (!el._parentElement || el._parentElement._children[0] !== el) return false;
        } else if (p === "last-child") {
            if (!el._parentElement) return false;
            var ch = el._parentElement._children;
            if (ch[ch.length - 1] !== el) return false;
        } else if (p === "nth-child") {
            var idx = _childIndex(el) + 1; // 1-based
            var anb = _parseAnB(parsed.pseudoArg || "0");
            if (!_matchesNthChild(idx, anb)) return false;
        } else if (p === "nth-last-child") {
            if (!el._parentElement) return false;
            var siblings = el._parentElement._children;
            var ridx = siblings.length - _childIndex(el); // 1-based from end
            var anb2 = _parseAnB(parsed.pseudoArg || "0");
            if (!_matchesNthChild(ridx, anb2)) return false;
        } else if (p === "only-child") {
            if (!el._parentElement || el._parentElement._children.length !== 1) return false;
        } else if (p === "empty") {
            if (el._children.length > 0 || (el._textContent && el._textContent.length > 0)) return false;
        } else if (p === "checked") {
            if (!el._checked) return false;
        } else if (p === "disabled") {
            if (!el._disabled) return false;
        } else if (p === "first-of-type") {
            if (!el._parentElement) return false;
            var tag = el.tagName;
            var fot = el._parentElement._children;
            var foundFirst = false;
            for (var fi = 0; fi < fot.length; fi++) {
                if (fot[fi].tagName === tag) { foundFirst = (fot[fi] === el); break; }
            }
            if (!foundFirst) return false;
        } else if (p === "last-of-type") {
            if (!el._parentElement) return false;
            var tag2 = el.tagName;
            var lot = el._parentElement._children;
            var foundLast = false;
            for (var li = lot.length - 1; li >= 0; li--) {
                if (lot[li].tagName === tag2) { foundLast = (lot[li] === el); break; }
            }
            if (!foundLast) return false;
        } else if (p === "nth-of-type") {
            if (!el._parentElement) return false;
            var tag3 = el.tagName;
            var notSiblings = el._parentElement._children;
            var typeIdx = 0;
            for (var ni = 0; ni < notSiblings.length; ni++) {
                if (notSiblings[ni].tagName === tag3) typeIdx++;
                if (notSiblings[ni] === el) break;
            }
            var anb3 = _parseAnB(parsed.pseudoArg || "0");
            if (!_matchesNthChild(typeIdx, anb3)) return false;
        } else if (p === "hover" || p === "focus" || p === "active") {
            // These are handled by StyleSheet pseudo-class registration, not static matching
        }
    }

    // Match attribute selectors
    if (parsed.attrSelectors) {
        for (var ai = 0; ai < parsed.attrSelectors.length; ai++) {
            var as = parsed.attrSelectors[ai];
            var attrVal = el.getAttribute(as.name);
            if (as.op === "" && as.value === "") {
                // [attr] — attribute exists
                if (attrVal === null) return false;
            } else if (as.op === "" || as.op === undefined) {
                // [attr="val"] — exact match
                if (attrVal !== as.value) return false;
            } else if (as.op === "~") {
                // [attr~="val"] — whitespace-separated word
                if (!attrVal || (" " + attrVal + " ").indexOf(" " + as.value + " ") < 0) return false;
            } else if (as.op === "^") {
                // [attr^="val"] — starts with
                if (!attrVal || attrVal.indexOf(as.value) !== 0) return false;
            } else if (as.op === "$") {
                // [attr$="val"] — ends with
                if (!attrVal || attrVal.indexOf(as.value, attrVal.length - as.value.length) < 0) return false;
            } else if (as.op === "*") {
                // [attr*="val"] — contains
                if (!attrVal || attrVal.indexOf(as.value) < 0) return false;
            }
        }
    }

    // Match parent constraint
    if (parsed.parent) {
        if (parsed.direct) {
            if (!el._parentElement || !_matchesSelector(el._parentElement, parsed.parent)) return false;
        } else {
            var ancestor = el._parentElement;
            var found = false;
            while (ancestor) {
                if (_matchesSelector(ancestor, parsed.parent)) { found = true; break; }
                ancestor = ancestor._parentElement;
            }
            if (!found) return false;
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// querySelector / querySelectorAll
// ═══════════════════════════════════════════════════════════════════════════════

function _querySelector(root, selector) {
    var parsed = _parseSelector(selector);
    return _findMatch(root, parsed, false);
}

function _querySelectorAll(root, selector) {
    var parsed = _parseSelector(selector);
    return _findMatch(root, parsed, true);
}

function _findMatch(root, parsed, findAll) {
    var results = [];
    var queue = root._children.slice();

    while (queue.length > 0) {
        var el = queue.shift();
        if (_matchesSelector(el, parsed)) {
            if (!findAll) return el;
            results.push(el);
        }
        for (var i = 0; i < el._children.length; i++) {
            queue.push(el._children[i]);
        }
    }

    return findAll ? results : null;
}

