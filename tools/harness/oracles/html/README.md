# HTML oracle

The HTML oracle is a **static reference table** sourced from the WHATWG HTML
living standard, the DOM standard, and Pulp's own DOM-lite shim
(`core/view/js/web-compat-{element,document,dom-ops}.js`). For week-1 scope
this is a simple JSON table — NOT a `jsdom` runtime. Upgrading to a real
`jsdom` reference (the Week-3+ goal noted in
`planning/pulp-agent-prompt-harness-week1.md`) would let us actually execute
DOM operations and compare; this static oracle is the precondition.

We choose static over `jsdom` for week 1 because:

1. The DOM-lite surface Pulp consumes is small (~60 entries) and well-known.
2. A static table keeps the harness a single `python3` invocation with no
   Node/`npm` dependency.
3. The classifier is the same three-layer pattern the yoga adapter uses
   (catalog vs. binding-evidence vs. oracle), so the conventions are
   already proven.

## File: `html-supported.json`

Schema:

```json
{
  "version": "DOM4 + HTML5 living-standard subset",
  "source": "...",
  "categories": {
    "element_method": "Element.prototype.X = function(...)",
    "element_property": "Object.defineProperty(Element.prototype, 'X', {...})",
    "document_member": "document = { X: ... }",
    "html_tag": "Tag handled by Element.prototype._ensureNative dispatch",
    "feature": "Higher-level feature touching multiple artifacts"
  },
  "entries": {
    "html/<key>": {
      "category": "<one of the above>",
      "js_member": "name to grep for in element/document shim",
      "tag": "html tag (for html_tag entries)",
      "expected_bridge_calls": ["createCol", "createLabel", ...],
      "expected_event_types": ["click", "mouseenter", ...],
      "spec": "URL",
      "evidence": "free-form note"
    }
  }
}
```

The adapter looks up each compat.json key in `entries` and decides:

| Category          | "PASS" criterion                                                          |
|-------------------|---------------------------------------------------------------------------|
| `element_method`  | `Element.prototype.<js_member> = function` exists in web-compat-element.js |
| `element_property`| `Object.defineProperty(Element.prototype, '<js_member>', ...)` exists      |
| `document_member` | `<js_member>:` appears inside the `document = {...}` object literal       |
| `html_tag`        | The tag is matched by `_ensureNative` AND each `expected_bridge_calls` entry is registered in `core/view/src/widget_bridge.cpp` |
| `feature`         | Status defaults to the catalog claim — these are documentary, not bindable |

If any binding evidence is missing the verdict is NOT-IMPL or DIVERGE per
the catalog status / unsupportedValues.

## Regeneration

The oracle is hand-maintained. To refresh:

1. Walk `core/view/js/web-compat-element.js` for new `Element.prototype.X` /
   `Object.defineProperty(Element.prototype, ...)` additions.
2. Walk `core/view/js/web-compat-document.js` for new `document.X` members.
3. Walk `core/view/src/widget_bridge.cpp` for new `engine_.register_function`
   bindings tied to HTML tags.
4. Mirror changes into `html-supported.json` and bump `version`.
5. Run `python3 tools/harness/verifier.py --surface=html` and review the drift list.

## Future upgrade path (Week 3+)

Replace the static `entries` table with a `jsdom`-driven runner that:

* Executes each `Element` / `document` API against `jsdom` to capture a
  reference behavior trace.
* Executes the same API against Pulp's web-compat JS (loaded into a QuickJS
  context with bridge function stubs) to capture an under-test trace.
* Diffs the two traces.

The static oracle's `category` + `js_member` keys are the input the future
runtime will consume, so adding `jsdom` is purely additive — no schema break.
