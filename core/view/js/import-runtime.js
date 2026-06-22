// import-runtime.js — DOM construction + walker for the
// `--execute-bundle` Claude Design import lane.
//
// Two responsibilities, both gated behind the `__pulpImportRuntime__`
// global so they don't pollute the runtime when the harness isn't in
// use:
//
// 1. `__pulpImportRuntime__.buildDom(html)` — minimal HTML parser that
//    constructs Element trees under `document.body` via the existing
//    web-compat document. Sufficient for Claude Design's bundler
//    template, which is well-formed HTML emitted by the exporter
//    (no script content, no CDATA, no comments — just a `<div id="root">`
//    plus a small set of <script src="<uuid>"> tags that we strip on
//    parse since the harness eval's the JS payloads itself).
//
// 2. `__pulpImportRuntime__.walkDom(root)` — serializer that turns the
//    materialized DOM tree (after React commits) into a plain-JSON tree
//    the C++ side ingests as DesignIR. Mirrors the shape of
//    parse_stitch_html's IR so codegen rules apply uniformly.

(function() {
    if (typeof globalThis.__pulpImportRuntime__ !== "undefined") return;

    // ─── HTML parser ────────────────────────────────────────────────────────
    //
    // Tag-state machine. NOT a real HTML parser — handles enough of the
    // Claude Design template surface (open/close tags, void elements,
    // attributes with single + double quotes, text nodes, basic entities).
    //
    // Strips <script> and <style> tags since:
    //   - <script src="<uuid>"> is the bundler asset reference; the harness
    //     evaluates the resolved JS payloads itself in C++.
    //   - <style> blobs are static CSS the React app will not query.
    //
    // Whitespace-only text nodes between block elements are dropped so
    // the materialized tree doesn't fill up with phantom Text nodes from
    // pretty-printed source HTML.

    var VOID_TAGS = {
        "area": 1, "base": 1, "br": 1, "col": 1, "embed": 1, "hr": 1,
        "img": 1, "input": 1, "link": 1, "meta": 1, "param": 1, "source": 1,
        "track": 1, "wbr": 1
    };

    var SKIP_TAGS = { "script": 1, "style": 1, "noscript": 1 };

    function decodeEntities(str) {
        if (str.indexOf("&") < 0) return str;
        return str
            .replace(/&lt;/g, "<")
            .replace(/&gt;/g, ">")
            .replace(/&quot;/g, "\"")
            .replace(/&#39;/g, "'")
            .replace(/&apos;/g, "'")
            .replace(/&nbsp;/g, " ")
            .replace(/&amp;/g, "&");
    }

    // Parse `key="val" key2='val2' boolflag` into an object.
    function parseAttrs(str) {
        var attrs = {};
        if (!str) return attrs;
        var re = /([a-zA-Z_:][a-zA-Z0-9_:.-]*)(?:\s*=\s*("([^"]*)"|'([^']*)'|([^\s>]+)))?/g;
        var m;
        while ((m = re.exec(str)) !== null) {
            var name = m[1];
            var val = m[3] != null ? m[3] : (m[4] != null ? m[4] : (m[5] != null ? m[5] : ""));
            attrs[name] = decodeEntities(val);
        }
        return attrs;
    }

    function applyAttrs(el, attrs) {
        for (var name in attrs) {
            if (!Object.prototype.hasOwnProperty.call(attrs, name)) continue;
            var val = attrs[name];
            if (name === "id") {
                el.id = val;
            } else if (name === "class") {
                el.className = val;
            } else if (name === "style") {
                // style attribute -> el.style.cssText not implemented, parse
                // "key:value;key2:value2" minimally so React's inline-style
                // strings still apply.
                var rules = val.split(";");
                for (var i = 0; i < rules.length; i++) {
                    var pair = rules[i].split(":");
                    if (pair.length < 2) continue;
                    var k = pair[0].trim();
                    var v = pair.slice(1).join(":").trim();
                    if (k && v) {
                        // camelCase: "background-color" -> "backgroundColor"
                        var camel = k.replace(/-([a-z])/g, function(_, c) {
                            return c.toUpperCase();
                        });
                        try { el.style[camel] = v; } catch (e) {}
                    }
                }
            } else {
                el.setAttribute(name, val);
            }
        }
    }

    function buildDom(html, parent) {
        if (typeof html !== "string") return;
        var stack = [parent];
        var pos = 0;
        var len = html.length;

        function top() { return stack[stack.length - 1]; }

        while (pos < len) {
            var lt = html.indexOf("<", pos);
            if (lt < 0) {
                // trailing text
                var text = html.slice(pos);
                if (text.trim().length > 0) {
                    top().appendChild(document.createTextNode(decodeEntities(text)));
                }
                break;
            }
            // Text node before the next tag.
            if (lt > pos) {
                var t = html.slice(pos, lt);
                if (t.trim().length > 0) {
                    top().appendChild(document.createTextNode(decodeEntities(t)));
                }
            }
            // Comment: <!--...-->
            if (html.substr(lt, 4) === "<!--") {
                var cend = html.indexOf("-->", lt + 4);
                if (cend < 0) break;
                pos = cend + 3;
                continue;
            }
            // DOCTYPE: <!DOCTYPE ...>
            if (html.substr(lt, 2) === "<!") {
                var dend = html.indexOf(">", lt + 2);
                if (dend < 0) break;
                pos = dend + 1;
                continue;
            }
            // Closing tag: </tag>
            if (html.charAt(lt + 1) === "/") {
                var ge = html.indexOf(">", lt);
                if (ge < 0) break;
                var closeName = html.slice(lt + 2, ge).trim().toLowerCase();
                // Pop stack until we find a matching tag.
                for (var s = stack.length - 1; s > 0; --s) {
                    if (stack[s].tagName && stack[s].tagName.toLowerCase() === closeName) {
                        stack.length = s;
                        break;
                    }
                }
                pos = ge + 1;
                continue;
            }
            // Opening tag: <tag attrs>
            var ge2 = html.indexOf(">", lt);
            if (ge2 < 0) break;
            var raw = html.slice(lt + 1, ge2);
            var selfClose = false;
            if (raw.charAt(raw.length - 1) === "/") {
                selfClose = true;
                raw = raw.slice(0, -1);
            }
            // Split tag name from attrs.
            var spaceIdx = raw.search(/\s/);
            var tagName, attrStr;
            if (spaceIdx < 0) { tagName = raw; attrStr = ""; }
            else { tagName = raw.slice(0, spaceIdx); attrStr = raw.slice(spaceIdx + 1); }
            tagName = tagName.toLowerCase();
            pos = ge2 + 1;

            // Skip <script>, <style>, <noscript> entirely — find matching close.
            if (SKIP_TAGS[tagName]) {
                var closeStr = "</" + tagName;
                var k = pos;
                while (k < len) {
                    var n = html.indexOf("<", k);
                    if (n < 0) { pos = len; break; }
                    if (html.substr(n, closeStr.length).toLowerCase() === closeStr) {
                        var ce = html.indexOf(">", n);
                        if (ce < 0) { pos = len; break; }
                        pos = ce + 1;
                        break;
                    }
                    k = n + 1;
                }
                continue;
            }

            var attrs = parseAttrs(attrStr);
            var el = document.createElement(tagName);
            applyAttrs(el, attrs);
            top().appendChild(el);
            if (!selfClose && !VOID_TAGS[tagName]) {
                stack.push(el);
            }
        }
    }

    // ─── DOM walker ─────────────────────────────────────────────────────────
    //
    // After the bundle runs, the DOM under `document.body` reflects whatever
    // React committed. Walk it into a plain-JSON tree the C++ side can ingest.
    // Each node looks like:
    //   { type: "div"|"span"|...,
    //     id: "...",
    //     class: "foo bar",
    //     attrs: { "data-pulp-role": "..." },
    //     style: { backgroundColor: "#1e1e2e", ... },
    //     text: "Hello",
    //     children: [ ... ] }
    //
    // Text nodes serialize as { type: "#text", text: "..." }.
    // Comment nodes are dropped (they're React reconciliation scaffolding).

    function serializeStyle(el) {
        var out = {};
        if (!el || !el.style || !el.style._props) return out;
        for (var k in el.style._props) {
            if (Object.prototype.hasOwnProperty.call(el.style._props, k)) {
                out[k] = el.style._props[k];
            }
        }
        return out;
    }

    function serializeAttrs(el) {
        var out = {};
        if (!el || !el._attributes) return out;
        for (var k in el._attributes) {
            if (Object.prototype.hasOwnProperty.call(el._attributes, k)) {
                if (k === "id" || k === "class" || k === "style") continue;
                out[k] = el._attributes[k];
            }
        }
        return out;
    }

    function walkDom(root) {
        if (!root) return null;
        // Text node?
        if (root._isTextNode) {
            return { type: "#text", text: root._textContent || "" };
        }
        // Comment node — drop.
        if (root._isCommentNode) return null;

        var node = {
            type: (root.tagName || "div").toLowerCase(),
            id: root._userIdSet ? (root._attributes && root._attributes["id"]) || "" : "",
            class: root._className || "",
            attrs: serializeAttrs(root),
            style: serializeStyle(root),
            text: "",
            children: []
        };

        var kids = root._children || [];
        if (kids.length === 0) {
            // Leaf with text content (typical for label/span/p/h1-h6).
            node.text = root._textContent || "";
        } else {
            for (var i = 0; i < kids.length; i++) {
                var c = walkDom(kids[i]);
                if (c) node.children.push(c);
            }
        }
        return node;
    }

    // Public entry points for the C++ harness to call via engine.invoke().

    globalThis.__pulpImportRuntime__ = {
        // Last error from buildDom (introspection hook for the C++
        // harness — set when buildDom catches but the C++ caller can't
        // see the JS exception directly).
        _lastError: "",
        buildDom: function(html) {
            try {
                buildDom(html, document.body);
                return true;
            } catch (e) {
                globalThis.__pulpImportRuntime__._lastError = String(e);
                return false;
            }
        },
        walkDom: function() {
            return walkDom(document.body);
        },
        // Convenience: serialize the body tree to a JSON string. The C++
        // side prefers a string return because choc::value round-trip
        // through QuickJS object handles is more brittle than JSON parse.
        walkDomJson: function() {
            try {
                return JSON.stringify(walkDom(document.body));
            } catch (e) {
                return JSON.stringify({ type: "#error", text: String(e) });
            }
        },
        // Re-mount: clear everything in body and rebuild from the given
        // template HTML. Used when the harness needs to re-run a fresh
        // import without spinning up a new engine.
        resetBody: function() {
            if (!document.body) return false;
            // Detach native counterparts so a re-run doesn't pile up.
            var kids = document.body._children.slice(0);
            for (var i = 0; i < kids.length; i++) {
                document.body.removeChild(kids[i]);
            }
            return true;
        }
    };
})();
