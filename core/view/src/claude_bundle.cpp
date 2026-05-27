// claude_bundle.cpp — Claude Design bundle import + runtime-import
// machinery, extracted from design_import.cpp in the 2026-05 Phase 6
// (A3) refactor.
//
// This is the pulp #468 runtime-import subsystem — the largest single
// cluster in the old design_import.cpp:
//
//   * Claude Design bundle envelope parsing (asset extraction, zip /
//     base64 payloads).
//   * parse_claude_html_with_runtime — the offline harness that boots
//     a ScriptEngine/View/WidgetBridge, evaluates the bundle, and walks
//     the materialized DOM into a DesignIR.
//   * the shared pre-payload shim + payload pipeline used by both the
//     offline harness and the live-engine path.
//   * WidgetBridge::evaluate_claude_bundle_in_live_engine — the live
//     runtime path that reuses the caller's already-mounted reconciler.
//
// Definitions only; declarations stay in pulp/view/design_import.hpp
// (and widget_bridge.hpp for the WidgetBridge member). Relocated so
// runtime-import work no longer recompiles the whole importer.

#include <pulp/view/design_import.hpp>
#include <pulp/view/anchor_strategy.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/zip.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/state/store.hpp>
#include <choc/text/choc_JSON.h>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>
#include <cmath>

#include "design_import_internal.hpp"

namespace pulp::view {

// ── Claude Design bundle envelope (pulp #468) ───────────────────────────

namespace {

// Strip the gzip header (RFC 1952) from `data`, leaving only the raw
// deflate payload that miniz's `deflate_decompress` can handle.
//
// Header layout:
//   1f 8b   - magic
//   08      - compression method (always deflate)
//   FLG     - flags byte (FTEXT / FHCRC / FEXTRA / FNAME / FCOMMENT)
//   MTIME   - 4 bytes
//   XFL     - 1 byte
//   OS      - 1 byte
//   [FEXTRA: 2-byte len + data]
//   [FNAME: NUL-terminated]
//   [FCOMMENT: NUL-terminated]
//   [FHCRC: 2-byte CRC]
//
// `pulp::runtime::gzip_decompress` is misnamed (it actually does zlib)
// so we can't reuse it for real RFC 1952 streams. This shim handles the
// envelope's compressed payloads (which always start with 1f 8b) without
// having to touch the runtime layer.
std::optional<std::vector<uint8_t>> claude_bundle_inflate(const uint8_t* data, size_t size) {
    if (size < 18) return std::nullopt;       // header + min trailer
    if (data[0] != 0x1f || data[1] != 0x8b) return std::nullopt; // magic
    if (data[2] != 0x08) return std::nullopt; // compression method
    const uint8_t flg = data[3];
    size_t off = 10;                          // fixed header

    auto read_u16 = [&](size_t at) -> uint16_t {
        return static_cast<uint16_t>(data[at]) |
               (static_cast<uint16_t>(data[at + 1]) << 8);
    };

    if (flg & 0x04) { // FEXTRA
        if (off + 2 > size) return std::nullopt;
        uint16_t xlen = read_u16(off);
        off += 2 + xlen;
    }
    if (flg & 0x08) { // FNAME (NUL-terminated)
        while (off < size && data[off] != 0) ++off;
        if (off >= size) return std::nullopt;
        ++off;
    }
    if (flg & 0x10) { // FCOMMENT
        while (off < size && data[off] != 0) ++off;
        if (off >= size) return std::nullopt;
        ++off;
    }
    if (flg & 0x02) { // FHCRC
        off += 2;
    }

    // 8-byte trailer: CRC32 + ISIZE.
    if (off + 8 > size) return std::nullopt;
    const size_t deflate_len = size - off - 8;
    return pulp::runtime::deflate_decompress(data + off, deflate_len);
}

} // namespace

namespace {

// Find a `<script type="__bundler/{manifest|template}">...</script>` tag
// and return its inner text. The bundler shape is fixed: opening
// `<script ...>` tag with the `type` attribute, content (which is JSON
// — base64 + alphanumerics for manifest, JSON-encoded HTML with
// `</script>` for template), then `</script>`.
//
// IMPORTANT: a real Claude Design export contains the inline bundler
// boot code that itself references `'__bundler/manifest'` and
// `'__bundler/template'` as string literals. A naive substring search
// matches those first. We anchor instead on the literal sequence
// `<script` that opens an HTML tag, then on the type attribute, so the
// boot-code references can't masquerade as the real bundler tags.
std::optional<std::string> extract_bundler_tag_content(const std::string& html,
                                                       const std::string& tag_type) {
    const std::string opener_dq = "<script type=\"" + tag_type + "\"";
    const std::string opener_sq = "<script type='" + tag_type + "'";
    size_t tag_start = html.find(opener_dq);
    size_t header_len = opener_dq.size();
    if (tag_start == std::string::npos) {
        tag_start = html.find(opener_sq);
        header_len = opener_sq.size();
        if (tag_start == std::string::npos) return std::nullopt;
    }
    // Find the '>' that closes the opening <script ...> tag (must be
    // beyond the type attribute we just matched).
    size_t open_end = html.find('>', tag_start + header_len);
    if (open_end == std::string::npos) return std::nullopt;
    // Find the closing </script>.
    size_t close = html.find("</script>", open_end + 1);
    if (close == std::string::npos) return std::nullopt;
    return html.substr(open_end + 1, close - (open_end + 1));
}

// Pull every <script src="..."> uuid out of the template HTML, in order.
// The template is JSON-encoded HTML — `<` characters are literal, not
// escaped — so a regex over the raw string is fine.
std::vector<std::string> extract_template_script_srcs(const std::string& template_html) {
    std::vector<std::string> srcs;
    // C++ ECMAScript regex: explicit alternation handles single-quote
    // and double-quote attribute values. The character classes inside
    // a raw string literal would be brittle (`["']` reads as four chars
    // `"`, `'` rather than the intended class) — alternation keeps
    // the parse unambiguous.
    static const std::regex re(
        R"RX(<script\b[^>]*\bsrc=(?:"([^"]+)"|'([^']+)'))RX",
        std::regex::icase);
    auto begin = std::sregex_iterator(template_html.begin(), template_html.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        if ((*it)[1].matched) srcs.push_back((*it)[1].str());
        else if ((*it)[2].matched) srcs.push_back((*it)[2].str());
    }
    return srcs;
}

// One inline `<script>` tag pulled out of the template HTML.
// `kind` is normalized to one of:
//   "javascript" — `type` missing, empty, or text/javascript / application/javascript
//   "babel"      — type="text/babel" or type="text/jsx"
//   "json"       — type ends with /json (used for inline config blobs the
//                  walker should skip — we keep them so callers see the
//                  full document order, but the harness ignores them)
//   "other"      — anything else (templates, x-shader, vendor-specific)
struct InlineScript {
    std::string kind;
    std::string source;
};

// Walk the template HTML and pull every inline `<script>...</script>` block
// in document order. Tags that carry a `src=` attribute are skipped — those
// are evaluated separately via the `javascript_indices` path. Bundler-only
// `<script type="__bundler/...">` tags are also skipped (those carry the
// envelope, not executable JS).
std::vector<InlineScript> extract_inline_template_scripts(const std::string& template_html) {
    std::vector<InlineScript> out;
    static const std::regex tag_re(
        R"RX(<script\b([^>]*)>([\s\S]*?)</script>)RX",
        std::regex::icase);
    static const std::regex src_attr_re(
        R"RX(\bsrc\s*=\s*(?:"[^"]*"|'[^']*'|\S+))RX",
        std::regex::icase);
    static const std::regex type_attr_re(
        R"RX(\btype\s*=\s*(?:"([^"]*)"|'([^']*)'|(\S+)))RX",
        std::regex::icase);

    auto begin = std::sregex_iterator(template_html.begin(), template_html.end(), tag_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string attrs = (*it)[1].str();
        std::string body  = (*it)[2].str();

        // Skip `<script src="...">` — the harness handles those via
        // javascript_indices.
        if (std::regex_search(attrs, src_attr_re)) continue;

        // Read the type attribute, lower-case it for matching. Each of
        // the three regex sub-groups represents one valid attribute
        // form (double-quoted, single-quoted, unquoted) — pick whichever
        // matched.
        std::string type_lc;
        std::smatch tm;
        if (std::regex_search(attrs, tm, type_attr_re)) {
            for (size_t g = 1; g <= 3; ++g) {
                if (tm[g].matched) { type_lc = tm[g].str(); break; }
            }
        }
        std::transform(type_lc.begin(), type_lc.end(), type_lc.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // Skip the bundler envelope tags — those are not executable JS.
        if (type_lc.rfind("__bundler/", 0) == 0) continue;

        // Classify the kind. Anything that isn't recognized as JS, Babel,
        // or JSON is classified as "other" and the harness ignores it.
        std::string kind = "other";
        if (type_lc.empty() ||
            type_lc == "text/javascript" ||
            type_lc == "application/javascript" ||
            type_lc == "module") {
            kind = "javascript";
        } else if (type_lc == "text/babel" || type_lc == "text/jsx") {
            kind = "babel";
        } else if (type_lc.size() >= 5 &&
                   type_lc.compare(type_lc.size() - 5, 5, "/json") == 0) {
            kind = "json";
        }

        out.push_back({std::move(kind), std::move(body)});
    }
    return out;
}

} // namespace

std::optional<ClaudeBundle> parse_claude_bundle(const std::string& html) {
    auto manifest_text = extract_bundler_tag_content(html, "__bundler/manifest");
    auto template_text = extract_bundler_tag_content(html, "__bundler/template");
    if (!manifest_text || !template_text) return std::nullopt;

    // The template tag's content is itself a JSON-encoded string (so the
    // literal HTML inside doesn't have to escape `<`). Decode to get the
    // real HTML. Use parseValue() because a bare JSON string at the top
    // level isn't accepted by the strict parse() entry point.
    std::string template_html;
    try {
        auto tval = choc::json::parseValue(*template_text);
        if (!tval.isString()) return std::nullopt;
        template_html = std::string(tval.getString());
    } catch (...) { return std::nullopt; }

    // Parse the manifest JSON into a {uuid: {mime, compressed, data}} map.
    choc::value::Value mval;
    try { mval = choc::json::parse(*manifest_text); }
    catch (...) { return std::nullopt; }
    if (!mval.isObject()) return std::nullopt;

    ClaudeBundle bundle;
    bundle.template_html = std::move(template_html);

    // Walk the object, decode each entry into a ClaudeBundleAsset.
    // choc::value::Value::size() + getObjectMemberAt drives the
    // iteration; member name lookup gives us the uuid key.
    for (uint32_t i = 0; i < mval.size(); ++i) {
        auto member = mval.getObjectMemberAt(i);
        std::string uuid(member.name);
        const auto& entry = member.value;
        if (!entry.isObject()) continue;
        ClaudeBundleAsset asset;
        asset.uuid = std::move(uuid);
        if (entry.hasObjectMember("mime")) {
            auto mime = entry["mime"];
            if (mime.isString()) asset.mime = std::string(mime.getString());
        }
        if (!entry.hasObjectMember("data")) continue;
        auto data_val = entry["data"];
        if (!data_val.isString()) continue;

        auto decoded = pulp::runtime::base64_decode(data_val.getString());
        if (!decoded) continue;

        bool compressed = false;
        if (entry.hasObjectMember("compressed")) {
            auto cval = entry["compressed"];
            if (cval.isBool()) compressed = cval.getBool();
        }
        if (compressed) {
            // Prefer the runtime gzip_decompress entry point — it handles
            // real RFC 1952 streams of any size. The inline
            // `claude_bundle_inflate` shim's `deflate_decompress`-via-
            // `inflate_raw` path was silently failing on assets > ~900 KB
            // inflated (the heuristic initial buffer is `compressed_size *
            // 4` and the buffer-doubling retry loop on MZ_BUF_ERROR doesn't
            // recover for some inputs miniz produces partial output for).
            // On canonical Spectr Claude exports that meant ReactDOM
            // (1.08 MB) and Babel-standalone (3.14 MB) were silently
            // dropped during parsing, leaving only the React payload —
            // and inline `text/babel` scripts (the actual app code) had
            // no Babel.transform to compile against. Fall back to the
            // inline shim if the runtime path returns nullopt — preserves
            // back-compat for any exotic stream shape we haven't seen yet.
            auto inflated = pulp::runtime::gzip_decompress(decoded->data(), decoded->size());
            if (!inflated) {
                inflated = claude_bundle_inflate(decoded->data(), decoded->size());
            }
            if (!inflated) continue;
            asset.data = std::move(*inflated);
        } else {
            asset.data = std::move(*decoded);
        }
        bundle.assets.push_back(std::move(asset));
    }

    if (bundle.assets.empty()) return std::nullopt;

    // Compute javascript_indices: walk the template's <script src> uuids
    // in order, look each one up in assets[] (must be MIME javascript).
    auto srcs = extract_template_script_srcs(bundle.template_html);
    for (const auto& src : srcs) {
        for (size_t k = 0; k < bundle.assets.size(); ++k) {
            if (bundle.assets[k].uuid == src && bundle.assets[k].mime == "text/javascript") {
                bundle.javascript_indices.push_back(k);
                break;
            }
        }
    }

    return bundle;
}

// ── parse_claude_html_with_runtime (pulp #468 harness) ───────────────────
//
// Boot a headless ScriptEngine + WidgetBridge, build the bundler
// template into document.body via the import-runtime prelude, evaluate
// each JS payload in template-script order, then walk the materialized
// DOM into a DesignIR. Falls back to parse_claude_html on any failure
// — the static-parser floor is the contract.

namespace {

// Convert a JSON node (produced by __pulpImportRuntime__.walkDomJson)
// into an IRNode. Mirrors the structure parse_stitch_html produces so
// downstream codegen rules apply unchanged.
IRNode json_to_ir_node(const choc::value::ValueView& v);

void json_children_to_ir(const choc::value::ValueView& children, IRNode& parent) {
    if (!children.isArray()) return;
    for (uint32_t i = 0; i < children.size(); ++i) {
        auto child_view = children[i];
        if (!child_view.isObject()) continue;
        IRNode child = json_to_ir_node(child_view);
        // Skip empty text/comment markers entirely.
        // Codex P2 on PR #731: json_to_ir_node maps DOM "#text" → IR
        // "text" (line ~294), so filter against the IR vocabulary, not
        // the wire format. Otherwise empty whitespace text nodes pad
        // the materialized tree count and inflate the >9 success-floor
        // check the integration test asserts on.
        if (child.type == "text" && child.text_content.empty()) continue;
        if (child.type == "#error") continue;
        parent.children.push_back(std::move(child));
    }
}

IRNode json_to_ir_node(const choc::value::ValueView& v) {
    IRNode node;
    if (!v.isObject()) {
        node.type = "frame";
        return node;
    }
    auto get_str = [&](const char* k) -> std::string {
        if (!v.hasObjectMember(k)) return {};
        auto val = v[k];
        if (!val.isString()) return {};
        return std::string(val.getString());
    };

    auto type_str = get_str("type");
    if (type_str == "#text") {
        node.type = "text";
        node.text_content = get_str("text");
        return node;
    }

    // Map common HTML tags to the IR vocabulary parse_stitch_html uses.
    // Containers -> "frame", text-bearing tags -> "text", inputs -> "input".
    if (type_str == "div" || type_str == "section" || type_str == "article" ||
        type_str == "aside" || type_str == "header" || type_str == "footer" ||
        type_str == "nav" || type_str == "main" || type_str == "ul" ||
        type_str == "ol" || type_str == "li" || type_str == "form") {
        node.type = "frame";
    } else if (type_str == "span" || type_str == "p" || type_str == "label" ||
               type_str == "h1" || type_str == "h2" || type_str == "h3" ||
               type_str == "h4" || type_str == "h5" || type_str == "h6" ||
               type_str == "a" || type_str == "strong" || type_str == "em" ||
               type_str == "small" || type_str == "code") {
        node.type = "text";
    } else if (type_str == "button") {
        node.type = "button";
    } else if (type_str == "input" || type_str == "textarea" || type_str == "select") {
        node.type = "input";
    } else if (type_str == "img" || type_str == "image") {
        node.type = "image";
    } else if (type_str == "canvas") {
        node.type = "canvas";
    } else if (type_str == "svg") {
        node.type = "frame";  // treat SVG roots as containers; children below
    } else if (!type_str.empty()) {
        node.type = type_str;
    } else {
        node.type = "frame";
    }

    node.name = get_str("id");
    if (node.name.empty()) node.name = get_str("class");
    if (node.name.empty()) node.name = node.type;

    auto text_content = get_str("text");
    if (!text_content.empty() && node.type == "text") {
        node.text_content = text_content;
    } else if (!text_content.empty()) {
        // Container with collapsed text — preserve as a child text node so
        // the codegen still emits a label.
        IRNode text_child;
        text_child.type = "text";
        text_child.name = "text";
        text_child.text_content = text_content;
        node.children.push_back(std::move(text_child));
    }

    // Collect attributes (incl. data-pulp-role markers) for downstream.
    if (v.hasObjectMember("attrs") && v["attrs"].isObject()) {
        auto attrs = v["attrs"];
        for (uint32_t i = 0; i < attrs.size(); ++i) {
            auto m = attrs.getObjectMemberAt(i);
            if (m.value.isString()) {
                node.attributes[std::string(m.name)] = std::string(m.value.getString());
            }
        }
    }
    auto class_str = get_str("class");
    if (!class_str.empty()) node.attributes["class"] = class_str;

    // Detect audio widget from id/class/data-pulp-role.
    auto role = node.attributes.count("data-pulp-role")
        ? node.attributes["data-pulp-role"] : std::string{};
    auto detect_source = node.name + " " + class_str + " " + role;
    node.audio_widget = detect_audio_widget(detect_source);

    // Style props -> IRStyle. Only the common ones are mapped; anything
    // else stays in attributes for future extension.
    if (v.hasObjectMember("style") && v["style"].isObject()) {
        auto style = v["style"];
        auto style_str = [&](const char* k) -> std::optional<std::string> {
            if (!style.hasObjectMember(k)) return std::nullopt;
            auto sv = style[k];
            if (!sv.isString()) return std::nullopt;
            return std::string(sv.getString());
        };
        node.style.background_color = style_str("backgroundColor");
        node.style.color = style_str("color");
        node.style.border = style_str("border");
        node.style.box_shadow = style_str("boxShadow");
        node.style.font_family = style_str("fontFamily");
        node.style.font_style = style_str("fontStyle");
        node.style.text_align = style_str("textAlign");
        node.style.text_transform = style_str("textTransform");
        node.style.cursor = style_str("cursor");
        node.style.overflow = style_str("overflow");
        node.style.position = style_str("position");
        node.style.transform = style_str("transform");
        node.style.filter = style_str("filter");
        auto try_float = [&](const char* k) -> std::optional<float> {
            auto s = style_str(k);
            if (!s) return std::nullopt;
            try {
                // Strip "px" / "%" suffix if present.
                std::string num = *s;
                while (!num.empty() && (std::isspace(static_cast<unsigned char>(num.back()))
                                        || std::isalpha(static_cast<unsigned char>(num.back()))
                                        || num.back() == '%')) {
                    num.pop_back();
                }
                if (num.empty()) return std::nullopt;
                return std::stof(num);
            } catch (...) { return std::nullopt; }
        };
        node.style.opacity = try_float("opacity");
        node.style.border_radius = try_float("borderRadius");
        node.style.font_size = try_float("fontSize");
        node.style.letter_spacing = try_float("letterSpacing");
        node.style.line_height = try_float("lineHeight");
        node.style.width = try_float("width");
        node.style.height = try_float("height");
        node.style.min_width = try_float("minWidth");
        node.style.min_height = try_float("minHeight");
        node.style.max_width = try_float("maxWidth");
        node.style.max_height = try_float("maxHeight");
        auto fw = style_str("fontWeight");
        if (fw) {
            try { node.style.font_weight = std::stoi(*fw); } catch (...) {}
        }
    }

    if (v.hasObjectMember("children")) {
        json_children_to_ir(v["children"], node);
    }
    return node;
}

// Count materialized IRNodes recursively.
size_t count_ir_nodes(const IRNode& n) {
    size_t total = 1;
    for (const auto& c : n.children) total += count_ir_nodes(c);
    return total;
}

void layout_runtime_snapshot_root_if_requested(View& root, const ClaudeRuntimeOptions& opts) {
    if (opts.runtime_snapshot_viewport_width <= 0 || opts.runtime_snapshot_viewport_height <= 0)
        return;

    root.set_bounds({0.0f,
                     0.0f,
                     static_cast<float>(opts.runtime_snapshot_viewport_width),
                     static_cast<float>(opts.runtime_snapshot_viewport_height)});
    root.layout_children();
}

std::string color_to_hex(Color color) {
    std::ostringstream out;
    out << '#'
        << std::hex << std::setfill('0') << std::nouppercase
        << std::setw(2) << static_cast<int>(color.r8())
        << std::setw(2) << static_cast<int>(color.g8())
        << std::setw(2) << static_cast<int>(color.b8());
    if (color.a8() != 255)
        out << std::setw(2) << static_cast<int>(color.a8());
    return out.str();
}

LayoutDirection ir_direction(FlexDirection direction) {
    return (direction == FlexDirection::row || direction == FlexDirection::row_reverse)
        ? LayoutDirection::row
        : LayoutDirection::column;
}

LayoutAlign ir_align(FlexAlign align) {
    switch (align) {
        case FlexAlign::center: return LayoutAlign::center;
        case FlexAlign::end: return LayoutAlign::flex_end;
        case FlexAlign::stretch: return LayoutAlign::stretch;
        case FlexAlign::start:
        case FlexAlign::auto_:
        case FlexAlign::baseline:
        default: return LayoutAlign::flex_start;
    }
}

LayoutAlign ir_justify(FlexJustify justify) {
    switch (justify) {
        case FlexJustify::center: return LayoutAlign::center;
        case FlexJustify::end_: return LayoutAlign::flex_end;
        case FlexJustify::space_between: return LayoutAlign::space_between;
        case FlexJustify::space_around:
        case FlexJustify::space_evenly:
            return LayoutAlign::space_around;
        case FlexJustify::start:
        default: return LayoutAlign::flex_start;
    }
}

std::optional<std::string> ir_align_self(FlexAlign align) {
    switch (align) {
        case FlexAlign::center: return std::string("center");
        case FlexAlign::end: return std::string("flex-end");
        case FlexAlign::stretch: return std::string("stretch");
        case FlexAlign::baseline: return std::string("baseline");
        case FlexAlign::start: return std::string("flex-start");
        case FlexAlign::auto_:
        default: return std::nullopt;
    }
}

void capture_view_border_style(const View& view, IRNode& node) {
    if (view.has_border()) {
        node.style.border_color = color_to_hex(view.border_color());
        node.style.border_width = view.border_width();
        if (view.corner_radius() != 0.0f)
            node.style.border_radius = view.corner_radius();
    }

    if (view.has_border_sides()) {
        node.style.border_top_color = color_to_hex(view.border_top_color());
        node.style.border_right_color = color_to_hex(view.border_right_color());
        node.style.border_bottom_color = color_to_hex(view.border_bottom_color());
        node.style.border_left_color = color_to_hex(view.border_left_color());
        if (view.has_border_top_set()) node.style.border_top_width = view.border_top_width();
        if (view.has_border_right_set()) node.style.border_right_width = view.border_right_width();
        if (view.has_border_bottom_set()) node.style.border_bottom_width = view.border_bottom_width();
        if (view.has_border_left_set()) node.style.border_left_width = view.border_left_width();
    }

    if (view.has_corner_radii()) {
        node.style.border_top_left_radius = view.corner_radius_tl();
        node.style.border_top_right_radius = view.corner_radius_tr();
        node.style.border_bottom_right_radius = view.corner_radius_br();
        node.style.border_bottom_left_radius = view.corner_radius_bl();
    }
}

IRNode view_to_ir_node(const View& view, std::string_view path) {
    IRNode node;
    node.name = !view.id().empty() ? view.id() : std::string(path);
    if (!view.id().empty()) {
        node.stable_anchor_id = view.id();
        node.anchor_strategy = "adapter";
    }

    if (const auto* label = dynamic_cast<const Label*>(&view)) {
        node.type = "text";
        node.text_content = label->text();
        if (label->has_own_font_size()) node.style.font_size = label->font_size();
        if (label->has_own_text_color()) node.style.color = color_to_hex(label->text_color());
        if (!label->font_family().empty()) node.style.font_family = label->font_family();
        if (label->has_own_font_weight()) node.style.font_weight = label->font_weight();
    } else if (const auto* button = dynamic_cast<const TextButton*>(&view)) {
        node.type = "button";
        node.text_content = button->label();
    } else if (const auto* editor = dynamic_cast<const TextEditor*>(&view)) {
        node.type = "input";
        node.attributes["value"] = editor->text();
    } else if (const auto* checkbox = dynamic_cast<const Checkbox*>(&view)) {
        node.type = "checkbox";
        node.attributes["checked"] = checkbox->is_checked() ? "true" : "false";
    } else if (const auto* knob = dynamic_cast<const Knob*>(&view)) {
        node.type = "knob";
        node.audio_widget = AudioWidgetType::knob;
        node.audio_label = knob->label();
        node.audio_default = knob->default_value();
        node.attributes["value"] = std::to_string(knob->value());
    } else if (const auto* fader = dynamic_cast<const Fader*>(&view)) {
        node.type = "fader";
        node.audio_widget = AudioWidgetType::fader;
        node.audio_label = fader->label();
        node.attributes["value"] = std::to_string(fader->value());
        if (fader->orientation() == Fader::Orientation::horizontal)
            node.attributes["orientation"] = "horizontal";
    } else if (const auto* meter = dynamic_cast<const Meter*>(&view)) {
        node.type = "meter";
        node.audio_widget = AudioWidgetType::meter;
        node.attributes["value"] = std::to_string(meter->display_rms());
    } else if (const auto* xy = dynamic_cast<const XYPad*>(&view)) {
        node.type = "xy_pad";
        node.audio_widget = AudioWidgetType::xy_pad;
        node.attributes["x"] = std::to_string(xy->x_value());
        node.attributes["y"] = std::to_string(xy->y_value());
    } else {
        node.type = "frame";
    }

    const auto& flex = view.flex();
    node.layout.direction = ir_direction(flex.direction);
    node.layout.justify = ir_justify(flex.justify_content);
    node.layout.align = ir_align(flex.align_items);
    node.layout.align_self = ir_align_self(flex.align_self);
    node.layout.gap = flex.gap;
    node.layout.padding_top = flex.padding_top >= 0 ? flex.padding_top : flex.padding;
    node.layout.padding_right = flex.padding_right >= 0 ? flex.padding_right : flex.padding;
    node.layout.padding_bottom = flex.padding_bottom >= 0 ? flex.padding_bottom : flex.padding;
    node.layout.padding_left = flex.padding_left >= 0 ? flex.padding_left : flex.padding;
    const auto margin_top = flex.margin_top >= 0 ? flex.margin_top : flex.margin;
    const auto margin_right = flex.margin_right >= 0 ? flex.margin_right : flex.margin;
    const auto margin_bottom = flex.margin_bottom >= 0 ? flex.margin_bottom : flex.margin;
    const auto margin_left = flex.margin_left >= 0 ? flex.margin_left : flex.margin;
    if (margin_top != 0.0f) node.layout.margin_top = margin_top;
    if (margin_right != 0.0f) node.layout.margin_right = margin_right;
    if (margin_bottom != 0.0f) node.layout.margin_bottom = margin_bottom;
    if (margin_left != 0.0f) node.layout.margin_left = margin_left;
    if (flex.flex_grow != 0.0f) node.layout.flex_grow = flex.flex_grow;
    if (flex.flex_shrink != 1.0f) node.layout.flex_shrink = flex.flex_shrink;

    if (flex.preferred_width > 0) node.style.width = flex.preferred_width;
    else if (view.bounds().width > 0) node.style.width = view.bounds().width;
    if (flex.preferred_height > 0) node.style.height = flex.preferred_height;
    else if (view.bounds().height > 0) node.style.height = view.bounds().height;
    if (flex.min_width > 0) node.style.min_width = flex.min_width;
    if (flex.min_height > 0) node.style.min_height = flex.min_height;
    if (flex.max_width > 0) node.style.max_width = flex.max_width;
    if (flex.max_height > 0) node.style.max_height = flex.max_height;
    if (view.has_background_color()) node.style.background_color = color_to_hex(view.background_color());
    capture_view_border_style(view, node);

    for (size_t i = 0; i < view.child_count(); ++i) {
        const auto* child = view.child_at(i);
        if (child == nullptr) continue;
        node.children.push_back(view_to_ir_node(*child, std::string(path) + "/" + std::to_string(i)));
    }
    return node;
}

}  // namespace

// JSON-encode an arbitrary string for safe injection into a JS string literal.
// External linkage (declared in design_import_internal.hpp) so the extracted
// claude_bundle_sources.cpp can reuse it without duplicating the escaper.
std::string json_string_literal(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '<':  out += "\\u003C"; break;  // defensive: avoid </script> closure
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

namespace {

void set_runtime_error(ClaudeRuntimeOptions& opts, const std::string& msg) {
    if (opts.error_out) *opts.error_out = msg;
}

// ── Shared Claude-bundle runtime-import shim + payload pipeline ─────────
//
// Both the offline harness (`parse_claude_html_with_runtime`) and the
// live-engine path (`WidgetBridge::evaluate_claude_bundle_in_live_engine`)
// need to run an identical sequence of pre-payload shims, asset payload
// evals, autoflushSync patch, inline-script evals (JSON/JS/Babel), and
// finally a synthetic readystatechange/DOMContentLoaded/load dispatch.
//
// The shape diverges only on two axes:
//   • the User-Agent string we plant on `navigator` (offline harness
//     identifies itself differently from the live runtime),
//   • whether host-installed `globalThis.React` / `globalThis.ReactDOM`
//     must be snapshotted around the asset-eval loop so the bundle's
//     react.development.js can't clobber the live reconciler's copy.
//   • how soft errors are surfaced — offline forwards them to a caller
//     sink (eventually `opts.error_out`), runtime silently swallows.
//
// The helper below encapsulates all of that. Tag-named via the ImportShimConfig
// struct so callers stay readable.
struct ImportShimConfig {
    // Value planted on navigator.userAgent. Offline harness uses
    // "PulpImportHarness/1.0"; live runtime uses "PulpImportRuntime/1.0".
    const char* user_agent = "PulpImportHarness/1.0";

    // When true, snapshot globalThis.React/ReactDOM into hidden globals
    // before the asset payload loop and restore them after. Required for
    // the live-engine path so the bundle's bundled React can't displace
    // the host reconciler's React. Offline harness has no host React
    // installed so this is a no-op there.
    bool preserve_host_react = false;

    // When true, gate the autoflushSync ReactDOM.createRoot patch on an
    // idempotency marker (__pulpAutoflushPatched__). The live engine may
    // re-enter the runtime path on the same ReactDOM instance; the
    // offline harness creates a fresh engine each call so the marker
    // isn't needed.
    bool guard_autoflush_idempotent = false;

    // Optional sink for soft errors. nullptr swallows everything (matches
    // the live-runtime behavior; the JS side reads error slots out of
    // globalThis instead).
    std::function<void(const std::string&)> report_error;
};

// Run the shared boot sequence against an already-configured engine.
//
// Steps:
//   1. Pre-payload shims (createElementNS/createTextNode, HTML*Element
//      stub constructors, addEventListener on document/window/globalThis,
//      navigator.userAgent).
//   2. Optionally snapshot host React/ReactDOM, then evaluate each
//      `javascript_indices` asset (each wrapped in a JS try/catch that
//      stashes errors in `globalThis.__pulpPayloadErr_<idx>__`), then
//      optionally restore host React/ReactDOM.
//   3. Patch ReactDOM.createRoot so root.render runs inside flushSync.
//   4. Re-inject JSON inline scripts (tweak-defaults pattern), then
//      text/javascript inline scripts, then text/babel inline scripts
//      (Babel.transform → eval per block).
//   5. Dispatch synthetic readystatechange → DOMContentLoaded → load.
//
// Soft errors are forwarded to `cfg.report_error` when set; otherwise
// silently swallowed. C++ exceptions never escape this helper for the
// asset/inline/DOM-event steps — they are caught and (optionally)
// reported. The shim-installation try/catches are best-effort by design.
void run_claude_bundle_payload_pipeline(ScriptEngine& engine,
                                        const ClaudeBundle& bundle,
                                        const ImportShimConfig& cfg) {
    auto report = [&](const std::string& msg) {
        if (cfg.report_error) cfg.report_error(msg);
    };

    // ── Pre-payload sandbox shims ──
    // document.createElementNS / createTextNode — React-DOM expects them.
    try {
        engine.evaluate(
            "(function(){"
            "  if (typeof document === 'undefined' || !document) return;"
            "  if (typeof document.createElementNS !== 'function') {"
            "    document.createElementNS = function(ns, type) {"
            "      var el = document.createElement(type);"
            "      try { el.namespaceURI = ns; } catch (e) {}"
            "      return el;"
            "    };"
            "  }"
            "  if (typeof document.createTextNode !== 'function') {"
            "    document.createTextNode = function(text) {"
            "      var el = document.createElement('#text');"
            "      try { el.nodeValue = text; el.textContent = text; } catch (e) {}"
            "      return el;"
            "    };"
            "  }"
            "})();void 0");
    } catch (...) {}

    // Stub HTML*Element constructors so React-DOM `instanceof` checks return
    // false instead of throwing "invalid 'instanceof' right operand".
    try {
        engine.evaluate(
            "(function(){"
            "  var names = ['Element','HTMLElement','Node','EventTarget',"
            "    'HTMLIFrameElement','HTMLInputElement','HTMLTextAreaElement',"
            "    'HTMLSelectElement','HTMLOptionElement','HTMLOptGroupElement',"
            "    'HTMLFormElement','HTMLAnchorElement','HTMLImageElement',"
            "    'HTMLDivElement','HTMLSpanElement','HTMLButtonElement',"
            "    'HTMLLabelElement','HTMLCanvasElement','HTMLBodyElement',"
            "    'HTMLDocument','SVGElement','DocumentFragment','Text','Comment'];"
            "  for (var i = 0; i < names.length; i++) {"
            "    var n = names[i];"
            "    if (typeof globalThis[n] === 'undefined') {"
            "      try { globalThis[n] = function(){}; } catch (e) {}"
            "    }"
            "    if (typeof window !== 'undefined' && typeof window[n] === 'undefined') {"
            "      try { window[n] = globalThis[n]; } catch (e) {}"
            "    }"
            "  }"
            "})();void 0");
    } catch (...) {}

    // Ensure addEventListener / removeEventListener exist on document/window/globalThis.
    try {
        engine.evaluate(
            "(function(){"
            "  function _ensureEL(t){"
            "    if (!t) return;"
            "    if (typeof t.addEventListener !== 'function') {"
            "      t.addEventListener = function(){};"
            "    }"
            "    if (typeof t.removeEventListener !== 'function') {"
            "      t.removeEventListener = function(){};"
            "    }"
            "  }"
            "  if (typeof document !== 'undefined') _ensureEL(document);"
            "  if (typeof window !== 'undefined')   _ensureEL(window);"
            "  if (typeof globalThis !== 'undefined') _ensureEL(globalThis);"
            "})();void 0");
    } catch (...) {}

    // navigator.userAgent — UMD factory bundles probe it during init.
    // Generic enough that browser-feature detection chains fall through
    // to safe defaults.
    {
        std::string ua_js =
            "if (typeof navigator === 'undefined' || !navigator || !navigator.userAgent) {"
            "  try {"
            "    globalThis.navigator = globalThis.navigator || {};"
            "    if (!globalThis.navigator.userAgent) {"
            "      Object.defineProperty(globalThis.navigator, 'userAgent', {"
            "        value: '";
        ua_js += cfg.user_agent;
        ua_js +=
            "', writable: false, configurable: true"
            "      });"
            "    }"
            "  } catch (e) {"
            "    globalThis.navigator = { userAgent: '";
        ua_js += cfg.user_agent;
        ua_js +=
            "' };"
            "  }"
            "}"
            ";void 0";
        try { engine.evaluate(ua_js); } catch (...) {}
    }

    // ── Asset payload eval ──
    //
    // When preserve_host_react is set, snapshot any host-installed
    // globalThis.React / globalThis.ReactDOM before the loop so a bundle
    // that ships its own react.development.js can't displace the live
    // reconciler's copy. Restore after.
    if (cfg.preserve_host_react) {
        try {
            engine.evaluate(
                "globalThis.__pulpHostReact__ = "
                "  (typeof globalThis.React !== 'undefined') ? globalThis.React : null;"
                "globalThis.__pulpHostReactDOM__ = "
                "  (typeof globalThis.ReactDOM !== 'undefined') ? globalThis.ReactDOM : null;"
                ";void 0");
        } catch (...) {}
    }

    for (auto idx : bundle.javascript_indices) {
        if (idx >= bundle.assets.size()) continue;
        const auto& asset = bundle.assets[idx];
        std::string source(asset.data.begin(), asset.data.end());
        std::string wrap_pre = "try {\n";
        std::string wrap_post = "\n} catch(e) { globalThis.__pulpPayloadErr_" + std::to_string(idx) +
            "__ = String(e && e.message ? e.message : e) + ' :: stack=' + (e && e.stack ? e.stack : '<no stack>'); }";
        source = wrap_pre + source + wrap_post;
        // Trailing `;void 0` so the payload's last expression doesn't
        // produce a value the engine has to convert back to choc. (The
        // CHOC QuickJS path can recurse on cyclical objects.)
        source += "\n;void 0";
        try {
            engine.evaluate(source);
        } catch (const std::exception& e) {
            report(std::string("payload ") + std::to_string(idx)
                   + " threw: " + e.what());
            // continue intentionally — soft-fail per asset.
        } catch (...) {
            report(std::string("payload ") + std::to_string(idx)
                   + " threw: unknown exception");
            // continue intentionally
        }
    }

    if (cfg.preserve_host_react) {
        try {
            engine.evaluate(
                "if (globalThis.__pulpHostReact__) {"
                "  globalThis.React = globalThis.__pulpHostReact__;"
                "}"
                "if (globalThis.__pulpHostReactDOM__) {"
                "  globalThis.ReactDOM = globalThis.__pulpHostReactDOM__;"
                "}"
                "delete globalThis.__pulpHostReact__;"
                "delete globalThis.__pulpHostReactDOM__;"
                ";void 0");
        } catch (...) {}
    }

    // Patch ReactDOM.createRoot so root.render wraps work in
    // ReactDOM.flushSync — forces React 18's concurrent renderer to
    // commit synchronously. Without this, render() schedules work via
    // the scheduler (MessageChannel) but the commit never fires before
    // walkDomJson (offline) or settle (runtime) runs.
    {
        std::string patch_js = "(function(){";
        patch_js += "  if (typeof ReactDOM !== 'object' || typeof ReactDOM.createRoot !== 'function') return;";
        if (cfg.guard_autoflush_idempotent) {
            patch_js += "  if (ReactDOM.__pulpAutoflushPatched__) return;";
            patch_js += "  ReactDOM.__pulpAutoflushPatched__ = true;";
        }
        patch_js +=
            "  var _origCreateRoot = ReactDOM.createRoot;"
            "  ReactDOM.createRoot = function(container, opts) {"
            "    var root = _origCreateRoot.call(this, container, opts);"
            "    if (root && typeof root.render === 'function') {"
            "      var _origRender = root.render.bind(root);"
            "      root.render = function(element) {"
            "        try {"
            "          if (typeof ReactDOM.flushSync === 'function') {"
            "            ReactDOM.flushSync(function(){ _origRender(element); });"
            "          } else {"
            "            _origRender(element);"
            "          }"
            "        } catch (e) {"
            "          globalThis.__pulpCreateRootRenderErr__ ="
            "            String(e && e.message ? e.message : e) + ' :: stack=' + (e && e.stack ? e.stack : '');"
            "        }"
            "      };"
            "    }"
            "    return root;"
            "  };"
            "})();void 0";
        try { engine.evaluate(patch_js); } catch (...) {}
    }

    // ── Inline scripts from the template ──
    auto inline_scripts = extract_inline_template_scripts(bundle.template_html);

    // Re-inject JSON-kind inline scripts (e.g. `<script id="tweak-defaults">`)
    // back into document.head — bundles read them via getElementById.
    for (size_t i = 0; i < inline_scripts.size(); ++i) {
        const auto& s = inline_scripts[i];
        if (s.kind != "json") continue;
        std::string js =
            "globalThis.__pulpJsonScript_" + std::to_string(i) + "__ = "
            + json_string_literal(s.source) + ";"
            "(function(){"
            "  var el = document.createElement('script');"
            "  el.type = 'application/json';"
            "  el.id = 'tweak-defaults';"
            "  el.textContent = globalThis.__pulpJsonScript_" + std::to_string(i) + "__;"
            "  document.head.appendChild(el);"
            "})();void 0";
        try { engine.evaluate(js); } catch (...) {}
    }

    // text/javascript inline blocks in document order. Soft-fail per
    // script — match the existing payload-eval pattern.
    for (size_t i = 0; i < inline_scripts.size(); ++i) {
        const auto& s = inline_scripts[i];
        if (s.kind != "javascript") continue;
        try {
            engine.evaluate(s.source + "\n;void 0");
        } catch (const std::exception& e) {
            report("inline JS script " + std::to_string(i)
                   + " threw: " + e.what());
        } catch (...) {
            report("inline JS script " + std::to_string(i)
                   + " threw: unknown exception");
        }
    }

    // text/babel inline blocks — verify Babel-standalone is in scope, then
    // transform each block via `Babel.transform(src, {presets: ['react']}).code`
    // before evaluating. Soft-fail per script.
    bool has_any_babel = false;
    for (const auto& s : inline_scripts) {
        if (s.kind == "babel") { has_any_babel = true; break; }
    }
    if (has_any_babel) {
        bool babel_loaded = false;
        try {
            // QuickJS / JSC may report the boolean expression as bool,
            // int32, or float64 depending on which path the engine took
            // to coerce — accept any numeric-or-bool truthy value.
            auto v = engine.evaluate(
                "!!(typeof globalThis.Babel !== 'undefined' && "
                "   typeof globalThis.Babel.transform === 'function')");
            if (v.isBool())          babel_loaded = v.getBool();
            else if (v.isInt32())    babel_loaded = (v.getInt32() != 0);
            else if (v.isInt64())    babel_loaded = (v.getInt64() != 0);
            else if (v.isFloat32() || v.isFloat64())
                babel_loaded = (v.getFloat64() != 0.0);
        } catch (...) {
            babel_loaded = false;
        }

        if (!babel_loaded) {
            report("babel-standalone not loaded; skipping inline text/babel scripts");
        } else {
            // Stash each Babel source as a JS string, transform via the
            // engine, then evaluate the transformed code. Using a global
            // staging slot keeps us from escaping JSX source for embedding
            // in a JS string literal — JSX happily contains characters
            // that would break a hand-rolled escape.
            auto soft_step = [&](size_t i, const char* phase,
                                 const std::string& src) -> bool {
                try {
                    engine.evaluate(src);
                    return true;
                } catch (const std::exception& e) {
                    report("inline babel script " + std::to_string(i)
                           + " " + phase + " failed: " + e.what());
                    return false;
                } catch (...) {
                    report("inline babel script " + std::to_string(i)
                           + " " + phase + " failed: unknown exception");
                    return false;
                }
            };

            for (size_t i = 0; i < inline_scripts.size(); ++i) {
                const auto& s = inline_scripts[i];
                if (s.kind != "babel") continue;

                std::string set_src = "globalThis.__pulpBabelSrc__ = ";
                set_src += json_string_literal(s.source);
                set_src += ";void 0";
                if (!soft_step(i, "stash", set_src)) continue;

                if (!soft_step(i, "transform",
                        "(function(){"
                        "  try {"
                        "    var out = globalThis.Babel.transform("
                        "      globalThis.__pulpBabelSrc__,"
                        "      { presets: ['react'] });"
                        "    globalThis.__pulpBabelOut__ = (out && out.code) ? out.code : '';"
                        "    globalThis.__pulpBabelErr__ = '';"
                        "  } catch (e) {"
                        "    globalThis.__pulpBabelOut__ = '';"
                        "    globalThis.__pulpBabelErr__ = String(e && e.message ? e.message : e);"
                        "  }"
                        "})();void 0")) continue;

                // Probe the babel-side error string and surface if non-empty.
                try {
                    auto err_v = engine.evaluate(
                        "globalThis.__pulpBabelErr__ || ''");
                    if (err_v.isString()) {
                        std::string err_msg(err_v.getString());
                        if (!err_msg.empty()) {
                            report("inline babel script " + std::to_string(i)
                                   + " babel error: " + err_msg);
                            continue;
                        }
                    }
                } catch (...) { /* ignore probe failure */ }

                // JS-side try/catch around eval so non-std::exception JS
                // errors get surfaced via __pulpEvalErr__ instead of being
                // silently swallowed by the C++ catch(...).
                soft_step(i, "eval",
                    "try {"
                    "  (0, eval)(globalThis.__pulpBabelOut__);"
                    "  globalThis.__pulpEvalErr__ = '';"
                    "} catch (e) {"
                    "  globalThis.__pulpEvalErr__ = String(e && e.message ? e.message : e)"
                    "    + ' :: stack=' + (e && e.stack ? e.stack : '<no stack>');"
                    "}"
                    ";void 0");
                try {
                    auto v = engine.evaluate("globalThis.__pulpEvalErr__ || ''");
                    if (v.isString()) {
                        std::string em(v.getString());
                        if (!em.empty()) {
                            report("inline babel script " + std::to_string(i)
                                   + " JS-eval error: " + em);
                        }
                    }
                } catch (...) { /* ignore best-effort surfacing */ }
            }

            // Tidy up the staging slots so they don't leak into the
            // walker's view of globals.
            try {
                engine.evaluate(
                    "delete globalThis.__pulpBabelSrc__;"
                    "delete globalThis.__pulpBabelOut__;"
                    "delete globalThis.__pulpBabelErr__;void 0");
            } catch (...) { /* nothing to do — best-effort */ }
        }
    }

    // ── Dispatch readystatechange → DOMContentLoaded → load ──
    // Mirrors the browser lifecycle for bundles that defer boot to
    // document-ready. The trailing `;void 0` avoids the QuickJS
    // toChocValue circular-ref recursion. We construct the event objects
    // defensively: use `new Event(t)` if available, otherwise a plain
    // literal so dispatch still runs against listeners keyed on `type`.
    try {
        engine.evaluate(
            "(function(){"
            "  if (typeof document === 'undefined') return;"
            "  function _mkEvent(t, target){"
            "    if (typeof Event === 'function') {"
            "      try { return new Event(t); } catch (e) {}"
            "    }"
            "    return { type: t, target: target, currentTarget: target,"
            "             bubbles: false, cancelable: false,"
            "             defaultPrevented: false,"
            "             preventDefault: function(){ this.defaultPrevented = true; },"
            "             stopPropagation: function(){},"
            "             stopImmediatePropagation: function(){} };"
            "  }"
            "  function _dispatch(target, type){"
            "    if (!target) return;"
            "    if (typeof target.dispatchEvent === 'function') {"
            "      try { target.dispatchEvent(_mkEvent(type, target)); } catch (e) {}"
            "    }"
            "  }"
            "  try { document.readyState = 'interactive'; } catch (e) {}"
            "  _dispatch(document, 'readystatechange');"
            "  _dispatch(document, 'DOMContentLoaded');"
            "  try { document.readyState = 'complete'; } catch (e) {}"
            "  _dispatch(document, 'readystatechange');"
            "  if (typeof window !== 'undefined') _dispatch(window, 'load');"
            "})();void 0");
    } catch (const std::exception& e) {
        report(std::string("DOMContentLoaded dispatch threw: ") + e.what());
    } catch (...) {
        report("DOMContentLoaded dispatch threw: unknown exception");
    }
}

} // namespace

std::optional<ClaudeBundle> parse_jsx_react(const std::string& bundle_js,
                                            const std::string& component_name) {
    // The C++ side does NOT compile JSX. The CLI shells out to
    // `tools/import-design/jsx-runtime/jsx-transform.mjs` (Node + esbuild)
    // and hands us the IIFE bundle as a string. We wrap it as a
    // ClaudeBundle so the existing runtime harness can materialize it
    // without a separate execution lane.
    //
    // The bundle is self-contained (ships React + ReactDOM + user JSX +
    // a navigator/document shim banner that runs before any ESM import
    // hoists). Mount happens against `document.getElementById('root')`
    // — falls back to document.body when running under pulp-screenshot.
    //
    // pulp jsx-instrument-import experiment (2026-05-17). See
    // planning/2026-05-17-jsx-instrument-import.md.
    if (bundle_js.size() < 100) return std::nullopt;

    ClaudeBundleAsset app;
    app.uuid = "jsx-runtime-app";
    app.mime = "text/javascript";
    app.data.assign(bundle_js.begin(), bundle_js.end());

    ClaudeBundle bundle;
    bundle.assets.push_back(std::move(app));
    bundle.javascript_indices.push_back(0);

    // Escape the component name for embedding in an HTML attribute. The
    // existing v0_html_attr_escape helper handles this safely.
    bundle.template_html =
        "<div id=\"root\" data-pulp-source=\"jsx\" data-jsx-component=\"" +
        v0_html_attr_escape(component_name) +
        "\"></div><script src=\"jsx-runtime-app\"></script>";
    return bundle;
}

std::string synthesize_runtime_envelope(const ClaudeBundle& bundle) {
    // Build a Claude-style HTML envelope around an arbitrary in-memory
    // ClaudeBundle so `parse_claude_html_with_runtime` can consume it
    // without a real Claude Design HTML wrapper on input. Uses raw base64
    // (compressed:false) — gzip+deflate is unnecessary overhead for
    // in-process synthesis. Matches the manifest_entry/build_envelope
    // helpers in test_design_import_claude_runtime.cpp.

    // Manifest: map of uuid → { mime, compressed:false, data: base64 }.
    std::ostringstream manifest;
    manifest << "{";
    bool first = true;
    for (const auto& asset : bundle.assets) {
        if (!first) manifest << ",";
        first = false;
        const auto b64 = pulp::runtime::base64_encode(
            asset.data.data(), asset.data.size());
        manifest << "\"" << asset.uuid << "\":{"
                 << "\"mime\":\"" << asset.mime << "\","
                 << "\"compressed\":false,"
                 << "\"data\":\"" << b64 << "\"}";
    }
    manifest << "}";

    // Template HTML is JSON-escaped per the bundler/template script-tag
    // contract — strings inside the script-tag body are quoted JSON.
    // Reuse the file-scope json_string_literal helper.
    const auto template_json = json_string_literal(bundle.template_html);

    std::ostringstream html;
    html << "<!DOCTYPE html><html><head><title>Pulp JSX Runtime Import</title>"
            "</head><body>"
            "<script type=\"__bundler/manifest\">" << manifest.str() << "</script>"
            "<script type=\"__bundler/template\">" << template_json << "</script>"
            "</body></html>";
    return html.str();
}


// External-linkage thin wrapper around the anonymous-namespace
// json_string_literal so widget_bridge.cpp (a separate TU) can use it
// without re-implementing JSON escaping. Forward-declared in
// widget_bridge.cpp at file scope.
namespace detail {
std::string json_string_literal_for_widget_bridge(const std::string& s) {
    return ::pulp::view::json_string_literal(s);
}
} // namespace detail

DesignIR parse_claude_html_with_runtime(const std::string& html, ClaudeRuntimeOptions opts) {
    auto static_fallback = [&](const std::string& reason) -> DesignIR {
        set_runtime_error(opts, reason);
        auto ir = parse_claude_html(html);
        ir.fallback_reason = reason;
        ImportDiagnostic diagnostic;
        diagnostic.severity = ImportDiagnosticSeverity::warning;
        diagnostic.kind = ImportDiagnosticKind::fallback_used;
        diagnostic.code = "runtime-fallback";
        diagnostic.path = "<root>";
        diagnostic.message = reason;
        ir.diagnostics.push_back(std::move(diagnostic));
        return ir;
    };

    auto bundle = parse_claude_bundle(html);
    if (!bundle) {
        return static_fallback("no bundler envelope (parse_claude_bundle returned nullopt)");
    }

    // Cap total JS bytes — an unbounded eval risks the QuickJS parser
    // stack on a 10+ MB minified blob. We err on the side of falling
    // back; the static path still produces the loader-shell IR.
    size_t total_js = 0;
    for (auto idx : bundle->javascript_indices) {
        if (idx < bundle->assets.size()) total_js += bundle->assets[idx].data.size();
    }
    if (total_js > opts.max_total_js_bytes) {
        std::ostringstream ss;
        ss << "bundled JS too large (" << total_js << " > " << opts.max_total_js_bytes
           << " bytes); rerun with a higher max_total_js_bytes to attempt eval";
        return static_fallback(ss.str());
    }
    if (bundle->javascript_indices.empty()) {
        return static_fallback("bundle has no javascript_indices");
    }

    // Boot the harness: ScriptEngine + minimal View/StateStore/WidgetBridge.
    // The WidgetBridge is what wires up the web-compat preludes, including
    // `__pulpImportRuntime__`. We don't render anything — the View is a
    // sink for the materialized native widgets.
    //
    // Honor opts.engine_override when present (Codex P2 on PR #731).
    // Useful for: deterministic tests, working around QuickJS parser
    // stack limits on larger Claude bundles by forcing JSC, etc.
    try {
        ScriptEngine engine = opts.engine_override.has_value()
            ? ScriptEngine(static_cast<JsEngineType>(*opts.engine_override))
            : ScriptEngine();
        View root;
        state::StateStore store;
        std::unique_ptr<WidgetBridge> bridge_ptr;
        try {
            bridge_ptr = std::make_unique<WidgetBridge>(engine, root, store);
        } catch (const std::exception& e) {
            return static_fallback(std::string("WidgetBridge ctor failed: ") + e.what());
        } catch (...) {
            return static_fallback("WidgetBridge ctor failed: unknown exception");
        }
        WidgetBridge& bridge = *bridge_ptr;

        // WidgetBridge defers the appendChild/insertBefore/etc. wiring
        // (kDomOpsInit) until the first load_script() call. The harness
        // bypasses load_script and goes straight to engine.evaluate(),
        // so prime it with an empty script to force those bindings on.
        // (Tracked as a known follow-up: see issue #468 thread re:
        // duplicated dom-ops sources of truth.)
        try {
            bridge.load_script("");
        } catch (const std::exception& e) {
            return static_fallback(std::string("dom-ops priming failed: ") + e.what());
        } catch (...) {
            return static_fallback("dom-ops priming failed: unknown exception");
        }

        // Inject the template HTML as a JS string and call buildDom.
        // `JSON.parse(...)` would be cleaner but the template is already
        // unwrapped HTML (not JSON-encoded), so a direct string literal
        // assignment is what we need.
        {
            std::string js = "globalThis.__pulpImportRuntime__.resetBody();"
                             "globalThis.__pulpImportRuntime__.buildDom(";
            js += json_string_literal(bundle->template_html);
            js += ");void 0";
            try {
                engine.evaluate(js);
                // buildDom catches its own exceptions and stashes the
                // message; surface it as a soft signal.
                try {
                    auto last = engine.evaluate(
                        "globalThis.__pulpImportRuntime__._lastError || ''");
                    if (last.isString()) {
                        std::string s(last.getString());
                        if (!s.empty()) {
                            set_runtime_error(opts,
                                std::string("buildDom soft error: ") + s);
                        }
                    }
                } catch (...) { /* ignore probe failure */ }
            } catch (const std::exception& e) {
                return static_fallback(std::string("buildDom failed: ") + e.what());
            } catch (...) {
                return static_fallback("buildDom failed: unknown exception");
            }
        }

        // Shim install + payload eval + inline-script eval + DOMContentLoaded
        // dispatch. Shared with WidgetBridge::evaluate_claude_bundle_in_live_engine
        // — see run_claude_bundle_payload_pipeline for the full sequence.
        // The offline harness uses a fresh ScriptEngine, so neither host-React
        // preservation nor the autoflushSync-idempotency marker is required.
        {
            ImportShimConfig cfg;
            cfg.user_agent = "PulpImportHarness/1.0";
            cfg.preserve_host_react = false;
            cfg.guard_autoflush_idempotent = false;
            cfg.report_error = [&](const std::string& msg) {
                set_runtime_error(opts, msg);
            };
            run_claude_bundle_payload_pipeline(engine, *bundle, cfg);
        }

        // Step 4: layered async drain. The original two-pump cycle stays;
        // we then run two more pump+frame-callback cycles so async chains
        // (Babel's transform queue, React 18 concurrent commits, fetch
        // microtasks) have a chance to settle.
        try {
            engine.pump_message_loop();
            bridge.service_frame_callbacks();
            engine.pump_message_loop();
            bridge.service_frame_callbacks();
            engine.pump_message_loop();
            bridge.service_frame_callbacks();
            engine.pump_message_loop();
        } catch (const std::exception& e) {
            return static_fallback(std::string("microtask drain failed: ") + e.what());
        } catch (...) {
            return static_fallback("microtask drain failed: unknown exception");
        }
        layout_runtime_snapshot_root_if_requested(root, opts);

        // Walk the materialized DOM into a JSON tree.
        std::string walked;
        try {
            auto val = engine.evaluate(
                "globalThis.__pulpImportRuntime__.walkDomJson()");
            if (!val.isString()) {
                return static_fallback("walkDomJson did not return a string");
            }
            walked = std::string(val.getString());
        } catch (const std::exception& e) {
            return static_fallback(std::string("walkDomJson threw: ") + e.what());
        } catch (...) {
            return static_fallback("walkDomJson threw: unknown exception");
        }

        if (walked.empty()) {
            return static_fallback("walkDomJson returned empty string");
        }

        choc::value::Value tree;
        try {
            tree = choc::json::parse(walked);
        } catch (const std::exception& e) {
            return static_fallback(std::string("choc::json::parse failed: ") + e.what());
        } catch (...) {
            return static_fallback("choc::json::parse failed: unknown exception");
        }
        if (!tree.isObject()) {
            return static_fallback("walkDomJson result was not a JSON object");
        }

        DesignIR ir;
        try {
            ir.source = DesignSource::claude;
            ir.root = json_to_ir_node(tree);
            ir.root.type = "frame";
            if (ir.root.name.empty()) ir.root.name = "ClaudeImport";
        } catch (const std::exception& e) {
            return static_fallback(std::string("json_to_ir_node failed: ") + e.what());
        } catch (...) {
            return static_fallback("json_to_ir_node failed: unknown exception");
        }

        // Success bar from the issue: more than 9 elements means the
        // walker actually saw the React commit, not just the loader
        // shell. If we're at-or-below the static-parse floor, fall back
        // so the caller never gets worse than the current behavior.
        size_t nodes = count_ir_nodes(ir.root);
        if (nodes <= 9) {
            // Native JSX live bundles route ReactDOM through @pulp/react, so
            // there may be no expanded DOM for walkDomJson() to harvest. In
            // that case the WidgetBridge root is the materialized tree; freeze
            // that native tree into DesignIR so the same bundle can still emit
            // baked IR/C++.
            if (root.child_count() > 0) {
                DesignIR native_ir;
                native_ir.source = DesignSource::claude;
                native_ir.root = view_to_ir_node(root, "root");
                native_ir.root.type = "frame";
                if (native_ir.root.name.empty()) native_ir.root.name = "ClaudeImport";
                const size_t native_nodes = count_ir_nodes(native_ir.root);
                if (native_nodes > 9) {
                    if (opts.error_out) opts.error_out->clear();
                    native_ir.root.provenance = IRProvenance{"claude-native-view", "1", {}};
                    native_ir.root.confidence = IRConfidence::pass;
                    assign_anchors(native_ir.root, AnchorStrategy::content_hash);
                    native_ir.capture_method = "runtime_native_snapshot";
                    native_ir.settle_rounds = 4;
                    native_ir.source_adapter = "claude-native-view";
                    native_ir.source_version = "1";
                    return native_ir;
                }
            }
            std::ostringstream ss;
            ss << "runtime walker produced only " << nodes
               << " nodes (<= loader-shell floor of 9); falling back to static parser";
            return static_fallback(ss.str());
        }

        // Clear error_out on success.
        if (opts.error_out) opts.error_out->clear();

        // Phase 0a: stamp provenance and assign anchors on the
        // runtime-walked DOM tree. The runtime walker doesn't have
        // native IDs (DOM `id` attrs are author-supplied and not
        // guaranteed unique across re-imports), so content-hash is the
        // right strategy — matches DEFAULT_ANCHOR_STRATEGY for
        // claude-design-html. Promotion runs before anchors because
        // content-hash anchors include node.type.
        ir.root.provenance = IRProvenance{"claude-design-html", "1", {}};
        ir.root.confidence = IRConfidence::pass;  // walker ran successfully
        promote_interactive_frames(ir.root);
        assign_anchors(ir.root, AnchorStrategy::content_hash);
        ir.capture_method = "runtime_snapshot";
        ir.settle_rounds = 4;
        ir.source_adapter = "claude-design-html";
        ir.source_version = "1";
        return ir;
    } catch (const std::exception& e) {
        return static_fallback(std::string("harness boot failed: ") + e.what());
    } catch (...) {
        return static_fallback("harness boot failed: unknown exception");
    }
}

// ── Runtime-import: evaluate a Claude bundle in the LIVE bridge engine ──
//
// Phase 6 of pulp-runtime-import-FINAL-design.md. The offline path
// (parse_claude_html_with_runtime, above) allocates its own
// ScriptEngine/View/StateStore/WidgetBridge harness, then walks the
// materialized DOM into a DesignIR. The runtime path is different:
// the caller's `@pulp/react` reconciler is already mounted on `engine_`,
// and we want the bundle's React components to render through it
// instead of into a throwaway sandbox. So we:
//
//   1. Install the same set of pre-payload shims (navigator.userAgent,
//      HTML*Element constructors, document.createElementNS, addEventListener
//      on document/window/globalThis, ReactDOM.createRoot autoflushSync).
//   2. Evaluate the bundle's `text/javascript` asset payloads, BUT
//      preserve any host-installed `globalThis.React` / `globalThis.ReactDOM`
//      that the JS-side `installHostReact` + `installReactDOMCapture`
//      shims placed before this call (codex amendment #2 — the bundle's
//      React payload must not clobber the reconciler's React).
//   3. Re-inject JSON-kind inline scripts (tweak-defaults pattern) and
//      evaluate text/javascript + text/babel inline scripts in document
//      order (Babel.transform → eval for JSX).
//   4. Dispatch readystatechange → DOMContentLoaded → load so bundles
//      that defer boot to document-ready actually fire.
//
// Explicitly NOT done here:
//   - buildDom / walkDomJson (offline-only; we have a live React tree).
//   - Pumping the message loop (the JS side calls __pulpRuntimeSettle__
//     separately so it can control how many drain rounds run).
//   - Allocating any new engine / view / bridge (use this->engine_).
//   - Static-fallback path (this returns void; soft-errors propagate via
//     globalThis.__pulpPayloadErr_<idx>__ / __pulpEvalErr__ slots and the
//     WidgetBridge's normal error surface, not a DesignIR replacement).
//
// Token budget: this duplicates roughly the shim + asset-eval + inline-eval
// + DOMContentLoaded blocks of parse_claude_html_with_runtime. The
// codex-anticipated factoring (a shared helper called from both paths) is
// the natural next step once both functions live side-by-side and a
// concrete duplication count is visible.
void WidgetBridge::evaluate_claude_bundle_in_live_engine(const ClaudeBundle& bundle) {
    // The live-engine path differs from the offline harness in three
    // small ways: it uses a different navigator.userAgent, it must
    // preserve any host-installed globalThis.React / ReactDOM around the
    // bundle's asset eval loop (so the bundle's bundled react.development.js
    // can't displace the live reconciler's copy — codex amendment #2),
    // and the autoflushSync ReactDOM.createRoot patch is guarded by an
    // idempotency marker since the same ReactDOM instance may be
    // re-entered on subsequent calls. Everything else (shims, inline-script
    // eval, DOMContentLoaded dispatch) is identical, so the shared helper
    // does the heavy lifting.
    //
    // We deliberately do NOT pump the message loop or run walkDomJson
    // here — the JS side calls __pulpRuntimeSettle__ separately so it
    // can control how many drain rounds run, and the live React tree
    // replaces the offline DOM walker. Soft errors propagate via
    // globalThis.__pulpPayloadErr_<idx>__ / __pulpEvalErr__ slots, not
    // via a caller-side sink.
    ImportShimConfig cfg;
    cfg.user_agent = "PulpImportRuntime/1.0";
    cfg.preserve_host_react = true;
    cfg.guard_autoflush_idempotent = true;
    cfg.report_error = nullptr;  // runtime path swallows; JS reads slots.
    run_claude_bundle_payload_pipeline(engine_, bundle, cfg);
}

std::string render_claude_bridge_scaffold(const std::string& generated_js_path) {
    std::ostringstream ss;
    ss <<
"// Starter bridge handlers for the imported Claude Design surface.\n"
"//\n"
"// Generated by `pulp import-design --from claude`. Edit the\n"
"// add_handler() registrations below to map message `type` strings\n"
"// emitted by your editor to processor-side state changes. Each\n"
"// handler receives the parsed payload and returns a JSON response\n"
"// envelope built via EditorBridge::ok_response() / err_response().\n"
"//\n"
"// Imported JS view: " << generated_js_path << "\n"
"//\n"
"// See docs/reference/editor-bridge.md for the full API and the\n"
"// standard error vocabulary (malformed_json, unknown_type,\n"
"// missing_field, wrong_type, internal_error).\n"
"\n"
"#include <pulp/view/editor_bridge.hpp>\n"
"#include <pulp/view/web_view.hpp>\n"
"\n"
"#include <memory>\n"
"\n"
"namespace {\n"
"\n"
"// Replace `MyPluginEditor` with the editor type that owns the\n"
"// WebViewPanel (or native JS runtime) and the processor reference.\n"
"class MyPluginEditor {\n"
"public:\n"
"    void wire_bridge(pulp::view::WebViewPanel& panel) {\n"
"        // TODO: register one add_handler() per message `type` your\n"
"        // editor JS will postMessage to the processor.\n"
"        bridge_.add_handler(\"hello\", [this](const auto& /*payload*/) {\n"
"            // ... mutate processor state here ...\n"
"            return pulp::view::EditorBridge::ok_response();\n"
"        });\n"
"\n"
"        // Example with a typed payload field:\n"
"        bridge_.add_handler(\"set_value\", [this](const auto& payload) {\n"
"            const auto v = pulp::view::EditorBridge::get_float(\n"
"                payload, \"value\", 0.0f);\n"
"            // ... apply v ...\n"
"            (void)v;\n"
"            return pulp::view::EditorBridge::ok_response();\n"
"        });\n"
"\n"
"        bridge_.attach_webview(panel);\n"
"    }\n"
"\n"
"    // For the pulp #468 native-JS-runtime import lane, swap the\n"
"    // attach_webview() call above for:\n"
"    //   bridge_.attach_native_runtime(runtime, \"<handler_name>\");\n"
"\n"
"private:\n"
"    pulp::view::EditorBridge bridge_;\n"
"};\n"
"\n"
"} // namespace\n";
    return ss.str();
}

} // namespace pulp::view
