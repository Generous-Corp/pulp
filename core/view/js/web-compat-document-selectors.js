// ═══════════════════════════════════════════════════════════════════════════════
// CSS selector engine (P5-7 first cut — extracted from web-compat-document.js)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Per the U-2 export map, web-compat-document.js (1,820 lines) splits
// naturally into four concerns:
//   1. StyleSheet + style application (stays in parent)
//   2. CSS parser (~150 lines, stays in parent for now)
//   3. **Selector engine** (this file)
//   4. getComputedStyle + global install (stays in parent)
//
// This file owns the minimal CSS selector parser + matcher used by
// document.querySelector / document.querySelectorAll:
//   - _parseSelector(str) — tokenizer producing { tag, id, classes,
//     attrs, pseudo, parent, direct } records
//   - _matchesSelector(el, parsed) — walks an Element against a parsed
//     selector, honoring tag / id / class / attribute / pseudo /
//     descendant + direct-child combinators
//   - _matchesPseudoClass(el, pseudo) — :hover, :focus, :active,
//     :nth-child, :first-child, :last-child, :not, :is, :where
//   - _matchesNth(pos, arg) — :nth-child(an+b) parser
//   - _querySelector / _querySelectorAll — top-level entry points
//   - _findMatch(root, parsed, findAll) — DOM walker shared by both
//
// Embed order: loaded AFTER web-compat-document.js so the Element +
// document objects are in scope when document.querySelector
// (defined inline on `document`) dispatches into the underscore-
// prefixed helpers.

// ═══════════════════════════════════════════════════════════════════════════════
// Selector parsing and matching
// ═══════════════════════════════════════════════════════════════════════════════

// pulp Wave 3 html.3 — minimal CSS selector parser.
//
// Supports the subset that React / Three.js helper code actually uses
// in practice:
//   tag           — element.tagName.toLowerCase() === tag
//   #id           — element.id === id
//   .class        — element.classList.contains(class)
//   [attr]        — element.hasAttribute(attr)
//   [attr=value]  — element.getAttribute(attr) === value
//   [attr^=v]     — startsWith
//   [attr$=v]     — endsWith
//   [attr*=v]     — contains
//   [attr|=v]     — value or value-prefixed-by-hyphen
//   [attr~=v]     — token-match in whitespace-separated list
//   compound      — tag.class, tag#id, tag[attr], #id.class.class[attr]
//   descendant    — `a b`     (ancestor anywhere in the chain)
//   child         — `a > b`   (immediate parent only)
//
// Pseudo-classes (`:hover`, `:nth-child`, etc.) are recognised by the
// tokenizer for forward-compat (so they don't blow up the parser) but
// are stored unmatched — the gotcha string in compat.json keeps them
// listed as unsupported.  Sibling combinators (`+`, `~`) are not
// implemented; selectors that include them silently fall through to
// no-match (legacy behaviour).
function _parseSelector(str) {
    var result = { tag: null, id: null, classes: [], attrs: [], pseudo: null,
                   parent: null, direct: false };

    if (!str) return result;
    str = String(str);

    // Strip trailing pseudo-class (`:hover`, `:nth-child(2n)`, …) — pulp
    // doesn't implement them, but we tolerate them at the tokenizer
    // level so a real-world selector like `div.foo:hover` still
    // matches `div.foo` instead of refusing to parse.
    //
    // pulp #1641 followup: scan for `:` at bracket depth 0 only, so
    // colons inside attribute selectors like `[href="http://x"]` or
    // `[data-time="12:30"]` aren't misinterpreted as pseudo-class
    // boundaries (which would truncate the selector mid-bracket).
    var pseudoIdx = -1;
    var pdepth = 0;
    for (var pi = 0; pi < str.length; pi++) {
        var pc = str[pi];
        if (pc === '[') pdepth++;
        else if (pc === ']') pdepth--;
        else if (pdepth === 0 && pc === ':' && (pi === 0 || str[pi - 1] !== '\\')) {
            pseudoIdx = pi; break;
        }
    }
    var mainPart = str;
    if (pseudoIdx >= 0) {
        result.pseudo = str.slice(pseudoIdx + 1);
        mainPart = str.slice(0, pseudoIdx);
    }
    mainPart = mainPart.trim();

    // Check for child / descendant combinators on the OUTERMOST level
    // (combinators inside `[attr=" > "]` are protected by the bracket
    // check below since we scan left-to-right respecting brackets).
    var splitIdx = -1;
    var splitDirect = false;
    var depth = 0;
    for (var ci = mainPart.length - 1; ci >= 0; ci--) {
        var ch = mainPart[ci];
        if (ch === "]") depth++;
        else if (ch === "[") depth--;
        else if (depth === 0 && ch === ">") { splitIdx = ci; splitDirect = true; break; }
        else if (depth === 0 && ch === " " && ci < mainPart.length - 1 &&
                 mainPart[ci + 1] !== ">" && (ci === 0 || mainPart[ci - 1] !== ">")) {
            splitIdx = ci; splitDirect = false;
            // Don't break — keep walking left to find the *rightmost*
            // descendant boundary so the parent selector accumulates
            // correctly for `a b c` -> parent=`a b`, child=`c`.
            break;
        }
    }

    if (splitIdx >= 0) {
        var leftRaw = mainPart.slice(0, splitIdx).trim();
        var rightRaw = mainPart.slice(splitIdx + 1).trim();
        if (leftRaw.length && rightRaw.length) {
            result.parent = _parseSelector(leftRaw);
            result.direct = splitDirect;
            mainPart = rightRaw;
        }
    }

    // Tokenize `tag`, `#id`, `.class`, `[attr...]` from the rightmost
    // simple selector.  Regex covers all four forms in one pass; brackets
    // are matched non-greedily so two adjacent `[attr][attr2]` work.
    var tokenRe = /\[[^\]]+\]|#[A-Za-z_][\w-]*|\.[A-Za-z_][\w-]*|[A-Za-z_][\w-]*/g;
    var match;
    while ((match = tokenRe.exec(mainPart)) !== null) {
        var t = match[0];
        if (!t) continue;
        if (t[0] === "#") {
            result.id = t.slice(1);
        } else if (t[0] === ".") {
            result.classes.push(t.slice(1));
        } else if (t[0] === "[") {
            // Strip `[` and `]`
            var inner = t.slice(1, -1);
            // Recognised operators (longest first so `^=` doesn't eat `=`):
            var op = null;
            var opIdx = -1;
            var ops = ["^=", "$=", "*=", "|=", "~=", "="];
            for (var oi = 0; oi < ops.length; oi++) {
                var idx = inner.indexOf(ops[oi]);
                if (idx >= 0 && (opIdx < 0 || idx < opIdx)) {
                    op = ops[oi]; opIdx = idx;
                }
            }
            if (op === null) {
                result.attrs.push({ name: inner.trim(), op: null, value: null });
            } else {
                var aname = inner.slice(0, opIdx).trim();
                var aval = inner.slice(opIdx + op.length).trim();
                // Strip optional surrounding quotes (single or double).
                if (aval.length >= 2 &&
                    ((aval[0] === '"' && aval[aval.length - 1] === '"') ||
                     (aval[0] === "'" && aval[aval.length - 1] === "'"))) {
                    aval = aval.slice(1, -1);
                }
                result.attrs.push({ name: aname, op: op, value: aval });
            }
        } else {
            // Bare identifier — first one is the tag.  Multiple bare
            // identifiers in a single compound selector are spec-invalid
            // (`div span` is a descendant combinator, not compound) so
            // any later bare token is ignored.
            if (result.tag === null) result.tag = t.toLowerCase();
        }
    }

    return result;
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

    // pulp Wave 3 html.3 — attribute selectors.
    if (parsed.attrs && parsed.attrs.length) {
        for (var ai = 0; ai < parsed.attrs.length; ai++) {
            var a = parsed.attrs[ai];
            if (a.op === null) {
                if (!el.hasAttribute(a.name)) return false;
                continue;
            }
            var av = el.getAttribute(a.name);
            if (av === null || av === undefined) return false;
            switch (a.op) {
                case "=":  if (av !== a.value) return false; break;
                case "^=": if (a.value === "" || String(av).indexOf(a.value) !== 0) return false; break;
                case "$=": {
                    if (a.value === "") return false;
                    var s = String(av);
                    if (s.length < a.value.length) return false;
                    if (s.slice(s.length - a.value.length) !== a.value) return false;
                    break;
                }
                case "*=": if (a.value === "" || String(av).indexOf(a.value) < 0) return false; break;
                case "|=": {
                    var s2 = String(av);
                    if (s2 !== a.value && s2.indexOf(a.value + "-") !== 0) return false;
                    break;
                }
                case "~=": {
                    if (!a.value || /\s/.test(a.value)) return false;
                    var tokens = String(av).split(/\s+/);
                    var foundTok = false;
                    for (var ti = 0; ti < tokens.length; ti++) {
                        if (tokens[ti] === a.value) { foundTok = true; break; }
                    }
                    if (!foundTok) return false;
                    break;
                }
                default: return false;
            }
        }
    }

    // Match parent constraint
    if (parsed.parent) {
        if (parsed.direct) {
            // Direct child: parent must be immediate parent
            if (!el._parentElement || !_matchesSelector(el._parentElement, parsed.parent)) return false;
        } else {
            // Descendant: any ancestor must match
            var ancestor = el._parentElement;
            var found = false;
            while (ancestor) {
                if (_matchesSelector(ancestor, parsed.parent)) { found = true; break; }
                ancestor = ancestor._parentElement;
            }
            if (!found) return false;
        }
    }

    // pulp #1737 — pseudo-class state matching for querySelector. The
    // parser stored parsed.pseudo (e.g. `disabled`, `checked`, `nth-child(2)`)
    // but pre-fix _matchesSelector ignored it, so `div:disabled` matched
    // every `div` (catalog DIVERGE on html/document_querySelector).
    //
    // Implemented here: state-on-element pseudo-classes (`:disabled`,
    // `:checked`, `:enabled`, `:hover`, `:focus`) read directly from the
    // matching el._* slot the bridge already maintains. DOM-position
    // pseudo-classes (`:first-child`, `:last-child`, `:nth-child(N)`,
    // `:nth-of-type(N)`) walk the parent's children. `:not(<simple>)`
    // recursively dispatches to _matchesSelector with the negated selector.
    //
    // Pseudo-classes that require the full CSS Selectors Level 4 cascade
    // engine (`:has()`, `:is()`, `:where()`, attribute-namespace forms)
    // remain a no-match — the catalog flags those as architectural per
    // CLAUDE.md (Pulp's selector engine is single-pass tag/.class/#id/
    // [attr]/combinator).
    if (parsed.pseudo) {
        if (!_matchesPseudoClass(el, parsed.pseudo)) return false;
    }

    return true;
}

// pulp #1737 — pseudo-class evaluator. Returns true if `el` matches the
// pseudo-class string (e.g. `disabled`, `checked`, `nth-child(2n+1)`,
// `not(.foo)`). Unknown / unimplemented forms return false (the broader
// catalog claim is "no-match rather than throw" — same precedent as the
// pre-#1737 parser-tolerates-but-matcher-ignores behaviour, except now
// the matcher explicitly rejects so `div:nth-child(2)` no longer leaks
// to all `div`).
function _matchesPseudoClass(el, pseudo) {
    if (!pseudo) return true;
    var lower = pseudo.toLowerCase();

    // State-on-element pseudo-classes — read the bridge-maintained slot.
    if (lower === "disabled") return !!el._disabled;
    if (lower === "enabled")  return !el._disabled;
    if (lower === "checked")  return !!el._checked;
    if (lower === "hover")    return !!el._isHovered;
    if (lower === "focus")    return !!el._hasFocus;
    if (lower === "active")   return !!el._isActive;

    // DOM-position pseudo-classes. Need a parent to compute the index.
    var parent = el._parentElement;
    if (lower === "first-child") {
        return !!parent && parent._children && parent._children[0] === el;
    }
    if (lower === "last-child") {
        if (!parent || !parent._children) return false;
        return parent._children[parent._children.length - 1] === el;
    }
    if (lower === "only-child") {
        return !!parent && parent._children && parent._children.length === 1
            && parent._children[0] === el;
    }
    // pulp #1737 (Codex P2 followup #3 on #1779): `:root` matches the
    // document root element specifically (`__bodyElement__`), not any
    // element with no parent. The previous `!el._parentElement` check
    // also matched DETACHED elements (createElement before appendChild),
    // which leaked `:root { ... }` theme/layout styles into normal
    // nodes when they were later inserted. StyleSheet.attach() walks
    // every entry in `__elements__` so the bug surfaced for any element
    // created mid-stylesheet-life.
    //
    // Tied to the root via identity check — __bodyElement__ is the
    // synthetic body element this shim creates at the top of
    // web-compat-document.js. Detached elements still have a non-null
    // _parentElement once mounted (and even pre-mount they're never
    // === __bodyElement__), so this branch is safe.
    //
    // Catalog still doesn't claim :root for document.querySelector
    // because _findMatch starts traversal from root._children — the
    // root itself is never queued. So:
    //   * stylesheet `:root { color: red }` → applies to body (this branch).
    //   * document.querySelector(':root') → returns null (traversal
    //     never sees the root). Catalog supportedValues notes the gap.
    if (lower === "root") {
        return el === __bodyElement__;
    }
    if (lower === "empty") {
        return !el._children || el._children.length === 0;
    }

    // Functional pseudo-classes: `:not(<simple>)` and `:nth-child(N|2n|...)`.
    // Use raw `pseudo` (not lowercased) so the inner selector retains case
    // semantics for tag names + attribute values.
    var notMatch = pseudo.match(/^not\((.+)\)$/i);
    if (notMatch) {
        var inner = _parseSelector(notMatch[1]);
        return !_matchesSelector(el, inner);
    }
    var nthMatch = pseudo.match(/^nth-child\((.+)\)$/i);
    if (nthMatch) {
        if (!parent || !parent._children) return false;
        var idx = parent._children.indexOf(el);
        if (idx < 0) return false;
        return _matchesNth(idx + 1, nthMatch[1].trim());
    }
    var nthLast = pseudo.match(/^nth-last-child\((.+)\)$/i);
    if (nthLast) {
        if (!parent || !parent._children) return false;
        var lastIdx = parent._children.indexOf(el);
        if (lastIdx < 0) return false;
        return _matchesNth(parent._children.length - lastIdx, nthLast[1].trim());
    }

    // Unknown pseudo-class — explicit no-match (per CSS Selectors Level 4
    // forward-compat: unknown pseudo-classes match nothing rather than
    // refusing to parse).
    return false;
}

// pulp #1737 — :nth-child(N) argument parser. Accepts:
//   * `odd` / `even` (case-insensitive)
//   * a positive integer literal (`2`, `5`)
//   * `An+B` / `An-B` formula (e.g. `2n`, `2n+1`, `3n-1`, `-n+3`).
// Returns true if `pos` (1-based child index) matches the formula.
function _matchesNth(pos, arg) {
    var lower = arg.toLowerCase().replace(/\s+/g, "");
    if (lower === "odd")  return pos % 2 === 1;
    if (lower === "even") return pos % 2 === 0;
    // Plain integer.
    if (/^-?\d+$/.test(lower)) return pos === parseInt(lower, 10);
    // An+B form. Match `[A]n[+|-B]` where A and B are signed integers.
    // Both are optional; n is required.
    var m = lower.match(/^(-?\d*)n([+-]\d+)?$/);
    if (!m) return false;
    var aRaw = m[1];
    var bRaw = m[2] || "0";
    var a = aRaw === "" || aRaw === "+" ? 1 : (aRaw === "-" ? -1 : parseInt(aRaw, 10));
    var b = parseInt(bRaw, 10);
    if (a === 0) return pos === b;
    var k = (pos - b) / a;
    // Index must be a non-negative integer.
    return k >= 0 && Math.floor(k) === k;
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

