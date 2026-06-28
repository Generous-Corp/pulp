# Web-Compat Layer

Pulp includes a browser-shaped JavaScript API so frontend developers can write familiar `document.createElement` / `element.style` / `appendChild` code against Pulp's native GPU UI. No WebView, no DOM, no browser engine — just a JS prelude that maps browser idioms to Pulp's native widget system.

## Quick Start

```js
// Create elements like you would in a browser
const panel = document.createElement('div');
panel.style.backgroundColor = '#1a1a2e';
panel.style.padding = '16px';
panel.style.borderRadius = '8px';

const title = document.createElement('h2');
title.textContent = 'My Plugin';

const knob = document.createElement('input');
knob.type = 'range';

panel.appendChild(title);
panel.appendChild(knob);
document.body.appendChild(panel);
```

This creates real GPU-rendered native widgets — not HTML elements.

## Tag Mapping

| HTML Tag | Pulp Widget | Notes |
|----------|-------------|-------|
| `<div>` | View (column) | Default flex-direction: column |
| `<span>`, `<p>`, `<label>` | Label | Text display |
| `<h1>`–`<h6>` | Label | With appropriate font size/weight |
| `<button>` | ToggleButton | Stateful push button |
| `<input type="text">` | TextEditor | Text input field |
| `<input type="range">` | Fader | Linear slider |
| `<input type="checkbox">` | Checkbox | Boolean control |
| `<select>` | ComboBox | Dropdown selector |
| `<textarea>` | TextEditor (multiline) | Multi-line text |
| `<canvas>` | CanvasWidget | JS-driven custom drawing |
| `<progress>` | ProgressBar | Progress indicator |
| `<img>` | ImageView | Image display |

## Element Properties

### Standard DOM Properties

```js
const el = document.createElement('div');

el.id = 'my-panel';              // Sets element ID
el.className = 'active panel';    // Space-separated class names
el.textContent = 'Hello';         // Text content (for labels)
el.hidden = true;                 // Visibility
el.disabled = true;               // Disabled state (blocks input, grays out)
```

### classList

```js
el.classList.add('active');
el.classList.remove('active');
el.classList.toggle('selected');
el.classList.contains('active');  // returns boolean
```

### dataset / attributes

```js
el.setAttribute('data-param-id', 'gain');
el.getAttribute('data-param-id');  // 'gain'
el.dataset.paramId;                // 'gain' (camelCase conversion)
```

## Styling

### element.style

Set CSS properties directly on elements. Values are parsed and mapped to native Pulp properties.

```js
// Dimensions
el.style.width = '200px';
el.style.height = '100px';
el.style.minWidth = '50px';
el.style.maxHeight = '300px';

// Flex layout
el.style.flexDirection = 'row';
el.style.justifyContent = 'center';
el.style.alignItems = 'center';
el.style.flexGrow = '1';
el.style.gap = '8px';

// Spacing (shorthand supported)
el.style.padding = '12px';           // all sides
el.style.padding = '8px 16px';       // vertical horizontal
el.style.margin = '4px 8px 12px 16px'; // top right bottom left

// Background
el.style.backgroundColor = '#1a1a2e';
el.style.backgroundColor = 'cornflowerblue';  // named colors
el.style.backgroundColor = 'rgb(30, 30, 46)';
el.style.backgroundColor = 'hsl(240, 20%, 15%)';

// Border
el.style.borderRadius = '8px';
el.style.border = '1px solid #333';

// Text
el.style.fontSize = '14px';
el.style.fontWeight = '700';
el.style.textAlign = 'center';
el.style.color = '#e0e0e0';

// Visual
el.style.opacity = '0.8';
el.style.display = 'none';  // hides element
el.style.overflow = 'hidden';

// Transform
el.style.transform = 'scale(1.5) rotate(45)';

// Position
el.style.position = 'absolute';
el.style.top = '10px';
el.style.left = '20px';
el.style.zIndex = '10';
```

### CSS Color Formats

All standard CSS color formats are supported:

```js
'#f00'                    // short hex
'#ff0000'                 // hex
'#ff000080'               // hex with alpha
'rgb(255, 0, 0)'          // rgb()
'rgba(255, 0, 0, 0.5)'    // rgba()
'hsl(0, 100%, 50%)'       // hsl()
'hsla(0, 100%, 50%, 0.5)' // hsla()
'red'                     // 148 named CSS colors
'transparent'             // fully transparent
```

## DOM Manipulation

### appendChild / removeChild

```js
const parent = document.createElement('div');
const child = document.createElement('span');
child.textContent = 'Hello';

parent.appendChild(child);        // adds child to parent
document.body.appendChild(parent); // adds parent to root

parent.removeChild(child);        // removes child
child.remove();                   // removes self from parent
```

### insertBefore / replaceChild

```js
parent.insertBefore(newChild, referenceChild);
parent.replaceChild(newChild, oldChild);
```

## Querying

### document.getElementById

```js
const el = document.getElementById('my-panel');
```

### querySelector / querySelectorAll

Supports: `#id`, `.class`, `tag`, `tag.class`, `.parent .child` (descendant), `.parent > .child` (direct child).

```js
const panel = document.querySelector('.panel');
const items = document.querySelectorAll('.item');
const heading = document.querySelector('h1');
const child = document.querySelector('.panel > .content');
```

### getElementsByClassName

```js
const panels = document.getElementsByClassName('panel');
```

## StyleSheet

Class-based styling with pseudo-class support:

```js
const styles = new StyleSheet({
    '.panel': {
        backgroundColor: '#1a1a2e',
        padding: '16px',
        borderRadius: '8px'
    },
    '.panel:hover': {
        backgroundColor: '#2a2a4e'
    },
    '.button': {
        width: '120px',
        height: '36px',
        backgroundColor: '#e94560'
    }
});
styles.attach();

// Elements with matching classes get styled automatically
const panel = document.createElement('div');
panel.className = 'panel';
document.body.appendChild(panel); // gets panel styles applied
```

## Events

### addEventListener

```js
el.addEventListener('click', function(event) {
    console.log('Clicked!', event.type);
});

el.addEventListener('mouseenter', function() {
    el.style.opacity = '1';
});

el.addEventListener('mouseleave', function() {
    el.style.opacity = '0.7';
});
```

Events propagate (bubble) from target up through parentElement chain. Use `event.stopPropagation()` to halt bubbling.

### Supported Events

| Event | Fires When |
|-------|-----------|
| `click` | Element clicked |
| `mousedown` / `mouseup` | Mouse button pressed / released |
| `mouseenter` | Mouse enters element (no bubble) |
| `mouseleave` | Mouse leaves element (no bubble) |
| `input` | Value changes (text editors, sliders) |
| `change` | Value committed |
| `keydown` / `keyup` | Key pressed / released |
| `focus` / `blur` | Element gains / loses focus |
| `pointerdown` / `pointermove` / `pointerup` | Pointer events (mouse, touch, pen) |
| `gesturestart` / `gesturechange` / `gestureend` | Multi-touch gestures (scale, rotation) |

Events propagate in the standard capture → target → bubble order. Capture phase listeners receive events first when registered with `{ capture: true }`.

## DOM Traversal & Querying

### closest / matches / contains

```js
// Find nearest ancestor matching a selector
const panel = el.closest('.panel');

// Test if element matches a selector
if (el.matches('.active')) { /* ... */ }

// Check if element contains another
if (container.contains(child)) { /* ... */ }
```

### querySelector on elements

```js
// Scoped queries — search within a specific element's subtree
const item = panel.querySelector('.item');
const buttons = panel.querySelectorAll('button');
```

### innerHTML

```js
// Set HTML content (simple parser supports nested tags)
panel.innerHTML = '<div class="header"><span>Title</span></div>';

// Read serialized HTML
console.log(panel.outerHTML);
```

### Modern DOM insertion

```js
parent.append(child1, child2, "text");    // Append multiple
parent.prepend(child);                     // Insert at start
el.before(sibling);                        // Insert before
el.after(sibling);                         // Insert after
el.replaceWith(replacement);               // Replace self
```

## Selectors

### Supported Selector Syntax

| Syntax | Example | Notes |
|--------|---------|-------|
| Tag | `div`, `button` | Element type |
| Class | `.panel` | Class name |
| ID | `#header` | Element ID |
| Multiple classes | `.btn.primary` | All classes must match |
| Descendant | `.panel .item` | Any depth |
| Child | `.panel > .item` | Direct child only |
| `:first-child` | `li:first-child` | First child of parent |
| `:last-child` | `li:last-child` | Last child of parent |
| `:nth-child(An+B)` | `li:nth-child(odd)`, `li:nth-child(2n+1)`, `li:nth-child(3)` | Pattern matching |
| `:nth-last-child(An+B)` | `li:nth-last-child(2)` | From end |
| `:only-child` | `div:only-child` | Sole child |
| `:empty` | `div:empty` | No children or text |
| `:checked` | `input:checked` | Checked inputs |
| `:disabled` | `input:disabled` | Disabled elements |
| `:not(selector)` | `.item:not(.disabled)` | Negation |
| `:hover` / `:focus` / `:active` | `.btn:hover` | Via StyleSheet rules |

## CSS Values & Units

### calc / min / max / clamp

```js
el.style.width = 'calc(100% - 40px)';
el.style.fontSize = 'clamp(12px, 2vw, 18px)';
el.style.padding = 'max(8px, 1%)';
el.style.height = 'min(200px, 50vh)';
```

Full expression evaluator with `+`, `-`, `*`, `/`, nested functions, and mixed units.

### Relative Units

| Unit | Resolves Against |
|------|-----------------|
| `px` | Pixels (default) |
| `em` | Parent element font-size |
| `rem` | Root element font-size (default 14px) |
| `%` | Parent element dimension |
| `vw` / `vh` | Root view width / height |
| `vmin` / `vmax` | Smaller / larger of vw and vh |
| `ch` | Approximate character width (0.5 × font-size) |

```js
el.style.fontSize = '1.5em';      // 1.5x parent font-size
el.style.width = '50%';           // Half of parent width
el.style.padding = '2vw';         // 2% of viewport width
```

### CSS Custom Properties

```js
// Set via element.style.setProperty
document.documentElement.style.setProperty('--accent', '#3b82f6');
document.documentElement.style.setProperty('--radius', '8px');

// Use in values
el.style.backgroundColor = 'var(--accent)';
el.style.borderRadius = 'var(--radius)';

// Use in StyleSheet rules
new StyleSheet({
    '.panel': { backgroundColor: 'var(--accent)' }
});
```

## Responsive Design

### matchMedia

```js
const mq = window.matchMedia('(min-width: 600px)');
if (mq.matches) {
    // Wide layout
}

// Supported queries: min-width, max-width, min-height, max-height, orientation
const portrait = window.matchMedia('(orientation: portrait)');
```

### Dynamic viewport dimensions

```js
window.innerWidth;   // Actual root view width (updates dynamically)
window.innerHeight;  // Actual root view height
```

## Additional CSS Properties

### Layout

```js
el.style.aspectRatio = '16/9';         // Maintain proportions
el.style.visibility = 'hidden';        // Hidden but preserves layout space
el.style.pointerEvents = 'none';       // Click-through overlay
el.style.alignContent = 'center';      // Multi-line flex cross-axis
```

### Visual

```js
el.style.outline = '2px solid blue';   // Focus indicator (outside border)
el.style.whiteSpace = 'nowrap';        // Prevent text wrapping
el.style.userSelect = 'none';          // Prevent text selection
el.style.fontFamily = 'Inter';         // Font selection
el.style.textShadow = '2px 2px 4px rgba(0,0,0,0.5)';
el.style.backgroundSize = 'cover';
el.style.backgroundPosition = 'center';
```

## Layout Inspection

### getBoundingClientRect

```js
const rect = el.getBoundingClientRect();
// { x, y, width, height, top, left, right, bottom }
```

### getComputedStyle

```js
const style = getComputedStyle(el);
style.width;    // e.g., "200px" (layout-resolved)
style.opacity;  // e.g., "1"
```

## Focus Management

```js
el.focus();   // Gives keyboard focus
el.blur();    // Removes keyboard focus
```

Tab / Shift+Tab cycles through focusable elements automatically.

## Timers

```js
window.setTimeout(fn, 500);       // Approximate via animation frames
window.setInterval(fn, 1000);     // Repeating timer
```

## Mixing with Native Bridge

The web-compat layer works alongside the native Pulp bridge. You can mix both:

```js
// Web-compat style
const div = document.createElement('div');
div.style.padding = '16px';
document.body.appendChild(div);

// Native bridge style (using the element's internal ID)
createKnob('my-knob', div._id);
setValue('my-knob', 0.75);
on('my-knob', 'change', function(val) {
    console.log('Knob:', val);
});
```

Use `element._id` to get the internal widget ID for native bridge calls.

## Current Limitations

- No per-side borders (border-top, border-right, etc.) — only uniform border
- No per-corner border-radius — only single radius value
- No CSS `@keyframes` animations (use Pulp's `animate()` bridge)
- No CSS `transitionend` events yet; `element.animate()` dispatches a Pulp-specific `animationend` event on the element
- `element.animate()` interpolates the first and last keyframe for single numeric values with optional units; transforms, colors, multi-value properties, and intermediate keyframes / offsets / easing snap to the final value
- No general per-element `resize` event yet — window resize is available via `window.addEventListener('resize', ...)`
- No `<form>`, `<table>`, `<video>`, `<audio>` elements
- No `::before` / `::after` pseudo-elements
- `getComputedStyle()` is partial — inline style plus selected layout dimensions, not a full browser CSSOM snapshot
- CSS `margin: auto` / `margin-left: auto` / `margin-right: auto` centering is not supported yet
- `min-content` / `max-content` / `fit-content` size keywords not supported
