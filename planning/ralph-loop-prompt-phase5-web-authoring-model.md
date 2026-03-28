"Implement Phase 5: Web-Native Authoring Model for Pulp.

References:
- planning/phase-5-web-native-authoring-model.md (full spec)
- planning/w3c-css-support-matrix.md (property parity tracking)
- CLAUDE.md (build, test, architecture)
Tracking: planning/w3c-css-support-matrix.md

GOAL:
Add a JS-only compatibility layer so frontend developers can write familiar HTML/CSS/JS (document.createElement, element.style.*, addEventListener, querySelector) against Pulp's native GPU UI system. The public API feels browser-shaped. Internals remain native.

NON-NEGOTIABLES:
- No WebView, no DOM, no CSSOM, no browser engine
- The web-compat layer is a JS prelude loaded before user scripts
- Existing bridge functions (createRow, setFlex, setBackground, etc.) remain the internal API
- All existing tests must continue to pass
- Every new feature gets automated tests
- Screenshot verification before showing visual results to the user

PHASE 5.1 — Core Authoring (do this first):
1. Create `core/view/js/web-compat.js` prelude with:
   - `document` global object with `body`, `createElement`, `getElementById`
   - `Element` proxy class wrapping native widget IDs
   - Element properties: id, className, classList (add/remove/toggle/contains), textContent, value, hidden, disabled
   - DOM manipulation: appendChild, removeChild, insertBefore, remove, children, parentElement
   - Tag mapping: div->View, span/p/h1-h6->Label, button->ToggleButton, input->TextEditor/Fader/Checkbox, select->ComboBox, textarea->TextEditor(multiline), canvas->CanvasWidget, progress->ProgressBar, img->ImageWidget
   - For div: default flex-direction column; detect row from style.flexDirection

2. Create `core/view/js/css-parser.js` prelude with:
   - parseCSSLength(str) -> {value, unit} for px, em, rem, %
   - parseCSSColor(str) -> hex string for #rgb, #rrggbb, #rrggbbaa, rgb(), rgba(), hsl(), hsla(), named colors
   - expandShorthand(str) -> [top, right, bottom, left] for margin/padding
   - parseTransform(str) -> [{fn, args}] for scale(), rotate(), translate()
   - parseTransition(str) -> {property, duration, easing, delay}

3. Create `core/view/js/css-colors.js` prelude with:
   - All 148 named CSS colors as a lookup table
   - Include transparent and currentColor

4. Implement `element.style` as a CSSStyleDeclaration proxy:
   - Property writes parse the CSS value string and call the appropriate bridge function
   - Cover all properties listed in the naming alignment table in phase-5 spec section 8
   - Shorthand expansion for margin, padding, border, flex, transition, background
   - var() references resolve via theme token system (applyTokenDiff / resolve_color)

5. Implement addEventListener / removeEventListener:
   - Events: click, mouseenter, mouseleave, input, change, keydown, focus, blur
   - Event object with: type, target, currentTarget, clientX, clientY, key, code, ctrlKey, shiftKey, altKey, metaKey
   - Bubbling: events propagate from target up through parentElement chain
   - stopPropagation() halts bubbling
   - Capture phase support via {capture: true} option

Files likely involved:
- core/view/js/web-compat.js (NEW — main prelude)
- core/view/js/css-parser.js (NEW — value parsing)
- core/view/js/css-colors.js (NEW — named color table)
- core/view/src/script_engine.cpp (load preludes before user script)
- core/view/src/widget_bridge.cpp (minor: expose preludes, ensure IDs work)

Acceptance criteria:
- Can write `const div = document.createElement('div'); div.style.backgroundColor = '#333'; div.style.padding = '12px'; document.body.appendChild(div);` and see a rendered box
- Can write `element.addEventListener('click', handler)` and receive events
- Named CSS colors, rgb(), hsl() all resolve to correct hex values
- Shorthand properties expand correctly (margin: '10px 20px' -> four sides)
- 50+ unit tests for parser functions
- 20+ integration tests for element creation and styling
- All existing tests still pass

PHASE 5.2 — StyleSheet + querySelector:
1. Implement `StyleSheet` class:
   - Constructor takes rules object: { '.class': { prop: value }, '.class:hover': { prop: value } }
   - `attach()` registers rules with the element system
   - On className change, re-evaluate matching rules in source order
   - :hover pseudo-class auto-registers hover tracking and toggles styles
   - :focus pseudo-class applies styles on focus/blur
   - :active pseudo-class applies styles on mousedown/mouseup
   - :disabled pseudo-class applies when element.disabled = true

2. Implement querySelector / querySelectorAll:
   - Support: tag, .class, #id, tag.class, .a .b (descendant), .a > .b (child)
   - Tree walk through Element parent/children structure
   - Maintain class-to-elements multimap for fast class lookups

3. Implement getBoundingClientRect():
   - Requires C++ bridge addition: getLayoutRect(id) returning {x, y, width, height}
   - Returns object with x, y, width, height, top, right, bottom, left

4. Implement getComputedStyle():
   - Requires C++ bridge additions to read resolved values from native views
   - Returns read-only object with resolved CSS property values (px strings)

Files likely involved:
- core/view/js/web-compat.js (extend)
- core/view/js/selector-engine.js (NEW — selector parsing + matching)
- core/view/src/widget_bridge.cpp (add getLayoutRect, getComputedValue)
- core/view/include/pulp/view/widget_bridge.hpp

Acceptance criteria:
- StyleSheet class-based rules apply to elements with matching className
- :hover styles toggle on mouse enter/leave
- querySelector('.panel') returns first matching element
- querySelectorAll('.item') returns all matching elements
- getBoundingClientRect() returns layout-resolved geometry
- getComputedStyle(el).width returns resolved pixel string
- 30+ tests for selector matching
- 20+ tests for StyleSheet rule application

PHASE 5.3 — calc/clamp + units + remaining APIs:
1. Implement calc(), min(), max(), clamp() expression evaluation
2. Implement percentage unit resolution (requires parent dimension query)
3. Implement em/rem unit resolution (requires font-size inheritance tracking)
4. Implement element.dataset for data-* attributes
5. Implement setAttribute / getAttribute
6. Implement scrollIntoView
7. Implement FontFace loading API (local fonts only)

Acceptance criteria:
- calc(100% - 20px) evaluates correctly given parent width
- clamp(100px, 50%, 300px) evaluates correctly at various parent sizes
- 1.5em resolves to 1.5x parent font-size
- 25+ tests for calc/unit resolution

VERIFICATION AT EACH STEP:
1. Run ctest --test-dir build --output-on-failure after every change
2. Screenshot-verify any visual output before showing to user
3. Keep web-compat.js prelude under 15KB minified
4. Measure frame time overhead — must be < 0.5ms for 200-element UI"
