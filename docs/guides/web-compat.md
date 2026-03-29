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
| `mouseenter` | Mouse enters element |
| `mouseleave` | Mouse leaves element |
| `input` | Value changes (text editors, sliders) |
| `change` | Value committed |

## Layout Inspection

### getBoundingClientRect

```js
const rect = el.getBoundingClientRect();
// { x, y, width, height, top, left, right, bottom }
```

### getComputedStyle

```js
const style = getComputedStyle(el);
style.getPropertyValue('width');   // e.g., "200px"
style.getPropertyValue('opacity'); // e.g., "1"
```

## CSS Parsing Utilities

These global functions are available for parsing CSS values in custom code:

```js
parseCSSColor('cornflowerblue');    // '#6495ed'
parseCSSColor('rgb(255, 128, 0)'); // '#ff8000'

parseCSSLength('20px');             // { value: 20, unit: 'px' }
parseCSSLength('50%');              // { value: 50, unit: '%' }

expandShorthand('10px 20px');       // [10, 20, 10, 20]

parseTransform('scale(1.5) rotate(45)');
// [{ fn: 'scale', args: [1.5] }, { fn: 'rotate', args: [45] }]
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

## Limitations

- No `<form>` elements or form submission
- No `<table>` layout (use CSS Grid instead)
- No CSS animations via `@keyframes` (use Pulp's `animate()` bridge)
- No `window.setTimeout` / `setInterval` (use Pulp's timer system)
- `calc()`, `clamp()`, `em`, `rem` units not yet supported
- No `<video>` or `<audio>` elements
