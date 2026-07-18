// widget_bridge/canvas2d_api.cpp - Canvas2D registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "css_color.hpp"
#include <pulp/view/canvas_widget.hpp>
#include <pulp/runtime/base64.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#endif

namespace pulp::view {

namespace {

std::string bridge_base64_encode(const std::vector<uint8_t>& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);

        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < data.size()) ? table[n & 0x3F] : '=';
    }

    return out;
}

} // namespace

void WidgetBridge::register_canvas2d_api() {
    BridgeApiContext api{engine_};
    register_bridge_function(api, "createCanvas", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto c = std::make_unique<CanvasWidget>(); c->set_id(id);
        c->set_native_gpu_texture_provider([this, id]() {
            return this->describe_native_canvas_frame(id);
        });
        widgets_[id] = c.get(); resolve_parent(pid)->add_child(std::move(c));
        return choc::value::createString(id);
    });

    // Canvas drawing
    register_bridge_function(api, "canvasClear", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, ""))))
            c->clear_commands();
        return choc::value::Value();
    });
    // Canvas 2D API — full CanvasRenderingContext2D equivalent
    // Two registered names for the same handler:
    //   * `canvasRect`     — legacy direct-bridge name used by hand-written
    //                        examples (`canvasRect(id, x, y, w, h, "#fff")`).
    //   * `canvasFillRect` — the name the HTML5 Canvas2D shim emits for
    //                        `ctx.fillRect()`.
    // Keep both names so hand-written bridge users and the web-compat shim
    // hit the same fill_rect command path.
    auto canvasRectHandler = [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            // When no color arg was passed (or it was the empty
            // string), honor the active fillStyle (color OR gradient) on
            // the canvas widget. This makes a JS shim like
            //   fillRect(x,y,w,h) { call('canvasFillRect', id, x,y,w,h); }
            // behave like the Canvas2D spec — `ctx.fillRect` paints with
            // whatever `ctx.fillStyle` was last set to, including a
            // CanvasGradient. With 6+ args the explicit color wins.
            const std::string color_str = args.get<std::string>(5, "");
            if (args.size() < 6 || color_str.empty()) {
                cmd.use_active_style = true;
            } else {
                cmd.color = parse_bridge_css_color(color_str);
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    };
    register_bridge_function(api, "canvasRect", canvasRectHandler);
    register_bridge_function(api, "canvasFillRect", canvasRectHandler);

    register_bridge_function(api, "canvasStrokeRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            // Same active-style fallback as canvasRect, applied
            // to strokeStyle. Width arg (index 6) is unaffected.
            const std::string color_str = args.get<std::string>(5, "");
            if (args.size() < 6 || color_str.empty()) {
                cmd.use_active_style = true;
            } else {
                cmd.color = parse_bridge_css_color(color_str);
            }
            cmd.extra = (float)args.get<double>(6, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasFillCircle", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_circle;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.extra=(float)args.get<double>(3,10);
            cmd.color = parse_bridge_css_color(args.get<std::string>(4, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasStrokeLine", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_line;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.color = parse_bridge_css_color(args.get<std::string>(5, "#fff"));
            cmd.extra=(float)args.get<double>(6, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasFillText", [this](choc::javascript::ArgumentList args) {
        auto cid = args.get<std::string>(0, "");
        auto* c = dynamic_cast<CanvasWidget*>(widget(cid));
        if (!c) return choc::value::Value();

        CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_text;

        // Accept BOTH calling conventions.
        //
        // Pulp's web-compat-canvas.js shim emits the 7-arg form:
        //   canvasFillText(id, text, x, y, size, color, maxWidth)
        //
        // Third-party shims bundled with imported designs can emit a 4-arg
        // form with text LAST:
        //   canvasFillText(id, x, y, text)
        //
        // Both are valid JS-side surface contracts. Detect by checking
        // whether slot 1 is a string (web-compat form) or a number
        // (legacy / third-party form). Without this branch, the 4-arg
        // form records an empty fill_text command and drops the intended text.
        std::string slot1_str = args.get<std::string>(1, "");
        const bool is_4arg_form = (args.numArgs == 4) && slot1_str.empty();

        if (is_4arg_form) {
            // canvasFillText(id, x, y, text). Third-party shims that emit
            // this form often do NOT call canvasSetFont first, so the
            // CanvasWidget's command buffer has no `set_font` command ahead
            // of this `fill_text`.
            // At paint time, CGCanvas / SkiaCanvas would create a font
            // with font_size_ = 0 (the canvas's default) → glyphs are
            // 0-pt → text draws invisibly. Inject a default set_font
            // command so the replay establishes a sane font state before
            // this fill_text command renders.
            //
            // Only inject the default set_font when no prior font state has
            // been recorded on this canvas. Scanning the recorded command
            // stream is the canvas's only source of "prior state" — there is
            // no separate accessor. If a caller already issued
            // canvasSetFont / canvasSetFontFull, preserve their state
            // (no override). Same rationale for cmd.color below: a
            // prior canvasSetFillColor (or fill-gradient/pattern) must
            // not be stomped by the hard-coded #fff default — scan
            // back for the most recent fill-style cmd and reuse its
            // color, otherwise fall back to #fff.
            const auto& prior = c->commands();
            bool has_prior_font = false;
            bool has_prior_fill = false;
            canvas::Color prior_fill_color = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
            for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
                if (!has_prior_font &&
                    (it->type == CanvasDrawCmd::Type::set_font ||
                     it->type == CanvasDrawCmd::Type::set_font_full)) {
                    has_prior_font = true;
                }
                if (!has_prior_fill &&
                    (it->type == CanvasDrawCmd::Type::set_fill_color ||
                     it->type == CanvasDrawCmd::Type::set_fill_gradient_linear ||
                     it->type == CanvasDrawCmd::Type::set_fill_gradient_radial ||
                     it->type == CanvasDrawCmd::Type::set_fill_gradient_radial_two_circles ||
                     it->type == CanvasDrawCmd::Type::set_fill_gradient_conic ||
                     it->type == CanvasDrawCmd::Type::set_fill_pattern)) {
                    has_prior_fill = true;
                    prior_fill_color = it->color;
                }
                if (has_prior_font && has_prior_fill) break;
            }

            if (!has_prior_font) {
                CanvasDrawCmd font_cmd;
                font_cmd.type  = CanvasDrawCmd::Type::set_font;
                font_cmd.text  = "system-ui";
                font_cmd.extra = 14.0f;
                c->add_command(font_cmd);
            }

            cmd.x    = (float)args.get<double>(1, 0);
            cmd.y    = (float)args.get<double>(2, 0);
            cmd.text = args.get<std::string>(3, "");
            cmd.extra = 14.0f;                  // default font size px
            // Preserve any prior fill style; only default to white when
            // the caller never set a fill color / gradient / pattern.
            cmd.color = has_prior_fill ? prior_fill_color
                                       : parse_bridge_css_color("#fff");
            cmd.w     = 0.0f;                   // no maxWidth
        } else {
            // canvasFillText(id, text, x, y, size, color, maxWidth)
            cmd.text  = slot1_str;
            cmd.x     = (float)args.get<double>(2, 0);
            cmd.y     = (float)args.get<double>(3, 0);
            cmd.extra = (float)args.get<double>(4, 14);
            cmd.color = parse_bridge_css_color(args.get<std::string>(5, "#fff"));
            // maxWidth threaded as 7th arg in CSS px;
            // `<= 0` or absent means "no constraint".
            cmd.w     = (float)args.get<double>(6, 0.0);
        }

        c->add_command(cmd);
        return choc::value::Value();
    });

    // Dedicated stroke_text bridge entry. canvasStrokeText records a distinct
    // stroke_text cmd so the paint loop can route it through
    // `Canvas::stroke_text` for true outlined rendering on backends that
    // override it (Skia, CG).
    //
    // Args: (id, text, x, y, fontSize, color, maxWidth?). Color carries
    // strokeStyle; lineWidth is set ahead of the call by the JS shim's
    // _syncStrokeState path (canvasSetLineWidth + canvasSetStrokeColor).
    register_bridge_function(api, "canvasStrokeText", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_text;
            cmd.text = args.get<std::string>(1, "");
            cmd.x=(float)args.get<double>(2,0); cmd.y=(float)args.get<double>(3,0);
            cmd.extra=(float)args.get<double>(4, 14);
            cmd.color = parse_bridge_css_color(args.get<std::string>(5, "#fff"));
            cmd.w = (float)args.get<double>(6, 0.0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetFillColor", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_color;
            cmd.color = parse_bridge_css_color(args.get<std::string>(1, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetStrokeColor", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_color;
            cmd.color = parse_bridge_css_color(args.get<std::string>(1, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetLineWidth", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_width;
            cmd.extra = (float)args.get<double>(1, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetFont", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_font;
            cmd.text = args.get<std::string>(1, "Inter");
            cmd.extra = (float)args.get<double>(2, 14);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas2D `ctx.font` full CSS font shorthand. The JS
    // shim parses `[<style>] [<variant>] [<weight>] <size>[/<lineHeight>]
    // <family>` and dispatches here when the parse extracts more than the
    // legacy size+family. Args: (id, family, size, weight, slant,
    // letter_spacing). Slant: 0 = upright, 1 = italic/oblique. Weight:
    // 100..900 (normal=400, bold=700). The `set_font_full` cmd routes to
    // `Canvas::set_font_full` on replay; Skia honours weight/slant via
    // `make_font(family, size, weight, slant)`. CG falls through to the
    // base default (family+size only).
    register_bridge_function(api, "canvasSetFontFull", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_font_full;
            cmd.text  = args.get<std::string>(1, "Inter");
            cmd.extra = (float)args.get<double>(2, 14);
            cmd.x     = (float)args.get<double>(3, 400);   // weight
            cmd.y     = (float)args.get<double>(4, 0);     // slant
            cmd.x2    = (float)args.get<double>(5, 0);     // letter_spacing
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Path operations
    register_bridge_function(api, "canvasBeginPath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::begin_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasMoveTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::move_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasLineTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::line_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasQuadTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::quad_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.x2=(float)args.get<double>(3,0); cmd.y2=(float)args.get<double>(4,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasCubicTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::cubic_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.x2=(float)args.get<double>(3,0); cmd.y2=(float)args.get<double>(4,0);
            cmd.x3=(float)args.get<double>(5,0); cmd.y3=(float)args.get<double>(6,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasClosePath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::close_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasFillPath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            // Read the optional fillRule int (0 = nonzero, 1 = evenodd)
            // so the spec arg threads into the recorded command. The
            // Skia / CG paint sides key on int_val for fill_path / clip.
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_path;
            cmd.int_val = static_cast<int>(args.get<double>(1, 0));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasStrokePath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Native arc subpaths. Each replaces the JS shim's
    // bezier approximation so Skia / CG see real arc geometry. Args:
    //   canvasPathArc(id, cx, cy, radius, startAngle, endAngle,
    //                 anticlockwise:0/1)
    //   canvasPathArcTo(id, x1, y1, x2, y2, radius)
    //   canvasPathEllipse(id, cx, cy, rx, ry, rotation, startAngle,
    //                     endAngle, anticlockwise:0/1)
    //   canvasPathRoundRect(id, x, y, w, h,
    //                       tl_x, tl_y, tr_x, tr_y,
    //                       br_x, br_y, bl_x, bl_y)
    register_bridge_function(api, "canvasPathArc", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::path_arc;
            cmd.x     = (float)args.get<double>(1, 0);
            cmd.y     = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 0);
            cmd.x2    = (float)args.get<double>(4, 0);
            cmd.y2    = (float)args.get<double>(5, 0);
            cmd.int_val = args.get<double>(6, 0) != 0.0 ? 1 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasPathArcTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::path_arc_to;
            cmd.x     = (float)args.get<double>(1, 0);
            cmd.y     = (float)args.get<double>(2, 0);
            cmd.x2    = (float)args.get<double>(3, 0);
            cmd.y2    = (float)args.get<double>(4, 0);
            cmd.extra = (float)args.get<double>(5, 0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasPathEllipse", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::path_ellipse;
            cmd.x     = (float)args.get<double>(1, 0);   // cx
            cmd.y     = (float)args.get<double>(2, 0);   // cy
            cmd.w     = (float)args.get<double>(3, 0);   // rx
            cmd.h     = (float)args.get<double>(4, 0);   // ry
            cmd.extra = (float)args.get<double>(5, 0);   // rotation
            cmd.x2    = (float)args.get<double>(6, 0);   // startAngle
            cmd.y2    = (float)args.get<double>(7, 0);   // endAngle
            cmd.int_val = args.get<double>(8, 0) != 0.0 ? 1 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasPathRoundRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::path_round_rect;
            cmd.x = (float)args.get<double>(1, 0);
            cmd.y = (float)args.get<double>(2, 0);
            cmd.w = (float)args.get<double>(3, 0);
            cmd.h = (float)args.get<double>(4, 0);
            cmd.gradient_positions = {
                (float)args.get<double>(5, 0),  (float)args.get<double>(6, 0),
                (float)args.get<double>(7, 0),  (float)args.get<double>(8, 0),
                (float)args.get<double>(9, 0),  (float)args.get<double>(10, 0),
                (float)args.get<double>(11, 0), (float)args.get<double>(12, 0),
            };
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // State
    register_bridge_function(api, "canvasSave", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::save; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasRestore", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::restore; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Transform
    register_bridge_function(api, "canvasTranslate", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::translate;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasScale", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::scale;
            cmd.x=(float)args.get<double>(1,1); cmd.y=(float)args.get<double>(2,1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasRotate", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::rotate;
            cmd.extra=(float)args.get<double>(1,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Path builder API from JS
    register_bridge_function(api, "beginPath", [](choc::javascript::ArgumentList) {
        // Legacy no-op; CanvasWidget path replay uses canvasBeginPath.
        return choc::value::Value();
    });

    // drawPath(canvasId, commands) — draw a path on a CanvasWidget
    // Commands: "M x y" (move), "L x y" (line), "Q cx cy x y" (quad), "C c1x c1y c2x c2y x y" (cubic), "Z" (close)
    register_bridge_function(api, "drawPath", [](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pathStr = args.get<std::string>(1, "");
        auto fillHex = args.get<std::string>(2, "");
        auto strokeHex = args.get<std::string>(3, "");
        auto lineW = static_cast<float>(args.get<double>(4, 1.0));
        (void)id; (void)pathStr; (void)fillHex; (void)strokeHex; (void)lineW;
        // Intentionally unimplemented legacy helper; Canvas2D shims use
        // canvasMoveTo / canvasLineTo / canvasFillPath instead.
        return choc::value::Value();
    });

    // measureText(text, fontSize) → {width, ascent, descent, lineHeight}
    register_bridge_function(api, "measureText", [](choc::javascript::ArgumentList args) {
        auto text = args.get<std::string>(0, "");
        auto size = static_cast<float>(args.get<double>(1, 14.0));
        // Return approximate metrics. Exact per-canvas metrics use canvasMeasureText.
        float width = static_cast<float>(text.size()) * size * 0.6f;
        float ascent = size * 0.75f;
        float descent = size * 0.25f;
        float lineHeight = size * 1.4f;
        auto result = choc::value::createObject("");
        result.addMember("width", choc::value::createFloat64(width));
        result.addMember("ascent", choc::value::createFloat64(ascent));
        result.addMember("descent", choc::value::createFloat64(descent));
        result.addMember("lineHeight", choc::value::createFloat64(lineHeight));
        return result;
    });

    // Canvas gradient fills
    register_bridge_function(api, "canvasSetLinearGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_gradient_linear;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.x2 = (float)args.get<double>(3, 0); cmd.y2 = (float)args.get<double>(4, 1);
            // Parse color stops from remaining args: color1, pos1, color2, pos2, ...
            for (int i = 5; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parse_bridge_css_color(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetRadialGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_gradient_radial;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 50); // radius
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parse_bridge_css_color(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // True two-circle radial gradient. Skia routes through
    // SkGradientShader::MakeTwoPointConical; CG routes through
    // CGContextDrawRadialGradient with both circles wired (the prior
    // single-circle bridge silently dropped (x0, y0, r0) which broke
    // offset / sized inner-circle gradients on both backends).
    // Args: (id, x0, y0, r0, x1, y1, r1, color1, pos1, color2, pos2, ...)
    register_bridge_function(api, "canvasSetRadialGradientTwoCircles",
            [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd;
            cmd.type = CanvasDrawCmd::Type::set_fill_gradient_radial_two_circles;
            // Inner circle (x0, y0, r0) → (x, y, extra).
            cmd.x = (float)args.get<double>(1, 0);
            cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 0);
            // Outer circle (x1, y1, r1) → (x2, y2, w).
            cmd.x2 = (float)args.get<double>(4, 0);
            cmd.y2 = (float)args.get<double>(5, 0);
            cmd.w  = (float)args.get<double>(6, 50);
            for (int i = 7; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parse_bridge_css_color(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasClearGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clear_fill_gradient;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // `ctx.strokeStyle = createLinearGradient(...)`.
    // Mirror of canvasSetLinearGradient targeting the new
    // `Canvas::set_stroke_gradient_linear` virtual. The JS shim's
    // _applyStrokeStyle dispatches here when the bridge fn is present;
    // older binaries fall back to the first-stop solid color without
    // crashing. Stops are color/position pairs starting at arg index 5,
    // matching the fill counterpart so the JS shim shares its packing
    // logic.
    register_bridge_function(api, "canvasSetStrokeLinearGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_gradient_linear;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.x2 = (float)args.get<double>(3, 0); cmd.y2 = (float)args.get<double>(4, 1);
            for (int i = 5; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parse_bridge_css_color(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Single-circle radial. Args: (id, cx, cy, radius, color1, pos1, ...).
    register_bridge_function(api, "canvasSetStrokeRadialGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_gradient_radial;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 50);
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parse_bridge_css_color(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Two-circle radial. Args: (id, x0, y0, r0, x1, y1, r1, color1, pos1, ...).
    register_bridge_function(api, "canvasSetStrokeRadialGradientTwoCircles",
            [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd;
            cmd.type = CanvasDrawCmd::Type::set_stroke_gradient_radial_two_circles;
            cmd.x = (float)args.get<double>(1, 0);
            cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 0);
            cmd.x2 = (float)args.get<double>(4, 0);
            cmd.y2 = (float)args.get<double>(5, 0);
            cmd.w  = (float)args.get<double>(6, 50);
            for (int i = 7; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parse_bridge_css_color(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Conic / sweep. Args: (id, cx, cy, startAngle, color1, pos1, ...).
    register_bridge_function(api, "canvasSetStrokeConicGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_gradient_conic;
            cmd.x = (float)args.get<double>(1, 0);
            cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 0);
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parse_bridge_css_color(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Reset stroke shader → solid stroke color.
    register_bridge_function(api, "canvasClearStrokeGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clear_stroke_gradient;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ctx.createConicGradient. Skia routes through
    // SkGradientShader::MakeSweep; CoreGraphics software-rasterizes the
    // conic sweep into a cached CGImage.
    // Args: (id, cx, cy, startAngle, color1, pos1, color2, pos2, ...)
    register_bridge_function(api, "canvasSetConicGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_gradient_conic;
            cmd.x = (float)args.get<double>(1, 0);   // cx
            cmd.y = (float)args.get<double>(2, 0);   // cy
            cmd.extra = (float)args.get<double>(3, 0); // start_angle (radians)
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parse_bridge_css_color(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ctx.createPattern. Fill patterns route through backend image-pattern
    // support (SkShader / CGPattern). Stroke patterns remain a separate
    // command because some backends support fills without stroke patterns.
    //
    // Args: (id, src, tile_x, tile_y)
    //   src      — image source (file path, "data:" URL, or "" for clear)
    //   tile_x   — "repeat" | "no-repeat"
    //   tile_y   — "repeat" | "no-repeat"
    register_bridge_function(api, "canvasSetFillPattern", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_pattern;
            cmd.text = args.get<std::string>(1, "");          // image source
            auto tx = args.get<std::string>(2, "repeat");
            auto ty = args.get<std::string>(3, "repeat");
            // Pack tile modes into int_val (bit 0 = x, bit 1 = y);
            // 0 = repeat, 1 = no-repeat. Mirrors set_image_smoothing's
            // pattern of folding multiple enum values into one int slot.
            int tx_i = (tx == "no-repeat") ? 1 : 0;
            int ty_i = (ty == "no-repeat") ? 1 : 0;
            cmd.int_val = tx_i | (ty_i << 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Stroke counterpart — same shape, different command type. Routes
    // through set_stroke_pattern on the live canvas; CG falls back to
    // solid stroke color.
    register_bridge_function(api, "canvasSetStrokePattern", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_pattern;
            cmd.text = args.get<std::string>(1, "");
            auto tx = args.get<std::string>(2, "repeat");
            auto ty = args.get<std::string>(3, "repeat");
            int tx_i = (tx == "no-repeat") ? 1 : 0;
            int ty_i = (ty == "no-repeat") ? 1 : 0;
            cmd.int_val = tx_i | (ty_i << 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ctx.miterLimit. Sticky stroke
    // state honoured by SkPaint::setStrokeMiter (Skia) and
    // CGContextSetMiterLimit (CG). Spec: non-positive / non-finite
    // values are silently ignored — backends do the clamp.
    // Args: (id, limit)
    register_bridge_function(api, "canvasSetMiterLimit", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_miter_limit;
            cmd.extra = (float)args.get<double>(1, 10.0); // spec default = 10
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ctx.imageSmoothingEnabled +
    // ctx.imageSmoothingQuality. Sticky paint flag honoured on the next
    // drawImage. Skia translates to SkSamplingOptions, CG to
    // CGContextSetInterpolationQuality.
    // Args: (id, enabled[, quality]) where quality ∈ "low" | "medium" | "high".
    register_bridge_function(api, "canvasSetImageSmoothing", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_image_smoothing;
            cmd.int_val = args.get<bool>(1, true) ? 1 : 0;
            auto q = args.get<std::string>(2, "low");
            int qi = 0;
            if (q == "medium") qi = 1;
            else if (q == "high") qi = 2;
            cmd.extra = static_cast<float>(qi);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas2D ctx.direction. Sticky text-shaping state
    // honoured by the SkShaper / HarfBuzz path on the next fillText
    // / strokeText. The shim coerces unknown strings to "ltr" before
    // hitting the bridge, so we accept the resolved enum directly.
    // Args: (id, enumVal) where enumVal ∈ 0=ltr | 1=rtl | 2=inherit.
    register_bridge_function(api, "canvasSetDirection", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_direction;
            int v = static_cast<int>(args.get<double>(1, 0.0));
            if (v < 0 || v > 2) v = 0;
            cmd.int_val = v;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas2D ctx.filter. Sticky CSS <filter-function-list>
    // string applied to subsequent fill/stroke/text/image draws. Skia
    // parses into an SkImageFilter chain (blur, grayscale, sepia, …);
    // RecordingCanvas captures the raw string for harness assertions;
    // CG / minimal backends accept the no-op default and render unfiltered.
    // Args: (id, cssFilterString) — "none" disables.
    register_bridge_function(api, "canvasSetFilter", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_filter;
            cmd.text = args.get<std::string>(1, "none");
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas arc — for pie charts, circular progress, arcs
    register_bridge_function(api, "canvasArc", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_arc;
            cmd.x = (float)args.get<double>(1, 0);     // cx
            cmd.y = (float)args.get<double>(2, 0);     // cy
            cmd.w = (float)args.get<double>(3, 50);    // radius
            cmd.x2 = (float)args.get<double>(4, 0);    // startAngle
            cmd.y2 = (float)args.get<double>(5, 6.28); // endAngle
            cmd.color = parse_bridge_css_color(args.get<std::string>(6, "#fff"));
            cmd.extra = (float)args.get<double>(7, 1);  // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas textAlign / textBaseline
    register_bridge_function(api, "canvasSetTextAlign", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_text_align;
            auto align = args.get<std::string>(1, "left");
            cmd.int_val = (align == "center") ? 1 : (align == "right") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetTextBaseline", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_text_baseline;
            auto bl = args.get<std::string>(1, "top");
            cmd.int_val = (bl == "middle") ? 1 : (bl == "bottom") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas clearRect
    register_bridge_function(api, "canvasClearRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clear_rect;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.w = (float)args.get<double>(3, 0); cmd.h = (float)args.get<double>(4, 0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas clipRect
    register_bridge_function(api, "canvasClipRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clip_rect;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.w = (float)args.get<double>(3, 0); cmd.h = (float)args.get<double>(4, 0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas fillRoundedRect / strokeRoundedRect / strokeCircle
    register_bridge_function(api, "canvasFillRoundedRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_rounded_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.extra=(float)args.get<double>(5,0); // radius
            cmd.color = parse_bridge_css_color(args.get<std::string>(6, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasStrokeRoundedRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_rounded_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.extra=(float)args.get<double>(5,0); // radius
            cmd.color = parse_bridge_css_color(args.get<std::string>(6, "#fff"));
            cmd.x2=(float)args.get<double>(7,1); // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasStrokeCircle", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_circle;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.extra=(float)args.get<double>(3,10); // radius
            cmd.color = parse_bridge_css_color(args.get<std::string>(4, "#fff"));
            cmd.x2=(float)args.get<double>(5,1); // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas globalAlpha
    register_bridge_function(api, "canvasSetGlobalAlpha", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_global_alpha;
            cmd.extra = (float)args.get<double>(1, 1.0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Canvas lineCap / lineJoin
    register_bridge_function(api, "canvasSetLineCap", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_cap;
            auto cap = args.get<std::string>(1, "butt");
            cmd.int_val = (cap == "round") ? 1 : (cap == "square") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetLineJoin", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_join;
            auto join = args.get<std::string>(1, "miter");
            cmd.int_val = (join == "round") ? 1 : (join == "bevel") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // CSS globalCompositeOperation → Canvas::BlendMode index. Returns -1
    // for unknown strings so callers can no-op gracefully.
    auto cssCompositeOpToBlendModeIndex = [](const std::string& mode) -> int {
        // Indices below MUST match Canvas::BlendMode in core/canvas/include/pulp/canvas/canvas.hpp.
        if (mode == "source-over")      return 16; // also accepted at index 0 (normal)
        if (mode == "destination-over") return 17;
        if (mode == "source-in")        return 18;
        if (mode == "destination-in")   return 19;
        if (mode == "source-out")       return 20;
        if (mode == "destination-out")  return 21;
        if (mode == "source-atop")      return 22;
        if (mode == "destination-atop") return 23;
        if (mode == "xor")              return 24;
        if (mode == "copy")             return 25;
        if (mode == "lighter")          return 26;
        // W3C advanced blend modes (indices match enum order)
        if (mode == "multiply")     return 1;
        if (mode == "screen")       return 2;
        if (mode == "overlay")      return 3;
        if (mode == "darken")       return 4;
        if (mode == "lighten")      return 5;
        if (mode == "color-dodge")  return 6;
        if (mode == "color-burn")   return 7;
        if (mode == "hard-light")   return 8;
        if (mode == "soft-light")   return 9;
        if (mode == "difference")   return 10;
        if (mode == "exclusion")    return 11;
        if (mode == "hue")          return 12;
        if (mode == "saturation")   return 13;
        if (mode == "color")        return 14;
        if (mode == "luminosity")   return 15;
        return -1; // unknown — caller treats as no-op
    };

    // Canvas globalCompositeOperation (blend mode) — back-compat alias
    register_bridge_function(api, "canvasSetBlendMode", [this, cssCompositeOpToBlendModeIndex](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            auto mode = args.get<std::string>(1, "source-over");
            int idx = cssCompositeOpToBlendModeIndex(mode);
            if (idx < 0) return choc::value::Value(); // unknown string → no-op
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_blend_mode;
            cmd.int_val = idx;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // canvasGlobalCompositeOperation — full CanvasRenderingContext2D
    // globalCompositeOperation surface. Accepts every standard
    // CSS string and falls back to no-op on unknown values.
    register_bridge_function(api, "canvasGlobalCompositeOperation", [this, cssCompositeOpToBlendModeIndex](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            auto mode = args.get<std::string>(1, "source-over");
            int idx = cssCompositeOpToBlendModeIndex(mode);
            if (idx < 0) return choc::value::Value(); // unknown — graceful no-op
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_blend_mode;
            cmd.int_val = idx;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // canvasSetTransform(id, a, b, c, d, e, f) — replace current transform
    // with the affine matrix. Used for devicePixelRatio scaling
    // (ctx.setTransform(scale, 0, 0, scale, 0, 0)).
    register_bridge_function(api, "canvasSetTransform", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_transform;
            cmd.x  = (float)args.get<double>(1, 1.0); // a (scaleX)
            cmd.y  = (float)args.get<double>(2, 0.0); // b (skewY)
            cmd.w  = (float)args.get<double>(3, 0.0); // c (skewX)
            cmd.h  = (float)args.get<double>(4, 1.0); // d (scaleY)
            cmd.x2 = (float)args.get<double>(5, 0.0); // e (translateX)
            cmd.y2 = (float)args.get<double>(6, 0.0); // f (translateY)
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // canvasClip(id, fillRule?) — intersect clip region with current path.
    // Also threads optional fillRule int (0 = nonzero, 1 = evenodd).
    // Same packing as canvasFillPath above.
    register_bridge_function(api, "canvasClip", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clip;
            cmd.int_val = static_cast<int>(args.get<double>(1, 0));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ═══════════════════════════════════════════════════════════════════
    // Final Canvas2D API coverage
    // ═══════════════════════════════════════════════════════════════════

    // Canvas drawImage(canvasId, imagePath, dx, dy, dw, dh)
    //   or 9-arg form: canvasDrawImage(id, path, dx,dy,dw,dh, sx,sy,sw,sh)
    // When args[6..9] are present the JS shim is using the
    // source-rect 9-arg form. Bridge stashes it in x2/y2/x3/y3 + sets
    // has_source_rect; the canvas_widget renderer routes through the
    // _rect overload so the source sub-rectangle lands on the dst rect.
    register_bridge_function(api, "canvasDrawImage", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::draw_image;
            cmd.text = args.get<std::string>(1, ""); // image source path
            cmd.x = (float)args.get<double>(2, 0);
            cmd.y = (float)args.get<double>(3, 0);
            cmd.w = (float)args.get<double>(4, 0);
            cmd.h = (float)args.get<double>(5, 0);
            // 9-arg drawImage source rect — JS appends sx,sy,sw,sh
            // when the caller used the 9-arg form. choc reports
            // missing args as defaults, so we gate on numArgs >= 10.
            if (args.numArgs >= 10) {
                cmd.x2 = (float)args.get<double>(6, 0);
                cmd.y2 = (float)args.get<double>(7, 0);
                cmd.x3 = (float)args.get<double>(8, 0);
                cmd.y3 = (float)args.get<double>(9, 0);
                cmd.has_source_rect = true;
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ═══════════════════════════════════════════════════════════════════
    // Canvas2D API coverage
    // ═══════════════════════════════════════════════════════════════════

    // canvasMeasureText(id, text, fontFamily, fontSize) →
    //   { width, actualBoundingBoxLeft, actualBoundingBoxRight,
    //     actualBoundingBoxAscent, actualBoundingBoxDescent,
    //     fontBoundingBoxAscent, fontBoundingBoxDescent }
    //
    // Per-canvas measureText routes through the canvas's font state — this
    // matters because each canvas can carry its own font setting via
    // canvasSetFont. When a Skia surface isn't available (RecordingCanvas,
    // CG fallback) we return font-size estimates so JS callers always get
    // a populated TextMetrics object with non-zero width for non-empty
    // text. (HTML5 spec — TextMetrics never has missing fields.)
    register_bridge_function(api, "canvasMeasureText", [this](choc::javascript::ArgumentList args) {
        auto text = args.get<std::string>(1, "");
        auto family = args.get<std::string>(2, "Inter");
        float size = static_cast<float>(args.get<double>(3, 14.0));

        canvas::Canvas::TextMetrics m;
#if defined(PULP_HAS_SKIA)
        // Skia-backed accurate metrics — uses SkFont::measureText() which
        // is what fill_text() ultimately renders against, so the returned
        // bbox matches the drawn text. No surface required.
        m = canvas::SkiaCanvas::measure_text_with_font(family, size, text);
#else
        // CPU fallback — keep all fields populated so JS centring works
        // (HTML5 spec: TextMetrics never has missing fields).
        m.width = static_cast<float>(text.size()) * size * 0.6f;
        m.ascent = size * 0.75f;
        m.descent = size * 0.25f;
        m.line_height = size * 1.2f;
        m.actual_bounding_box_left = 0;
        m.actual_bounding_box_right = m.width;
        m.actual_bounding_box_ascent = m.ascent;
        m.actual_bounding_box_descent = m.descent;
#endif

        // (void)c — measureText doesn't need to mutate the canvas. Calling
        // with a missing id still returns a valid metrics object so callers
        // can measure before getContext('2d').
        (void)widget(args.get<std::string>(0, ""));

        auto result = choc::value::createObject("");
        result.addMember("width", choc::value::createFloat64(m.width));
        result.addMember("actualBoundingBoxLeft",
                          choc::value::createFloat64(m.actual_bounding_box_left));
        result.addMember("actualBoundingBoxRight",
                          choc::value::createFloat64(m.actual_bounding_box_right));
        result.addMember("actualBoundingBoxAscent",
                          choc::value::createFloat64(m.actual_bounding_box_ascent));
        result.addMember("actualBoundingBoxDescent",
                          choc::value::createFloat64(m.actual_bounding_box_descent));
        result.addMember("fontBoundingBoxAscent",
                          choc::value::createFloat64(m.ascent));
        result.addMember("fontBoundingBoxDescent",
                          choc::value::createFloat64(m.descent));
        return result;
    });

    // canvasSetLineDash(id, [a, b, c, ...], phase = 0)
    // Pattern is an HTML5-style array; an odd-length array is
    // duplicated to even per the spec — the JS prelude handles that
    // before calling here.
    register_bridge_function(api, "canvasSetLineDash", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_dash;
            cmd.extra = static_cast<float>(args.get<double>(2, 0.0)); // phase
            // Pattern: pulled from arg[1]. Choc exposes JS arrays as
            // indexable ValueViews here, matching other bridge array handlers.
            // Reuse cmd.gradient_positions to avoid expanding CanvasDrawCmd.
            if (args.numArgs > 1 && args[1]) {
                auto& pattern = *args[1];
                cmd.gradient_positions.reserve(pattern.size());
                for (uint32_t i = 0; i < pattern.size(); ++i) {
                    cmd.gradient_positions.push_back(
                        static_cast<float>(pattern[i].getWithDefault<double>(0.0)));
                }
                // HTML5: drop the entire pattern if any value is negative
                // or non-finite — spec says behavior is implementation-
                // defined; we choose graceful "solid stroke".
                bool valid = true;
                for (float v : cmd.gradient_positions) {
                    if (!(v >= 0.0f) || !std::isfinite(v)) { valid = false; break; }
                }
                if (!valid) cmd.gradient_positions.clear();
                // HTML5 spec: odd-length patterns are duplicated.
                if (cmd.gradient_positions.size() % 2 == 1) {
                    auto orig = cmd.gradient_positions;
                    cmd.gradient_positions.insert(cmd.gradient_positions.end(),
                                                  orig.begin(), orig.end());
                }
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // ── Canvas2D shadow* state ──────────────────────────────────────────────
    //
    // Sticky drop-shadow state that wraps subsequent fill/stroke/text
    // draws — matching `CanvasRenderingContext2D.shadowColor` /
    // `shadowBlur` / `shadowOffsetX` / `shadowOffsetY`. Each setter
    // records one CanvasDrawCmd that the paint dispatch flushes through
    // to the underlying canvas (Skia → SkImageFilters::DropShadow,
    // CoreGraphics → CGContextSetShadowWithColor). The shadow is gated
    // on color.a > 0 AND (blur > 0 OR offset_x != 0 OR offset_y != 0)
    // by the canvas backends; the bridge captures every assignment so
    // the state stays in lockstep with the JS-side `ctx.shadow*`
    // properties even when one of them is set to 0/transparent.
    //
    // Shadow color is parsed via the shared `parse_bridge_css_color` helper used
    // by `canvasSetFillStyle` etc., so all the CSS color forms
    // (`#rgb`, `#rrggbb`, `rgba(...)`, `hsl(...)`, `transparent`,
    // `red`, …) work uniformly.
    register_bridge_function(api, "canvasSetShadowColor", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_shadow_color;
            cmd.color = parse_bridge_css_color(args.get<std::string>(1, "rgba(0,0,0,0)"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetShadowBlur", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_shadow_blur;
            // HTML5 spec: shadowBlur must be non-negative finite; reject
            // negatives at the boundary so the canvas backends don't have
            // to redo the validation.
            double blur = args.get<double>(1, 0.0);
            cmd.extra = static_cast<float>(blur >= 0.0 ? blur : 0.0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetShadowOffsetX", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_shadow_offset_x;
            cmd.extra = static_cast<float>(args.get<double>(1, 0.0));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    register_bridge_function(api, "canvasSetShadowOffsetY", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_shadow_offset_y;
            cmd.extra = static_cast<float>(args.get<double>(1, 0.0));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // canvasGetImageData(id, x, y, w, h) →
    //   { width, height, data: <base64 string of RGBA bytes> }
    //
    // Returns the pixel data of the canvas's currently rasterized
    // surface. On non-Skia / non-rasterized backends this returns an
    // empty buffer of the requested size; callers should treat that as
    // "not available" and fall back to whatever they'd do with a stub
    // 1×1 placeholder. The base64 wrapping keeps the result usable
    // across QuickJS / JSC / V8 without a Uint8ClampedArray bridge.
    register_bridge_function(api, "canvasGetImageData", [this](choc::javascript::ArgumentList args) {
        int w = static_cast<int>(args.get<double>(3, 0.0));
        int h = static_cast<int>(args.get<double>(4, 0.0));
        if (w < 0) w = 0;
        if (h < 0) h = 0;

        std::vector<uint8_t> pixels(static_cast<size_t>(w) *
                                     static_cast<size_t>(h) * 4u, 0u);

        // The bridge has no direct access to the live render surface
        // (CanvasWidget paints into whatever canvas the host provides
        // each frame), so getImageData over a JS-recorded command list
        // can only return zeros until a render-host integration lands.
        // We still validate the canvas id so JS sees a well-formed
        // result with the right dimensions (matching HTML5 spec for
        // out-of-bounds reads).
        (void)widget(args.get<std::string>(0, ""));

        auto result = choc::value::createObject("");
        result.addMember("width",  choc::value::createInt32(w));
        result.addMember("height", choc::value::createInt32(h));
        result.addMember("data",   choc::value::createString(
            bridge_base64_encode(pixels)));
        return result;
    });

    // canvasPutImageData(id, base64Pixels, width, height, dx, dy)
    //
    // Decodes `base64Pixels` (expected width*height*4 RGBA bytes) and
    // records a put_image_data command. The widget's paint() pass
    // applies it via Canvas::write_pixels() — currently only Skia
    // implements that end-to-end; other backends are a no-op.
    register_bridge_function(api, "canvasPutImageData", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            auto b64 = args.get<std::string>(1, "");
            int width  = static_cast<int>(args.get<double>(2, 0.0));
            int height = static_cast<int>(args.get<double>(3, 0.0));
            int dx     = static_cast<int>(args.get<double>(4, 0.0));
            int dy     = static_cast<int>(args.get<double>(5, 0.0));
            if (width <= 0 || height <= 0) return choc::value::Value();

            auto bytes = runtime::base64_decode(b64);
            if (!bytes) return choc::value::Value();
            const size_t expected = static_cast<size_t>(width) *
                                     static_cast<size_t>(height) * 4u;
            if (bytes->size() < expected) return choc::value::Value();

            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::put_image_data;
            cmd.int_val = width;
            cmd.x2      = static_cast<float>(height);
            cmd.x       = static_cast<float>(dx);
            cmd.y       = static_cast<float>(dy);
            cmd.text.assign(reinterpret_cast<const char*>(bytes->data()), expected);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

}

} // namespace pulp::view
