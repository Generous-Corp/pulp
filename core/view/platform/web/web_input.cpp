// DOM plumbing for the Emscripten BrowserWindowHost: pointer / wheel / keyboard
// / text / resize listeners on the target canvas, plus the canvas sizing and
// devicePixelRatio queries the host needs.
//
// This file owns the browser side ONLY. Every event is handed straight to the
// Emscripten-free translation + routing layer in web_event_translate.hpp, which
// is where the coordinate math lives and where it is unit-tested.

#include <pulp/view/web/web_event_translate.hpp>

#if defined(__EMSCRIPTEN__)

#include <emscripten/emscripten.h>

#include <string>

namespace pulp::view::web {
namespace {

WebInputRouter* g_router = nullptr;

// Modifier bitmask exchanged with the JS listeners (their own encoding — the
// Pulp `Modifier` flags are derived from it in translate_modifiers()).
constexpr int kJsModCtrl = 1 << 0;
constexpr int kJsModShift = 1 << 1;
constexpr int kJsModAlt = 1 << 2;
constexpr int kJsModMeta = 1 << 3;

BrowserModifiers decode_modifiers(int mask) {
    BrowserModifiers mods;
    mods.ctrl = (mask & kJsModCtrl) != 0;
    mods.shift = (mask & kJsModShift) != 0;
    mods.alt = (mask & kJsModAlt) != 0;
    mods.meta = (mask & kJsModMeta) != 0;
    return mods;
}

BrowserPointerPhase decode_phase(int phase) {
    switch (phase) {
        case 1: return BrowserPointerPhase::move;
        case 2: return BrowserPointerPhase::up;
        case 3: return BrowserPointerPhase::cancel;
        default: return BrowserPointerPhase::down;
    }
}

BrowserPointerType decode_pointer_type(int type) {
    switch (type) {
        case 1: return BrowserPointerType::touch;
        case 2: return BrowserPointerType::pen;
        default: return BrowserPointerType::mouse;
    }
}

BrowserDeltaMode decode_delta_mode(int mode) {
    switch (mode) {
        case 1: return BrowserDeltaMode::line;
        case 2: return BrowserDeltaMode::page;
        default: return BrowserDeltaMode::pixel;
    }
}

}  // namespace

// ── C entry points called from the JS listeners ─────────────────────────────

extern "C" {

EMSCRIPTEN_KEEPALIVE void pulp_web_on_pointer(float css_x, float css_y, int phase,
                                              int button, int buttons,
                                              int pointer_id, int is_primary,
                                              int click_count, int pointer_type,
                                              float pressure, int modifiers) {
    if (!g_router) return;
    BrowserPointerEvent event;
    event.css_x = css_x;
    event.css_y = css_y;
    event.phase = decode_phase(phase);
    event.button = button;
    event.buttons = buttons;
    event.pointer_id = pointer_id;
    event.is_primary = is_primary != 0;
    event.click_count = click_count;
    event.pointer_type = decode_pointer_type(pointer_type);
    event.pressure = pressure;
    event.modifiers = decode_modifiers(modifiers);
    g_router->handle_pointer(event);
}

EMSCRIPTEN_KEEPALIVE void pulp_web_on_wheel(float css_x, float css_y,
                                            float delta_x, float delta_y,
                                            int delta_mode, int modifiers) {
    if (!g_router) return;
    BrowserWheelEvent event;
    event.css_x = css_x;
    event.css_y = css_y;
    event.delta_x = delta_x;
    event.delta_y = delta_y;
    event.delta_mode = decode_delta_mode(delta_mode);
    event.modifiers = decode_modifiers(modifiers);
    g_router->handle_wheel(event);
}

EMSCRIPTEN_KEEPALIVE void pulp_web_on_key(const char* key, int is_down,
                                          int is_repeat, int modifiers) {
    if (!g_router || !key) return;
    BrowserKeyEvent event;
    event.key = key;
    event.is_down = is_down != 0;
    event.is_repeat = is_repeat != 0;
    event.modifiers = decode_modifiers(modifiers);
    g_router->handle_key(event);
}

EMSCRIPTEN_KEEPALIVE void pulp_web_on_text(const char* text) {
    if (!g_router || !text) return;
    g_router->handle_text(text);
}

EMSCRIPTEN_KEEPALIVE void pulp_web_on_resize(float css_width, float css_height) {
    notify_browser_host_resized(css_width, css_height);
}

}  // extern "C"

}  // namespace pulp::view::web

// ── JS listeners ────────────────────────────────────────────────────────────
//
// EM_JS declarations live at file scope: the macro declares an imported C
// function, and the JS side resolves the compiled entry points by their
// exported C names (_pulp_web_on_*).
//
// Pointer coordinates are taken from getBoundingClientRect(), so they are CSS
// pixels relative to the canvas box — never device pixels. The
// devicePixelRatio is applied to the BACKING STORE only (see
// pulp_web_set_canvas_size); scaling the coordinates here too would scale
// input twice.

EM_JS(void, pulp_web_install_listeners, (const char* selector), {
    var sel = UTF8ToString(selector);
    var canvas = document.querySelector(sel);
    if (!canvas) return;

    Module.__pulpWebListeners = Module.__pulpWebListeners || {};
    if (Module.__pulpWebListeners[sel]) return;

    // Gesture hygiene. A Pulp canvas is a control surface, not a document: a
    // knob drag must not select page text, pan the page, or raise the iOS
    // touch callout, and a double-click must not select anything. preventDefault
    // on pointerdown does NOT reliably suppress selection (WebKit starts it from
    // the mousedown that follows), so the canvas must also be unselectable by
    // style, and touch-action must be surrendered BEFORE the browser decides the
    // gesture is a scroll — it consults the style, not our listeners.
    canvas.style.touchAction = 'none';
    canvas.style.userSelect = 'none';
    canvas.style.webkitUserSelect = 'none';
    canvas.style.webkitTouchCallout = 'none';

    var mods = function(e) {
        return (e.ctrlKey ? 1 : 0) | (e.shiftKey ? 2 : 0) |
               (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
    };
    var pos = function(e) {
        var r = canvas.getBoundingClientRect();
        return [e.clientX - r.left, e.clientY - r.top];
    };
    var ptype = function(e) {
        if (e.pointerType === 'touch') return 1;
        if (e.pointerType === 'pen') return 2;
        return 0;
    };
    var pointer = function(e, phase) {
        var p = pos(e);
        _pulp_web_on_pointer(p[0], p[1], phase, e.button | 0, e.buttons | 0,
                             e.pointerId | 0, e.isPrimary ? 1 : 0,
                             e.detail ? e.detail : 1, ptype(e),
                             e.pressure === undefined ? 0.5 : e.pressure,
                             mods(e));
    };

    var state = {};
    state.pointerdown = function(e) {
        // Panning opt-in. A control surface keeps the default touch-action:none and
        // captures every gesture. But an editor embedded in a scrollable page (only
        // horizontal controls, no vertical drags) can set touch-action:pan-y so a
        // VERTICAL touch-drag scrolls the page. Capturing the pointer or
        // preventDefault-ing here would cancel that scroll, so for a touch on a
        // pannable canvas we let the browser arbitrate: vertical → page scroll,
        // horizontal drag or tap → stays with the canvas (pan-y claims only the
        // vertical axis, so the pointer stream still drives a horizontal slider).
        var pannable = e.pointerType === 'touch' &&
                       canvas.style.touchAction && canvas.style.touchAction !== 'none';
        // Pointer capture keeps drag ticks and the release coming to the canvas
        // once the pointer leaves it mid-gesture (knob drags routinely do).
        if (!pannable && canvas.setPointerCapture) {
            try { canvas.setPointerCapture(e.pointerId); } catch (err) {}
        }
        // preventScroll is load-bearing: focus() otherwise scrolls the canvas
        // into view, so clicking a knob on a page that scrolls yanks the page
        // out from under the pointer mid-gesture.
        if (canvas.focus) {
            try { canvas.focus({ preventScroll: true }); } catch (err) { canvas.focus(); }
        }
        pointer(e, 0);
        if (!pannable) e.preventDefault();
    };
    state.pointermove = function(e) { pointer(e, 1); };
    state.pointerup = function(e) { pointer(e, 2); e.preventDefault(); };
    state.pointercancel = function(e) { pointer(e, 3); };
    state.contextmenu = function(e) { e.preventDefault(); };
    // A double-click on a control is a gesture (reset-to-default), never a text
    // selection; selectstart catches the drag-select the pointerdown handler's
    // preventDefault does not.
    state.selectstart = function(e) { e.preventDefault(); };
    state.dblclick = function(e) { e.preventDefault(); };
    state.wheel = function(e) {
        var p = pos(e);
        _pulp_web_on_wheel(p[0], p[1], e.deltaX, e.deltaY, e.deltaMode | 0, mods(e));
        e.preventDefault();
    };
    state.keydown = function(e) {
        var p = stringToNewUTF8(e.key);
        _pulp_web_on_key(p, 1, e.repeat ? 1 : 0, mods(e));
        _free(p);
    };
    state.keyup = function(e) {
        var p = stringToNewUTF8(e.key);
        _pulp_web_on_key(p, 0, 0, mods(e));
        _free(p);
    };

    canvas.addEventListener('pointerdown', state.pointerdown);
    canvas.addEventListener('pointermove', state.pointermove);
    canvas.addEventListener('pointerup', state.pointerup);
    canvas.addEventListener('pointercancel', state.pointercancel);
    canvas.addEventListener('contextmenu', state.contextmenu);
    canvas.addEventListener('selectstart', state.selectstart);
    canvas.addEventListener('dblclick', state.dblclick);
    canvas.addEventListener('wheel', state.wheel, { passive: false });
    // Key events go to the focused element; the canvas only gets them once it
    // is focusable, so make it so and still listen on the window as a backstop
    // for pages that keep focus elsewhere.
    if (!canvas.hasAttribute('tabindex')) canvas.setAttribute('tabindex', '0');
    window.addEventListener('keydown', state.keydown);
    window.addEventListener('keyup', state.keyup);

    state.resize = function() {
        var r = canvas.getBoundingClientRect();
        _pulp_web_on_resize(r.width, r.height);
    };
    window.addEventListener('resize', state.resize);
    if (typeof ResizeObserver !== 'undefined') {
        state.observer = new ResizeObserver(state.resize);
        state.observer.observe(canvas);
    }

    Module.__pulpWebListeners[sel] = state;
});

EM_JS(void, pulp_web_remove_listeners, (const char* selector), {
    var sel = UTF8ToString(selector);
    var all = Module.__pulpWebListeners;
    if (!all || !all[sel]) return;
    var state = all[sel];
    var canvas = document.querySelector(sel);
    if (canvas) {
        canvas.removeEventListener('pointerdown', state.pointerdown);
        canvas.removeEventListener('pointermove', state.pointermove);
        canvas.removeEventListener('pointerup', state.pointerup);
        canvas.removeEventListener('pointercancel', state.pointercancel);
        canvas.removeEventListener('contextmenu', state.contextmenu);
        canvas.removeEventListener('selectstart', state.selectstart);
        canvas.removeEventListener('dblclick', state.dblclick);
        canvas.removeEventListener('wheel', state.wheel);
    }
    window.removeEventListener('keydown', state.keydown);
    window.removeEventListener('keyup', state.keyup);
    window.removeEventListener('resize', state.resize);
    if (state.observer) state.observer.disconnect();
    delete all[sel];
});

EM_JS(double, pulp_web_device_pixel_ratio, (), {
    return window.devicePixelRatio || 1.0;
});

EM_JS(double, pulp_web_canvas_css_width, (const char* selector), {
    var canvas = document.querySelector(UTF8ToString(selector));
    return canvas ? canvas.getBoundingClientRect().width : 0.0;
});

EM_JS(double, pulp_web_canvas_css_height, (const char* selector), {
    var canvas = document.querySelector(UTF8ToString(selector));
    return canvas ? canvas.getBoundingClientRect().height : 0.0;
});

EM_JS(void, pulp_web_set_canvas_size,
      (const char* selector, int backing_w, int backing_h, double css_w,
       double css_h), {
    var canvas = document.querySelector(UTF8ToString(selector));
    if (!canvas) return;
    canvas.width = backing_w;
    canvas.height = backing_h;
    canvas.style.width = css_w + 'px';
    canvas.style.height = css_h + 'px';
});

EM_JS(void, pulp_web_set_canvas_visible, (const char* selector, int visible), {
    var canvas = document.querySelector(UTF8ToString(selector));
    if (!canvas) return;
    if (visible) {
        canvas.style.removeProperty('display');
    } else {
        canvas.style.display = 'none';
    }
});

// ── Public surface (declared in web_event_translate.hpp) ────────────────────

namespace pulp::view::web {

void install_web_input_listeners(const std::string& canvas_selector,
                                 WebInputRouter& router) {
    g_router = &router;
    pulp_web_install_listeners(canvas_selector.c_str());
}

void remove_web_input_listeners(const std::string& canvas_selector) {
    pulp_web_remove_listeners(canvas_selector.c_str());
    g_router = nullptr;
}

float device_pixel_ratio() {
    return static_cast<float>(pulp_web_device_pixel_ratio());
}

bool canvas_css_size(const std::string& canvas_selector, float& css_width,
                     float& css_height) {
    const double w = pulp_web_canvas_css_width(canvas_selector.c_str());
    const double h = pulp_web_canvas_css_height(canvas_selector.c_str());
    if (w <= 0.0 || h <= 0.0) return false;
    css_width = static_cast<float>(w);
    css_height = static_cast<float>(h);
    return true;
}

void set_canvas_size(const std::string& canvas_selector,
                     const BackingStore& backing, float css_width,
                     float css_height) {
    pulp_web_set_canvas_size(canvas_selector.c_str(),
                             static_cast<int>(backing.width),
                             static_cast<int>(backing.height),
                             static_cast<double>(css_width),
                             static_cast<double>(css_height));
}

void set_canvas_visible(const std::string& canvas_selector, bool visible) {
    pulp_web_set_canvas_visible(canvas_selector.c_str(), visible ? 1 : 0);
}

}  // namespace pulp::view::web

#endif  // __EMSCRIPTEN__
