#include <pulp/view/design_import.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/zip.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/view.hpp>
#include <pulp/state/store.hpp>
#include <choc/text/choc_JSON.h>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>
#include <cmath>
#include <map>
#include <unordered_set>

namespace pulp::view {

// ── Design source helpers ───────────────────────────────────────────────

std::optional<DesignSource> parse_design_source(const std::string& name) {
    if (name == "figma")  return DesignSource::figma;
    if (name == "stitch") return DesignSource::stitch;
    if (name == "v0")     return DesignSource::v0;
    if (name == "pencil") return DesignSource::pencil;
    if (name == "claude") return DesignSource::claude;
    return std::nullopt;
}

const char* design_source_name(DesignSource source) {
    switch (source) {
        case DesignSource::figma:  return "Figma";
        case DesignSource::stitch: return "Stitch";
        case DesignSource::v0:     return "v0";
        case DesignSource::pencil: return "Pencil";
        case DesignSource::claude: return "Claude Design";
    }
    return "unknown";
}

DesignIR parse_claude_html(const std::string& html) {
    // Claude Design exports the same HTML+CSS shape as other web tools,
    // so parse with the existing Stitch HTML pipeline and re-tag the
    // source. Per pulp #468 (manual-export framing), users hand the
    // exported HTML over directly — no Anthropic API integration.
    auto ir = parse_stitch_html(html);
    ir.source = DesignSource::claude;
    return ir;
}

// ── Claude Design classname extraction (pulp #1035) ─────────────────────
//
// Mirrors Spectr's `tools/extract-html-bundle/extract.mjs` classname
// pass: pull every `<style>...</style>` block, parse the CSS rules, and
// emit `classname → { cssProp(camelCase): cssValue, ... }`. The
// `@pulp/css-adapt` layer downstream consumes this map to merge
// class-based styles into inline before forwarding to bridge calls.

namespace {

// Convert a CSS hyphen-cased property name (`font-family`) to a
// JS-friendly camelCase key (`fontFamily`). Mirrors Spectr's
// `parseDeclarationsToCamelCase` so the artifacts stay byte-compatible.
// Pure string-ops — no allocation beyond the result.
std::string css_prop_to_camel_case(const std::string& prop) {
    std::string out;
    out.reserve(prop.size());
    bool upper_next = false;
    for (char c : prop) {
        if (c == '-') {
            upper_next = true;
        } else if (upper_next) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            upper_next = false;
        } else {
            out += c;
        }
    }
    return out;
}

// Trim ASCII whitespace from both ends of a string view in place.
std::string trim_ascii_ws(std::string_view sv) {
    size_t i = 0, j = sv.size();
    while (i < j && std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(sv[j - 1]))) --j;
    return std::string(sv.substr(i, j - i));
}

// Strip CSS `/* ... */` comments from a block. Multi-line safe. We
// don't need to be a full CSS tokenizer — Claude Design exports use
// vanilla rules, not CSS-in-JS or nested at-rules.
std::string strip_css_comments(const std::string& css) {
    std::string out;
    out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            auto end = css.find("*/", i + 2);
            if (end == std::string::npos) break;  // unterminated — drop rest
            i = end + 2;
        } else {
            out += css[i++];
        }
    }
    return out;
}

// Parse a CSS declaration block body (the text between `{` and `}`)
// into camelCase prop → value pairs. Splits on `;`, then on the first
// `:` per declaration. Skips empty declarations and bare colons.
std::map<std::string, std::string> parse_css_declarations(const std::string& body) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        auto semi = body.find(';', i);
        std::string decl = body.substr(i, (semi == std::string::npos ? body.size() : semi) - i);
        i = (semi == std::string::npos) ? body.size() : semi + 1;
        auto colon = decl.find(':');
        if (colon == std::string::npos) continue;
        auto prop = trim_ascii_ws(std::string_view(decl).substr(0, colon));
        auto value = trim_ascii_ws(std::string_view(decl).substr(colon + 1));
        if (prop.empty() || value.empty()) continue;
        out[css_prop_to_camel_case(prop)] = value;
    }
    return out;
}

// Walk a CSS source string (the inside of one `<style>` block) and
// merge every `.classname { ... }` rule into `into`. Skips at-rules
// (anything that begins with `@`), `:root` blocks, descendant /
// pseudo-class selectors (anything with whitespace, `:`, `>` or `,`
// between the dot and the `{`), and `.scheme-*` selectors (those are
// already handled as theme-mode token overrides upstream).
//
// We hand-walk character-by-character rather than regex because:
//   1. CSS values can contain `{` (e.g. `linear-gradient(...)`) — but
//      not at the top level of a declaration block.
//   2. The selector list can include commas, so we need to split on
//      `,` and apply the same body to every classname in the list.
void collect_classnames_from_css(const std::string& css_in,
                                 ClaudeClassNameRules& into) {
    auto css = strip_css_comments(css_in);
    size_t i = 0;
    while (i < css.size()) {
        // Skip whitespace.
        while (i < css.size() && std::isspace(static_cast<unsigned char>(css[i]))) ++i;
        if (i >= css.size()) break;

        // Find the next `{` that opens a rule body. Anything between
        // `i` and that brace is the selector list.
        auto open = css.find('{', i);
        if (open == std::string::npos) break;
        std::string selector_list = css.substr(i, open - i);

        // Find the matching `}`. Top-level only — declaration values
        // never embed `{...}` braces in well-formed CSS.
        auto close = css.find('}', open + 1);
        if (close == std::string::npos) break;
        std::string body = css.substr(open + 1, close - (open + 1));
        i = close + 1;

        // Skip at-rules (`@media`, `@font-face`, `@keyframes`, etc.).
        // The first non-whitespace char of the selector tells us.
        auto first_non_ws = selector_list.find_first_not_of(" \t\r\n");
        if (first_non_ws == std::string::npos) continue;
        if (selector_list[first_non_ws] == '@') continue;

        // Split selector_list on top-level commas.
        std::vector<std::string> selectors;
        size_t s_start = 0;
        for (size_t k = 0; k <= selector_list.size(); ++k) {
            if (k == selector_list.size() || selector_list[k] == ',') {
                selectors.push_back(trim_ascii_ws(
                    std::string_view(selector_list).substr(s_start, k - s_start)));
                s_start = k + 1;
            }
        }

        // Parse the body once — every matching selector references the
        // same map.
        std::optional<std::map<std::string, std::string>> decls;

        for (auto& sel : selectors) {
            // Only accept simple `.classname` selectors. The classname
            // grammar matches Spectr's regex: `[a-zA-Z][a-zA-Z0-9_-]*`.
            // A trailing chained selector (`.foo .bar`, `.foo > .bar`,
            // `.foo:hover`, `.foo[data-x]`) means this isn't a plain
            // classname rule — skip it.
            if (sel.empty() || sel[0] != '.') continue;
            std::string name;
            size_t k = 1;
            if (k >= sel.size() || !(std::isalpha(static_cast<unsigned char>(sel[k])) || sel[k] == '_'))
                continue;
            while (k < sel.size() &&
                   (std::isalnum(static_cast<unsigned char>(sel[k])) ||
                    sel[k] == '_' || sel[k] == '-')) {
                name += sel[k++];
            }
            // Anything left over → not a plain classname selector.
            if (k != sel.size()) continue;
            if (name.empty()) continue;
            // Theme-scope selectors are handled upstream as token
            // overrides, not classname rules.
            if (name.rfind("scheme-", 0) == 0) continue;

            if (!decls) decls = parse_css_declarations(body);
            if (decls->empty()) continue;

            // Cascade: later blocks override earlier ones for the same
            // classname. Per-prop merge keeps unrelated declarations
            // from being lost when two blocks define the same class.
            auto& existing = into[name];
            for (auto& [prop, val] : *decls) {
                existing[prop] = val;
            }
        }
    }
}

// Pull every `<style>...</style>` block out of an HTML string, in
// document order. Skips `<style>` blocks whose first 200 chars contain
// `font-face` (matches Spectr's filter — those carry only `@font-face`
// rules, no classnames). Returns the inner CSS bodies.
std::vector<std::string> extract_html_style_blocks(const std::string& html) {
    std::vector<std::string> blocks;
    static const std::regex style_re(
        R"RX(<style\b[^>]*>([\s\S]*?)</style>)RX",
        std::regex::icase);
    auto begin = std::sregex_iterator(html.begin(), html.end(), style_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string body = (*it)[1].str();
        // Spectr's filter: skip blocks whose head looks like @font-face.
        // The head check (first 200 chars) keeps a normal classname
        // block that happens to mention `font-face` later from being
        // dropped.
        std::string head = body.substr(0, std::min<size_t>(body.size(), 200));
        std::transform(head.begin(), head.end(), head.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (head.find("font-face") != std::string::npos) continue;
        blocks.push_back(std::move(body));
    }
    return blocks;
}

} // namespace

ClaudeClassNameRules extract_claude_classnames(const std::string& html) {
    ClaudeClassNameRules rules;

    // Walk the raw HTML's <style> blocks first. For non-bundled
    // exports (the `--no-bundle` flow, or hand-written test fixtures)
    // this is the only source.
    for (auto& css : extract_html_style_blocks(html)) {
        collect_classnames_from_css(css, rules);
    }

    // For self-bundled Claude Design exports, the actual app CSS lives
    // inside the `<script type="__bundler/template">` payload (a
    // JSON-encoded HTML string). Unwrap and walk its <style> blocks
    // too. parse_claude_bundle silently returns nullopt when the
    // envelope is missing — that's the expected branch for hand-rolled
    // fixtures, so we just fall through.
    if (auto bundle = parse_claude_bundle(html)) {
        for (auto& css : extract_html_style_blocks(bundle->template_html)) {
            collect_classnames_from_css(css, rules);
        }
    }

    return rules;
}

std::string serialize_claude_classnames(const ClaudeClassNameRules& rules) {
    // Use choc::value::createObject for stable, well-escaped JSON. The
    // outer map is a std::map so keys arrive in alphabetical order
    // already; per-class declaration maps are also std::map for the
    // same property-order guarantee. choc::json::toString preserves
    // insertion order, so the resulting JSON is deterministic.
    auto root = choc::value::createObject("");
    for (const auto& [name, decls] : rules) {
        auto obj = choc::value::createObject("");
        for (const auto& [prop, val] : decls) {
            obj.addMember(prop, val);
        }
        root.addMember(name, obj);
    }
    // Pretty-print with line breaks so the artifact is human-readable
    // and diff-friendly (matches Spectr's `JSON.stringify(_, null, 2)`
    // output shape for parity with the existing tooling).
    return choc::json::toString(root, /*useLineBreaks=*/true);
}

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

// JSON-encode an arbitrary string for safe injection into a JS string literal.
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

struct V0ReactSource {
    std::string source;
    std::string file_name;
};

std::string v0_trim(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string v0_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool v0_contains_ci(const std::string& haystack, const std::string& needle) {
    auto h = v0_lower(haystack);
    auto n = v0_lower(needle);
    return h.find(n) != std::string::npos;
}

bool v0_ends_with_ci(const std::string& s, const std::string& suffix) {
    auto ls = v0_lower(s);
    auto lf = v0_lower(suffix);
    return ls.size() >= lf.size() &&
           ls.compare(ls.size() - lf.size(), lf.size(), lf) == 0;
}

bool v0_is_react_file(const std::string& path) {
    return v0_ends_with_ci(path, ".tsx") || v0_ends_with_ci(path, ".jsx") ||
           v0_ends_with_ci(path, ".ts") || v0_ends_with_ci(path, ".js");
}

std::string v0_extract_file_attr(const std::string& header) {
    static const std::regex file_re(R"RX(\bfile\s*=\s*"([^"]+)")RX");
    std::smatch m;
    if (std::regex_search(header, m, file_re)) return m[1].str();
    return {};
}

std::optional<V0ReactSource> v0_extract_source(const std::string& input) {
    if (input.find("[V0_FILE]") == std::string::npos) {
        auto src = v0_trim(input);
        if (src.empty()) return std::nullopt;
        return V0ReactSource{std::move(src), "component.tsx"};
    }

    std::vector<V0ReactSource> candidates;
    size_t marker = 0;
    while ((marker = input.find("[V0_FILE]", marker)) != std::string::npos) {
        auto header_end = input.find('\n', marker);
        auto header = input.substr(marker, header_end == std::string::npos
            ? std::string::npos
            : header_end - marker);
        auto content_begin = header_end == std::string::npos ? input.size() : header_end + 1;
        auto next = input.find("\n[V0_FILE]", content_begin);
        auto content_end = next == std::string::npos ? input.size() : next;

        auto file_name = v0_extract_file_attr(header);
        if (v0_is_react_file(file_name)) {
            auto body = v0_trim(input.substr(content_begin, content_end - content_begin));
            if (!body.empty()) {
                candidates.push_back({std::move(body), std::move(file_name)});
            }
        }

        marker = next == std::string::npos ? input.size() : next + 1;
    }

    if (candidates.empty()) return std::nullopt;

    auto score = [](const V0ReactSource& c) {
        auto name = v0_lower(c.file_name);
        int s = 0;
        if (name == "app/page.tsx" || name == "src/app/page.tsx") s += 100;
        if (name.find("/page.tsx") != std::string::npos) s += 80;
        if (v0_ends_with_ci(name, ".tsx")) s += 20;
        if (c.source.find("export default") != std::string::npos) s += 10;
        return s;
    };

    return *std::max_element(candidates.begin(), candidates.end(),
        [&](const auto& a, const auto& b) { return score(a) < score(b); });
}

bool v0_import_statement_is_supported(const std::string& statement) {
    static const std::regex from_re(R"RX(\bfrom\s*["']([^"']+)["'])RX");
    static const std::regex side_effect_re(R"RX(^\s*import\s*["'][^"']+["'])RX");
    if (std::regex_search(statement, side_effect_re)) return false;

    std::smatch m;
    if (!std::regex_search(statement, m, from_re)) return false;
    return m[1].str() == "react";
}

bool v0_imports_are_supported(const std::string& source) {
    std::istringstream lines(source);
    std::string line;
    std::string statement;
    bool in_import = false;

    auto starts_import = [](const std::string& t) {
        if (t.rfind("import", 0) != 0) return false;
        return t.size() == 6 ||
               (!std::isalnum(static_cast<unsigned char>(t[6])) && t[6] != '_');
    };
    static const std::regex from_re(R"RX(\bfrom\s*["'][^"']+["'])RX");
    static const std::regex side_effect_re(R"RX(^\s*import\s*["'][^"']+["'])RX");

    while (std::getline(lines, line)) {
        auto t = v0_trim(line);
        if (!in_import) {
            if (!starts_import(t)) continue;
            statement = t;
        } else {
            statement += ' ';
            statement += t;
        }

        if (std::regex_search(statement, from_re) ||
            std::regex_search(statement, side_effect_re) ||
            statement.find(';') != std::string::npos) {
            if (!v0_import_statement_is_supported(statement)) return false;
            statement.clear();
            in_import = false;
        } else {
            in_import = true;
        }
    }

    return !in_import;
}

bool v0_uses_only_supported_surfaces(const std::string& source) {
    if (!v0_imports_are_supported(source)) return false;

    const auto lower = v0_lower(source);
    const char* unsupported_markers[] = {
        "classname", "@/components", "@radix-ui", "radix-ui", "shadcn",
        "next/", "next\\", "next/dynamic", "lucide-react", "framer-motion",
        "tailwind", "clsx(", "cva(", "cn(", "fetch(", "xmlhttprequest",
        "localstorage", "sessionstorage", "indexeddb", "websocket",
        "serviceworker", "sharedworker", "new worker", "broadcastchannel",
        "settimeout(", "setinterval(", "document.", "window.", "navigator.",
        "history.", "location.", "dangerouslysetinnerhtml",
        "<form", "<select", "<textarea", "<iframe"
    };
    for (const char* marker : unsupported_markers) {
        if (lower.find(marker) != std::string::npos) return false;
    }

    static const std::regex dynamic_import_re(R"RX(\bimport\s*\()RX");
    if (std::regex_search(source, dynamic_import_re)) return false;

    static const std::regex input_tag_re(R"RX(<input\b)RX", std::regex::icase);
    static const std::regex input_type_re(
        R"RX(<input\b[^>]*\btype\s*=\s*(?:"([^"]+)"|'([^']+)'))RX",
        std::regex::icase);
    const auto input_tag_count = static_cast<size_t>(std::distance(
        std::sregex_iterator(source.begin(), source.end(), input_tag_re),
        std::sregex_iterator()));
    size_t typed_input_count = 0;
    auto input_begin = std::sregex_iterator(source.begin(), source.end(), input_type_re);
    auto input_end = std::sregex_iterator();
    for (auto it = input_begin; it != input_end; ++it) {
        ++typed_input_count;
        std::string type = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
        if (v0_lower(type) != "range") return false;
    }
    if (typed_input_count != input_tag_count) return false;

    static const std::regex tag_re(R"RX(<\s*/?\s*([A-Za-z][A-Za-z0-9]*)(?=[\s>/]))RX");
    static const std::unordered_set<std::string> supported = {
        "div", "span", "button", "canvas", "svg", "path", "rect", "circle",
        "image", "input", "img", "p", "h1", "h2", "h3", "h4", "h5", "h6"
    };
    static const std::unordered_set<std::string> ts_type_names = {
        "HTMLCanvasElement", "HTMLDivElement", "HTMLInputElement", "HTMLElement",
        "SVGElement", "ReactElement"
    };

    auto tag_begin = std::sregex_iterator(source.begin(), source.end(), tag_re);
    auto tag_end = std::sregex_iterator();
    for (auto it = tag_begin; it != tag_end; ++it) {
        auto tag = (*it)[1].str();
        if (!tag.empty() && std::isupper(static_cast<unsigned char>(tag[0]))) {
            if (ts_type_names.count(tag) != 0) continue;
            return false;
        }
        if (supported.count(v0_lower(tag)) == 0) return false;
    }

    return true;
}

std::optional<std::string> v0_match_first(const std::string& source, const std::regex& re) {
    std::smatch m;
    if (!std::regex_search(source, m, re)) return std::nullopt;
    for (size_t i = 1; i < m.size(); ++i) {
        if (m[i].matched) return v0_trim(m[i].str());
    }
    return std::nullopt;
}

std::string v0_extract_component_name(const std::string& source) {
    static const std::regex export_fn_re(
        R"RX(export\s+default\s+function\s+([A-Za-z_][A-Za-z0-9_]*))RX");
    if (auto m = v0_match_first(source, export_fn_re)) return *m;
    return "V0RuntimeImport";
}

std::string v0_slug_from_component(std::string name) {
    std::string out = "v0";
    for (char c : name) {
        if (std::isupper(static_cast<unsigned char>(c)) && !out.empty() && out.back() != '-') {
            out += '-';
        }
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == '-' || c == '_') {
            if (!out.empty() && out.back() != '-') out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "v0-runtime-import" : out;
}

std::string v0_extract_root_id(const std::string& source, const std::string& component_name) {
    static const std::regex id_re(R"RX(\bid\s*=\s*(?:"([^"]+)"|'([^']+)'))RX");
    if (auto m = v0_match_first(source, id_re)) return *m;
    return v0_slug_from_component(component_name);
}

size_t v0_count_tag(const std::string& source, const char* tag) {
    std::regex re(std::string(R"RX(<\s*)RX") + tag + R"RX(\b)RX", std::regex::icase);
    return static_cast<size_t>(std::distance(
        std::sregex_iterator(source.begin(), source.end(), re),
        std::sregex_iterator()));
}

void v0_push_unique(std::vector<std::string>& values, std::string value) {
    value = v0_trim(std::move(value));
    if (value.empty()) return;
    if (value.size() > 80) return;
    if (value[0] == '#') return;
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

std::vector<std::string> v0_extract_texts(const std::string& source, const char* tag) {
    std::vector<std::string> out;
    std::regex re(std::string(R"RX(<\s*)RX") + tag +
        R"RX(\b[^>]*>\s*([^<>{}]+?)\s*</\s*)RX" + tag + R"RX(\s*>)RX",
        std::regex::icase);
    auto begin = std::sregex_iterator(source.begin(), source.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        v0_push_unique(out, (*it)[1].str());
    }
    return out;
}

std::string v0_json_array(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ",";
        out += json_string_literal(values[i]);
    }
    out += "]";
    return out;
}

std::string v0_html_attr_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string replace_all_copy(std::string s,
                             const std::string& needle,
                             const std::string& replacement) {
    if (needle.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        s.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return s;
}

std::string v0_build_runtime_js(const std::string& source,
                                const std::string& file_name,
                                const std::string& component_name,
                                const std::string& root_id) {
    auto headings = v0_extract_texts(source, "h[1-6]");
    auto paragraphs = v0_extract_texts(source, "p");
    auto spans = v0_extract_texts(source, "span");
    auto buttons = v0_extract_texts(source, "button");

    const size_t canvas_count = v0_count_tag(source, "canvas");
    const size_t slider_count = v0_count_tag(source, "input");
    size_t button_count = v0_count_tag(source, "button");

    if (spans.empty()) spans = {"Control"};
    while (spans.size() < slider_count) {
        spans.push_back("Control " + std::to_string(spans.size() + 1));
    }
    if (buttons.empty() && button_count > 0) {
        buttons = {"Armed", "Bypass", "Play", "Stop"};
    }

    const auto title = headings.empty() ? component_name : headings.front();
    const auto subtitle = paragraphs.empty()
        ? std::string("v0.dev React runtime import")
        : paragraphs.front();

    std::ostringstream js;
    js << "(function(){\n"
       << "  var React = globalThis.React;\n"
       << "  var ReactDOM = globalThis.ReactDOM;\n"
       << "  if (!React || !ReactDOM || typeof ReactDOM.createRoot !== 'function') {\n"
       << "    throw new Error('v0.dev runtime import requires host React and ReactDOM');\n"
       << "  }\n"
       << "  var h = React.createElement;\n"
       << "  var rootId = " << json_string_literal(root_id) << ";\n"
       << "  var title = " << json_string_literal(title) << ";\n"
       << "  var subtitle = " << json_string_literal(subtitle) << ";\n"
       << "  var sourceFile = " << json_string_literal(file_name) << ";\n"
       << "  var sliderLabels = " << v0_json_array(spans) << ";\n"
       << "  var buttonLabels = " << v0_json_array(buttons) << ";\n"
       << "  var hasCanvas = " << (canvas_count > 0 ? "true" : "false") << ";\n"
       << "  var sliderCount = " << slider_count << ";\n"
       << "  var buttonCount = " << button_count << ";\n"
       << "  function App(){\n"
       << "    var canvasRef = React.useRef(null);\n"
       << "    var levelState = React.useState(0.55);\n"
       << "    var level = levelState[0];\n"
       << "    var setLevel = levelState[1];\n"
       << "    var enabledState = React.useState(true);\n"
       << "    var enabled = enabledState[0];\n"
       << "    var setEnabled = enabledState[1];\n"
       << "    React.useEffect(function(){\n"
       << "      if (!hasCanvas) return function(){};\n"
       << "      var active = true;\n"
       << "      function draw(){\n"
       << "        var canvas = canvasRef.current;\n"
       << "        if (canvas && typeof canvas.getContext === 'function') {\n"
       << "          var ctx = canvas.getContext('2d');\n"
       << "          if (ctx) {\n"
       << "            var width = canvas.width || 384;\n"
       << "            var height = canvas.height || 112;\n"
       << "            var t = ((typeof performance !== 'undefined' && performance.now) ? performance.now() : 0) / 1000;\n"
       << "            var peak = enabled ? Math.max(0, Math.min(1, 0.35 + level * 0.45 + Math.sin(t * 2.4) * 0.12)) : 0.12;\n"
       << "            var bars = 22;\n"
       << "            var gap = 3;\n"
       << "            var barWidth = (width - gap * (bars - 1)) / bars;\n"
       << "            ctx.clearRect(0, 0, width, height);\n"
       << "            ctx.fillStyle = '#111827';\n"
       << "            ctx.fillRect(0, 0, width, height);\n"
       << "            for (var i = 0; i < bars; i++) {\n"
       << "              var ratio = (i + 1) / bars;\n"
       << "              var barHeight = Math.max(6, height * ratio * 0.86);\n"
       << "              ctx.fillStyle = ratio <= peak ? (ratio > 0.82 ? '#f97316' : '#22c55e') : '#263244';\n"
       << "              ctx.fillRect(i * (barWidth + gap), height - barHeight, barWidth, barHeight);\n"
       << "            }\n"
       << "            ctx.fillStyle = '#d1d5db';\n"
       << "            ctx.font = '12px Inter, sans-serif';\n"
       << "            ctx.fillText(sourceFile.indexOf('/') >= 0 ? 'v0' : 'OUT', 8, 18);\n"
       << "          }\n"
       << "        }\n"
       << "        if (active && typeof requestAnimationFrame === 'function') requestAnimationFrame(draw);\n"
       << "      }\n"
       << "      draw();\n"
       << "      return function(){ active = false; };\n"
       << "    }, [enabled, level]);\n"
       << "    var panelStyle = { width: 420, minHeight: 280, display: 'flex', flexDirection: 'column', gap: 16, padding: 18, backgroundColor: '#0b1020', color: '#f9fafb', border: '1px solid #334155', borderRadius: 8, fontFamily: 'Inter, system-ui, sans-serif' };\n"
       << "    var rowStyle = { display: 'flex', flexDirection: 'row', gap: 12, alignItems: 'center' };\n"
       << "    var controlStyle = { display: 'flex', flexDirection: 'column', gap: 6, flexGrow: 1 };\n"
       << "    var buttonStyle = { minWidth: 88, minHeight: 36, borderRadius: 6, border: '1px solid #475569', backgroundColor: enabled ? '#14532d' : '#1f2937', color: '#f8fafc', cursor: 'pointer' };\n"
       << "    var buttonChildren = [];\n"
       << "    for (var b = 0; b < buttonCount; b++) {\n"
       << "      buttonChildren.push(h('button', { key: 'button-' + b, type: 'button', onClick: function(){ setEnabled(!enabled); }, style: buttonStyle }, buttonLabels[b] || (enabled ? 'Armed' : 'Bypass')));\n"
       << "    }\n"
       << "    var sliderChildren = [];\n"
       << "    for (var s = 0; s < sliderCount; s++) {\n"
       << "      sliderChildren.push(h('div', { key: 'slider-' + s, style: controlStyle },\n"
       << "        h('span', { style: { fontSize: 12, color: '#cbd5e1' } }, sliderLabels[s] || ('Control ' + (s + 1))),\n"
       << "        h('input', { type: 'range', min: 0, max: 1, step: 0.01, value: level, onChange: function(event){ setLevel(Number(event.currentTarget.value)); } }),\n"
       << "        h('span', { style: { fontSize: 12, color: '#94a3b8' } }, String(Math.round(level * 100)) + '%')));\n"
       << "    }\n"
       << "    var children = [\n"
       << "      h('div', { key: 'header', style: Object.assign({}, rowStyle, { justifyContent: 'space-between' }) },\n"
       << "        h('div', null,\n"
       << "          h('h2', { style: { margin: 0, fontSize: 22, lineHeight: 1.1 } }, title),\n"
       << "          h('p', { style: { margin: '6px 0 0', color: '#94a3b8', fontSize: 13 } }, subtitle)),\n"
       << "        h('div', { style: rowStyle }, buttonChildren))\n"
       << "    ];\n"
       << "    if (hasCanvas) children.push(h('canvas', { key: 'canvas', ref: canvasRef, width: 384, height: 112, style: { width: '100%', height: 112, borderRadius: 6, border: '1px solid #1f2937', backgroundColor: '#111827' } }));\n"
       << "    if (sliderCount > 0) children.push(h('div', { key: 'sliders', style: rowStyle }, sliderChildren));\n"
       << "    return h('div', { id: rootId, style: panelStyle, 'data-pulp-source': 'v0' }, children);\n"
       << "  }\n"
       << "  var mount = document.getElementById('root') || document.body || document.documentElement;\n"
       << "  ReactDOM.createRoot(mount).render(h(App));\n"
       << "})();\n";
    return js.str();
}

bool figma_has_source_signal(const std::string& source) {
    const auto lower = v0_lower(source);
    return lower.find("source: figma") != std::string::npos ||
           lower.find("figma make") != std::string::npos ||
           lower.find("figma-make") != std::string::npos ||
           lower.find("figma:asset/") != std::string::npos ||
           lower.find("@figma/code-connect") != std::string::npos;
}

bool figma_has_versioned_import(const std::string& source) {
    static const std::regex from_re(
        R"RX(\bfrom\s*["'][^"']+@[0-9]+(?:\.[0-9]+){1,2}[^"']*["'])RX",
        std::regex::icase);
    static const std::regex side_effect_re(
        R"RX(\bimport\s*["'][^"']+@[0-9]+(?:\.[0-9]+){1,2}[^"']*["'])RX",
        std::regex::icase);
    return std::regex_search(source, from_re) ||
           std::regex_search(source, side_effect_re);
}

bool figma_uses_only_supported_surfaces(const std::string& source) {
    if (!figma_has_source_signal(source)) return false;

    const auto lower = v0_lower(source);
    const char* figma_reject_markers[] = {
        "\"use client\"", "'use client'", "figma:asset/",
        "@figma/code-connect", "figma.connect(", "@radix-ui", "radix-ui",
        "tailwind", "classname", "next/", "next\\", "next/dynamic"
    };
    for (const char* marker : figma_reject_markers) {
        if (lower.find(marker) != std::string::npos) return false;
    }
    if (figma_has_versioned_import(source)) return false;

    return v0_uses_only_supported_surfaces(source);
}

std::string figma_slug_from_component(std::string name) {
    std::string out = "figma";
    for (char c : name) {
        if (std::isupper(static_cast<unsigned char>(c)) && !out.empty() && out.back() != '-') {
            out += '-';
        }
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (c == '-' || c == '_') {
            if (!out.empty() && out.back() != '-') out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "figma-runtime-import" : out;
}

std::string figma_extract_root_id(const std::string& source,
                                  const std::string& component_name) {
    static const std::regex id_re(R"RX(\bid\s*=\s*(?:"([^"]+)"|'([^']+)'))RX");
    if (auto m = v0_match_first(source, id_re)) return *m;
    return figma_slug_from_component(component_name);
}

std::string figma_build_runtime_js(const std::string& source,
                                   const std::string& file_name,
                                   const std::string& component_name,
                                   const std::string& root_id) {
    auto js = v0_build_runtime_js(source, file_name, component_name, root_id);
    js = replace_all_copy(js,
        "v0.dev runtime import requires host React and ReactDOM",
        "Figma Make runtime import requires host React and ReactDOM");
    js = replace_all_copy(js,
        "v0.dev React runtime import",
        "Figma Make React runtime import");
    js = replace_all_copy(js,
        "'data-pulp-source': 'v0'",
        "'data-pulp-source': 'figma'");
    js = replace_all_copy(js,
        "sourceFile.indexOf('/') >= 0 ? 'v0' : 'OUT'",
        "sourceFile.indexOf('/') >= 0 ? 'FIGMA' : 'FIG'");
    return js;
}

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

std::optional<ClaudeBundle> parse_v0_dev_react(const std::string& tsx_or_envelope) {
    auto extracted = v0_extract_source(tsx_or_envelope);
    if (!extracted) return std::nullopt;

    const auto& source = extracted->source;
    if (source.find("export default") == std::string::npos) return std::nullopt;
    if (!v0_contains_ci(source, "react")) return std::nullopt;
    if (!v0_uses_only_supported_surfaces(source)) return std::nullopt;

    const auto component_name = v0_extract_component_name(source);
    const auto root_id = v0_extract_root_id(source, component_name);
    auto runtime_js = v0_build_runtime_js(
        source, extracted->file_name, component_name, root_id);

    ClaudeBundleAsset app;
    app.uuid = "v0-runtime-app";
    app.mime = "text/javascript";
    app.data.assign(runtime_js.begin(), runtime_js.end());

    ClaudeBundle bundle;
    bundle.assets.push_back(std::move(app));
    bundle.javascript_indices.push_back(0);
    bundle.template_html =
        "<div id=\"root\" data-pulp-source=\"v0\" data-v0-root=\"" +
        v0_html_attr_escape(root_id) +
        "\"></div><script src=\"v0-runtime-app\"></script>";
    return bundle;
}

std::optional<ClaudeBundle> parse_figma_make_react(const std::string& tsx) {
    auto source = v0_trim(tsx);
    if (source.empty()) return std::nullopt;
    if (source.find("export default") == std::string::npos) return std::nullopt;
    if (!v0_contains_ci(source, "react")) return std::nullopt;
    if (!figma_uses_only_supported_surfaces(source)) return std::nullopt;

    auto component_name = v0_extract_component_name(source);
    if (component_name == "V0RuntimeImport") component_name = "FigmaRuntimeImport";
    const auto root_id = figma_extract_root_id(source, component_name);
    auto runtime_js = figma_build_runtime_js(
        source, "App.tsx", component_name, root_id);

    ClaudeBundleAsset app;
    app.uuid = "figma-runtime-app";
    app.mime = "text/javascript";
    app.data.assign(runtime_js.begin(), runtime_js.end());

    ClaudeBundle bundle;
    bundle.assets.push_back(std::move(app));
    bundle.javascript_indices.push_back(0);
    bundle.template_html =
        "<div id=\"root\" data-pulp-source=\"figma\" data-figma-root=\"" +
        v0_html_attr_escape(root_id) +
        "\"></div><script src=\"figma-runtime-app\"></script>";
    return bundle;
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
        return parse_claude_html(html);
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
            std::ostringstream ss;
            ss << "runtime walker produced only " << nodes
               << " nodes (<= loader-shell floor of 9); falling back to static parser";
            return static_fallback(ss.str());
        }

        // Clear error_out on success.
        if (opts.error_out) opts.error_out->clear();
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

// ── Audio widget detection ──────────────────────────────────────────────

static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

AudioWidgetType detect_audio_widget(const std::string& name) {
    auto lower = to_lower(name);
    if (lower.find("knob") != std::string::npos)      return AudioWidgetType::knob;
    if (lower.find("dial") != std::string::npos)       return AudioWidgetType::knob;
    if (lower.find("fader") != std::string::npos)      return AudioWidgetType::fader;
    if (lower.find("slider") != std::string::npos)     return AudioWidgetType::fader;
    if (lower.find("meter") != std::string::npos)      return AudioWidgetType::meter;
    if (lower.find("level") != std::string::npos)      return AudioWidgetType::meter;
    if (lower.find("vu") != std::string::npos)         return AudioWidgetType::meter;
    if (lower.find("xypad") != std::string::npos)      return AudioWidgetType::xy_pad;
    if (lower.find("xy_pad") != std::string::npos)     return AudioWidgetType::xy_pad;
    if (lower.find("xy pad") != std::string::npos)     return AudioWidgetType::xy_pad;
    if (lower.find("waveform") != std::string::npos)   return AudioWidgetType::waveform;
    if (lower.find("oscilloscope") != std::string::npos) return AudioWidgetType::waveform;
    if (lower.find("spectrum") != std::string::npos)   return AudioWidgetType::spectrum;
    if (lower.find("analyzer") != std::string::npos)   return AudioWidgetType::spectrum;
    if (lower.find("analyser") != std::string::npos)   return AudioWidgetType::spectrum;
    return AudioWidgetType::none;
}

// ── JSON parsing helpers ────────────────────────────────────────────────

static std::string get_string(const choc::value::ValueView& obj, const char* key, const char* def = "") {
    if (obj.hasObjectMember(key))
        return std::string(obj[key].toString());
    return def;
}

static float get_float(const choc::value::ValueView& obj, const char* key, float def = 0.0f) {
    if (obj.hasObjectMember(key))
        return static_cast<float>(obj[key].getWithDefault<double>(def));
    return def;
}

static int get_int(const choc::value::ValueView& obj, const char* key, int def = 0) {
    if (obj.hasObjectMember(key))
        return static_cast<int>(obj[key].getWithDefault<int64_t>(def));
    return def;
}

static bool get_bool(const choc::value::ValueView& obj, const char* key, bool def = false) {
    if (obj.hasObjectMember(key))
        return obj[key].getWithDefault<bool>(def);
    return def;
}

// ── IR from JSON ────────────────────────────────────────────────────────

static IRStyle parse_ir_style(const choc::value::ValueView& obj) {
    IRStyle s;
    if (!obj.isObject()) return s;

    auto set_opt_str = [&](const char* key, std::optional<std::string>& field) {
        if (obj.hasObjectMember(key)) field = std::string(obj[key].toString());
    };
    auto set_opt_float = [&](const char* key, std::optional<float>& field) {
        if (obj.hasObjectMember(key)) field = static_cast<float>(obj[key].getWithDefault<double>(0));
    };
    auto set_opt_int = [&](const char* key, std::optional<int>& field) {
        if (obj.hasObjectMember(key)) field = static_cast<int>(obj[key].getWithDefault<int64_t>(0));
    };

    set_opt_str("backgroundColor", s.background_color);
    set_opt_str("backgroundGradient", s.background_gradient);
    set_opt_str("color", s.color);
    set_opt_float("opacity", s.opacity);
    set_opt_float("borderRadius", s.border_radius);
    set_opt_str("border", s.border);
    set_opt_str("boxShadow", s.box_shadow);
    set_opt_str("filter", s.filter);
    set_opt_str("fontFamily", s.font_family);
    set_opt_float("fontSize", s.font_size);
    set_opt_int("fontWeight", s.font_weight);
    set_opt_str("fontStyle", s.font_style);
    set_opt_str("textAlign", s.text_align);
    set_opt_float("letterSpacing", s.letter_spacing);
    set_opt_float("lineHeight", s.line_height);
    set_opt_str("textTransform", s.text_transform);
    set_opt_str("overflow", s.overflow);
    set_opt_str("cursor", s.cursor);
    set_opt_str("position", s.position);
    set_opt_float("top", s.top);
    set_opt_float("left", s.left);
    set_opt_float("right", s.right);
    set_opt_float("bottom", s.bottom);
    set_opt_int("zIndex", s.z_index);
    set_opt_str("transform", s.transform);
    set_opt_float("width", s.width);
    set_opt_float("height", s.height);
    set_opt_float("minWidth", s.min_width);
    set_opt_float("minHeight", s.min_height);
    set_opt_float("maxWidth", s.max_width);
    set_opt_float("maxHeight", s.max_height);

    return s;
}

static IRLayout parse_ir_layout(const choc::value::ValueView& obj) {
    IRLayout l;
    if (!obj.isObject()) return l;

    auto dir = get_string(obj, "direction", "column");
    l.direction = (dir == "row") ? LayoutDirection::row : LayoutDirection::column;
    l.gap = get_float(obj, "gap");
    l.wrap = get_bool(obj, "wrap");

    // Padding — support uniform or per-side
    if (obj.hasObjectMember("padding")) {
        float p = get_float(obj, "padding");
        l.padding_top = l.padding_right = l.padding_bottom = l.padding_left = p;
    }
    if (obj.hasObjectMember("paddingTop"))    l.padding_top = get_float(obj, "paddingTop");
    if (obj.hasObjectMember("paddingRight"))  l.padding_right = get_float(obj, "paddingRight");
    if (obj.hasObjectMember("paddingBottom")) l.padding_bottom = get_float(obj, "paddingBottom");
    if (obj.hasObjectMember("paddingLeft"))   l.padding_left = get_float(obj, "paddingLeft");

    auto parse_align = [](const std::string& s) -> LayoutAlign {
        if (s == "center")        return LayoutAlign::center;
        if (s == "flex-end" || s == "end") return LayoutAlign::flex_end;
        if (s == "stretch")       return LayoutAlign::stretch;
        if (s == "space-between") return LayoutAlign::space_between;
        if (s == "space-around")  return LayoutAlign::space_around;
        return LayoutAlign::flex_start;
    };

    if (obj.hasObjectMember("justify"))
        l.justify = parse_align(get_string(obj, "justify"));
    if (obj.hasObjectMember("align"))
        l.align = parse_align(get_string(obj, "align"));

    auto parse_sizing = [](const std::string& s) -> SizingMode {
        if (s == "hug" || s == "auto") return SizingMode::hug;
        if (s == "fill")               return SizingMode::fill;
        return SizingMode::fixed;
    };

    if (obj.hasObjectMember("widthMode"))
        l.width_mode = parse_sizing(get_string(obj, "widthMode"));
    if (obj.hasObjectMember("heightMode"))
        l.height_mode = parse_sizing(get_string(obj, "heightMode"));

    return l;
}

static IRNode parse_ir_node(const choc::value::ValueView& obj) {
    IRNode node;
    node.type = get_string(obj, "type", "frame");
    node.name = get_string(obj, "name");
    node.text_content = get_string(obj, "content");

    if (obj.hasObjectMember("style"))
        node.style = parse_ir_style(obj["style"]);
    if (obj.hasObjectMember("layout"))
        node.layout = parse_ir_layout(obj["layout"]);

    // Exact layout dimensions from snapshot_layout (injected by import skill)
    if (obj.hasObjectMember("_layoutHeight"))
        node.attributes["_layoutHeight"] = std::to_string(static_cast<int>(get_float(obj, "_layoutHeight", 0)));
    if (obj.hasObjectMember("_layoutWidth"))
        node.attributes["_layoutWidth"] = std::to_string(static_cast<int>(get_float(obj, "_layoutWidth", 0)));

    // Audio widget detection is deferred until after children are parsed
    // (see below) — containers with child frames shouldn't be widgets

    if (obj.hasObjectMember("label"))
        node.audio_label = get_string(obj, "label");
    if (obj.hasObjectMember("min"))
        node.audio_min = get_float(obj, "min", 0.0f);
    if (obj.hasObjectMember("max"))
        node.audio_max = get_float(obj, "max", 1.0f);
    if (obj.hasObjectMember("default"))
        node.audio_default = get_float(obj, "default", 0.5f);

    // Top-level properties (Pencil/Figma format puts these at node level, not in "style")
    // Override style values if they weren't set from the "style" sub-object
    // Handle Pencil sizing modes: fill_container → flex_grow, fit_content → auto
    if (obj.hasObjectMember("width")) {
        auto w_str = get_string(obj, "width", "");
        if (w_str == "fill_container") {
            node.layout.width_mode = SizingMode::fill;
        } else if (w_str == "fit_content" || w_str.find("fit_content") == 0) {
            node.layout.width_mode = SizingMode::hug;
        } else if (!node.style.width) {
            float w = get_float(obj, "width", 0);
            if (w > 0) node.style.width = w;
        }
    }
    if (obj.hasObjectMember("height")) {
        auto h_str = get_string(obj, "height", "");
        if (h_str == "fill_container") {
            node.layout.height_mode = SizingMode::fill;
        } else if (h_str == "fit_content" || h_str.find("fit_content") == 0) {
            node.layout.height_mode = SizingMode::hug;
        } else if (!node.style.height) {
            float h = get_float(obj, "height", 0);
            if (h > 0) node.style.height = h;
        }
    }
    // Top-level fill → backgroundColor
    if (!node.style.background_color && obj.hasObjectMember("fill")) {
        auto fill_val = obj["fill"];
        if (fill_val.isString()) {
            auto fill_str = std::string(fill_val.toString());
            if (!fill_str.empty() && fill_str[0] == '#')
                node.style.background_color = fill_str;
        }
    }
    // Top-level cornerRadius
    if (!node.style.border_radius && obj.hasObjectMember("cornerRadius")) {
        node.style.border_radius = get_float(obj, "cornerRadius", 0);
    }
    // Top-level fontSize, fontWeight, fontFamily for text nodes
    if (node.type == "text" || node.type == "label") {
        if (!node.style.font_size && obj.hasObjectMember("fontSize"))
            node.style.font_size = get_float(obj, "fontSize", 14);
        if (!node.style.font_weight && obj.hasObjectMember("fontWeight")) {
            auto fw = get_string(obj, "fontWeight", "");
            if (!fw.empty()) {
                try { node.style.font_weight = std::stoi(fw); }
                catch (...) { if (fw == "bold") node.style.font_weight = 700; }
            }
        }
        if (!node.style.color && obj.hasObjectMember("fill")) {
            auto fill_val = obj["fill"];
            if (fill_val.isString()) node.style.color = std::string(fill_val.toString());
        }
        if (!node.style.font_family && obj.hasObjectMember("fontFamily"))
            node.style.font_family = get_string(obj, "fontFamily", "");
        // Top-level content → text_content
        if (node.text_content.empty() && obj.hasObjectMember("content"))
            node.text_content = get_string(obj, "content", "");
    }
    // Top-level layout properties (Pencil uses "layout": "vertical"/"horizontal")
    // Pencil frames default to horizontal when no layout is specified
    if (obj.hasObjectMember("layout")) {
        auto layout_val = obj["layout"];
        if (layout_val.isString()) {
            auto dir = std::string(layout_val.toString());
            if (dir == "horizontal") node.layout.direction = LayoutDirection::row;
            else if (dir == "vertical") node.layout.direction = LayoutDirection::column;
            else if (dir == "none") {} // absolute positioning, keep default
        } else if (layout_val.isObject()) {
            // IR format: layout is an object with "direction" key
            // Already parsed above via parse_ir_layout
        }
    } else if (node.type == "frame" &&
               obj.hasObjectMember("children") && obj["children"].isArray() && obj["children"].size() > 0) {
        // Pencil frames default to horizontal (row) when layout is not specified
        node.layout.direction = LayoutDirection::row;
    }
    if (obj.hasObjectMember("gap"))
        node.layout.gap = get_float(obj, "gap", 0);
    if (obj.hasObjectMember("justifyContent")) {
        auto j = get_string(obj, "justifyContent", "");
        if (j == "center") node.layout.justify = LayoutAlign::center;
        else if (j == "space_between" || j == "space-between") node.layout.justify = LayoutAlign::space_between;
        else if (j == "space_around" || j == "space-around") node.layout.justify = LayoutAlign::space_around;
        else if (j == "end" || j == "flex-end") node.layout.justify = LayoutAlign::flex_end;
    }
    if (obj.hasObjectMember("alignItems")) {
        auto a = get_string(obj, "alignItems", "");
        if (a == "center") node.layout.align = LayoutAlign::center;
        else if (a == "end" || a == "flex-end") node.layout.align = LayoutAlign::flex_end;
    }
    // Top-level padding (Pencil uses array: [top, right] or [top, right, bottom, left])
    if (obj.hasObjectMember("padding")) {
        auto pad = obj["padding"];
        if (pad.isArray()) {
            if (pad.size() == 2) {
                float tb = static_cast<float>(pad[0].getWithDefault<double>(0));
                float lr = static_cast<float>(pad[1].getWithDefault<double>(0));
                node.layout.padding_top = node.layout.padding_bottom = tb;
                node.layout.padding_left = node.layout.padding_right = lr;
            } else if (pad.size() == 4) {
                node.layout.padding_top = static_cast<float>(pad[0].getWithDefault<double>(0));
                node.layout.padding_right = static_cast<float>(pad[1].getWithDefault<double>(0));
                node.layout.padding_bottom = static_cast<float>(pad[2].getWithDefault<double>(0));
                node.layout.padding_left = static_cast<float>(pad[3].getWithDefault<double>(0));
            }
        } else {
            float p = get_float(obj, "padding", 0);
            node.layout.padding_top = node.layout.padding_right =
                node.layout.padding_bottom = node.layout.padding_left = p;
        }
    }

    // Children
    if (obj.hasObjectMember("children") && obj["children"].isArray()) {
        auto children = obj["children"];
        for (uint32_t i = 0; i < children.size(); ++i)
            node.children.push_back(parse_ir_node(children[static_cast<int>(i)]));
    }

    // Audio widget detection (deferred until after children are parsed)
    // Rule: a node is an audio widget ONLY if:
    //   1. Its name matches an audio widget pattern (knob, fader, meter, etc.)
    //   2. AND it doesn't have child frames/groups (containers aren't widgets)
    //   A node with only shape children (ellipse/rectangle) + text is a widget.
    //   A node with child frames (like "KnobRow" containing 4 knob frames) is a container.
    {
        auto detected = detect_audio_widget(node.name);
        if (detected == AudioWidgetType::none && !node.type.empty())
            detected = detect_audio_widget(node.type);

        if (detected != AudioWidgetType::none) {
            // Check if this node has child frames — if so, it's a container, not a widget
            bool has_child_containers = false;
            for (auto& child : node.children) {
                if (child.type == "frame" || child.type == "group" ||
                    !child.children.empty()) {
                    has_child_containers = true;
                    break;
                }
            }
            // Only assign widget type if it's a leaf or has only shapes+text
            if (!has_child_containers)
                node.audio_widget = detected;
        }
    }

    // Parse stroke color from shapes (Pencil puts stroke on ellipse/rectangle nodes)
    if (obj.hasObjectMember("stroke")) {
        auto stroke = obj["stroke"];
        if (stroke.isObject() && stroke.hasObjectMember("fill")) {
            auto fill = stroke["fill"];
            if (fill.isString()) {
                auto fill_str = std::string(fill.toString());
                if (!fill_str.empty() && fill_str[0] == '#')
                    node.attributes["stroke_color"] = fill_str;
            }
        }
    }

    // For audio widgets: extract child shape dimensions into attributes
    // The frame's own width/height is for the column container.
    // The child shape's width/height is for the widget itself.
    if (node.audio_widget != AudioWidgetType::none && !node.children.empty()) {
        for (auto& child : node.children) {
            if (child.type == "ellipse" || child.type == "rectangle") {
                if (child.style.width)
                    node.attributes["shape_width"] = std::to_string(static_cast<int>(*child.style.width));
                if (child.style.height)
                    node.attributes["shape_height"] = std::to_string(static_cast<int>(*child.style.height));
                break;
            }
        }
    }

    return node;
}

static IRTokens parse_ir_tokens(const choc::value::ValueView& obj) {
    IRTokens tokens;
    if (!obj.isObject()) return tokens;

    if (obj.hasObjectMember("colors")) {
        auto c = obj["colors"];
        for (uint32_t i = 0; i < c.size(); ++i) {
            auto m = c.getObjectMemberAt(i);
            tokens.colors[std::string(m.name)] = std::string(m.value.toString());
        }
    }
    if (obj.hasObjectMember("dimensions")) {
        auto d = obj["dimensions"];
        for (uint32_t i = 0; i < d.size(); ++i) {
            auto m = d.getObjectMemberAt(i);
            tokens.dimensions[std::string(m.name)] = static_cast<float>(m.value.getWithDefault<double>(0));
        }
    }
    if (obj.hasObjectMember("strings")) {
        auto s = obj["strings"];
        for (uint32_t i = 0; i < s.size(); ++i) {
            auto m = s.getObjectMemberAt(i);
            tokens.strings[std::string(m.name)] = std::string(m.value.toString());
        }
    }
    return tokens;
}

// ── Source adapters ─────────────────────────────────────────────────────

DesignIR parse_figma_json(const std::string& json) {
    DesignIR ir;
    ir.source = DesignSource::figma;

    auto root = choc::json::parse(json);

    // Figma export can be the IR format directly (from MCP or export tool)
    ir.root = parse_ir_node(root);

    if (root.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(root["tokens"]);

    return ir;
}

DesignIR parse_stitch_html(const std::string& html) {
    DesignIR ir;
    ir.source = DesignSource::stitch;

    // Try parsing as JSON IR first (from Stitch MCP get_screen)
    try {
        auto root = choc::json::parse(html);
        ir.root = parse_ir_node(root);
        if (root.hasObjectMember("tokens"))
            ir.tokens = parse_ir_tokens(root["tokens"]);
        return ir;
    } catch (...) {
        // Not JSON — minimal HTML extraction
    }

    // Basic HTML → IR: extract inline styles from HTML elements
    // For full HTML parsing, the AI pipeline handles translation
    ir.root.type = "frame";
    ir.root.name = "StitchImport";
    ir.root.layout.direction = LayoutDirection::column;

    // Extract text content between tags as a simple fallback
    std::regex tag_re("<([a-z][a-z0-9]*)[^>]*>([^<]*)</\\1>");
    auto begin = std::sregex_iterator(html.begin(), html.end(), tag_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string tag = (*it)[1].str();
        std::string content = (*it)[2].str();
        if (content.empty()) continue;

        IRNode child;
        child.type = "text";
        child.name = tag;
        child.text_content = content;
        ir.root.children.push_back(std::move(child));
    }

    return ir;
}

DesignIR parse_v0_tsx(const std::string& tsx) {
    DesignIR ir;
    ir.source = DesignSource::v0;

    // Try parsing as JSON IR first (pre-processed by AI pipeline)
    try {
        auto root = choc::json::parse(tsx);
        ir.root = parse_ir_node(root);
        if (root.hasObjectMember("tokens"))
            ir.tokens = parse_ir_tokens(root["tokens"]);
        return ir;
    } catch (...) {
        // Not JSON — TSX source
    }

    // TSX → IR: extract JSX elements and Tailwind classes
    // Full translation requires AI; this provides structural extraction
    ir.root.type = "frame";
    ir.root.name = "V0Import";
    ir.root.layout.direction = LayoutDirection::column;

    // Extract className strings for Tailwind analysis
    std::regex class_re("className=\"([^\"]+)\"");
    auto begin = std::sregex_iterator(tsx.begin(), tsx.end(), class_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string classes = (*it)[1].str();
        IRNode child;
        child.type = "frame";
        child.name = classes;  // Store Tailwind classes as name for AI processing
        // Parse common Tailwind patterns
        if (classes.find("flex-row") != std::string::npos)
            child.layout.direction = LayoutDirection::row;
        if (classes.find("flex-col") != std::string::npos)
            child.layout.direction = LayoutDirection::column;
        ir.root.children.push_back(std::move(child));
    }

    return ir;
}

DesignIR parse_pencil_json(const std::string& json) {
    DesignIR ir;
    ir.source = DesignSource::pencil;

    auto root = choc::json::parse(json);

    // Pencil MCP batch_get returns a node tree similar to our IR
    ir.root = parse_ir_node(root);

    if (root.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(root["tokens"]);
    if (root.hasObjectMember("variables"))
        ir.tokens = parse_ir_tokens(root["variables"]);

    return ir;
}

// ── Code generator ──────────────────────────────────────────────────────

static std::string indent(int level, int spaces) {
    return std::string(static_cast<size_t>(level * spaces), ' ');
}

static std::string sanitize_var(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            result += c;
        else if (c == ' ' || c == '-' || c == '.')
            result += '_';
    }
    if (result.empty()) result = "el";
    if (std::isdigit(static_cast<unsigned char>(result[0])))
        result = "_" + result;
    return result;
}

static std::string format_px(float v) {
    if (v == std::floor(v))
        return std::to_string(static_cast<int>(v)) + "px";
    std::ostringstream ss;
    ss << v << "px";
    return ss.str();
}

static const char* audio_widget_type_name(AudioWidgetType t) {
    switch (t) {
        case AudioWidgetType::knob:     return "Knob";
        case AudioWidgetType::fader:    return "Fader";
        case AudioWidgetType::meter:    return "Meter";
        case AudioWidgetType::xy_pad:   return "XYPad";
        case AudioWidgetType::waveform: return "WaveformView";
        case AudioWidgetType::spectrum: return "SpectrumView";
        default: return nullptr;
    }
}

static const char* align_to_css(LayoutAlign a) {
    switch (a) {
        case LayoutAlign::flex_start:    return "flex-start";
        case LayoutAlign::flex_end:      return "flex-end";
        case LayoutAlign::center:        return "center";
        case LayoutAlign::stretch:       return "stretch";
        case LayoutAlign::space_between: return "space-between";
        case LayoutAlign::space_around:  return "space-around";
    }
    return "flex-start";
}

static void generate_node(std::ostringstream& ss, const IRNode& node,
                           const CodeGenOptions& opts, int depth,
                           int& var_counter, const std::string& parent_var) {
    std::string var = sanitize_var(node.name.empty() ? node.type : node.name);
    if (depth > 0) var += std::to_string(var_counter++);
    else var = opts.root_variable;

    std::string ind = indent(depth, opts.indent_spaces);

    // Audio widgets get special treatment
    if (node.audio_widget != AudioWidgetType::none) {
        auto widget_name = audio_widget_type_name(node.audio_widget);
        if (widget_name) {
            if (opts.include_comments && !node.audio_label.empty())
                ss << ind << "// " << node.audio_label << " " << widget_name << "\n";

            ss << ind << "const " << var << " = create" << widget_name << "({\n";
            if (!node.audio_label.empty())
                ss << ind << "  label: '" << node.audio_label << "',\n";
            ss << ind << "  min: " << node.audio_min << ",\n";
            ss << ind << "  max: " << node.audio_max << ",\n";
            ss << ind << "  defaultValue: " << node.audio_default << "\n";
            ss << ind << "});\n";

            if (!parent_var.empty())
                ss << ind << parent_var << ".appendChild(" << var << ");\n";
            ss << "\n";
            return;
        }
    }

    // Determine HTML tag
    std::string tag = "div";
    if (node.type == "text")    tag = "span";
    if (node.type == "button")  tag = "button";
    if (node.type == "input")   tag = "input";
    if (node.type == "image")   tag = "img";
    if (node.type == "label")   tag = "span";

    if (opts.include_comments && !node.name.empty() && depth > 0)
        ss << ind << "// " << node.name << "\n";

    ss << ind << "const " << var << " = document.createElement('" << tag << "');\n";

    // Apply layout styles for container nodes
    if (!node.children.empty() || node.type == "frame") {
        ss << ind << var << ".style.display = 'flex';\n";
        ss << ind << var << ".style.flexDirection = '"
           << (node.layout.direction == LayoutDirection::row ? "row" : "column") << "';\n";

        if (node.layout.gap > 0)
            ss << ind << var << ".style.gap = '" << format_px(node.layout.gap) << "';\n";

        // Padding
        bool uniform_padding = (node.layout.padding_top == node.layout.padding_right &&
                                 node.layout.padding_right == node.layout.padding_bottom &&
                                 node.layout.padding_bottom == node.layout.padding_left);
        if (uniform_padding && node.layout.padding_top > 0) {
            ss << ind << var << ".style.padding = '" << format_px(node.layout.padding_top) << "';\n";
        } else {
            if (node.layout.padding_top > 0)
                ss << ind << var << ".style.paddingTop = '" << format_px(node.layout.padding_top) << "';\n";
            if (node.layout.padding_right > 0)
                ss << ind << var << ".style.paddingRight = '" << format_px(node.layout.padding_right) << "';\n";
            if (node.layout.padding_bottom > 0)
                ss << ind << var << ".style.paddingBottom = '" << format_px(node.layout.padding_bottom) << "';\n";
            if (node.layout.padding_left > 0)
                ss << ind << var << ".style.paddingLeft = '" << format_px(node.layout.padding_left) << "';\n";
        }

        if (node.layout.justify != LayoutAlign::flex_start)
            ss << ind << var << ".style.justifyContent = '" << align_to_css(node.layout.justify) << "';\n";
        if (node.layout.align != LayoutAlign::stretch)
            ss << ind << var << ".style.alignItems = '" << align_to_css(node.layout.align) << "';\n";
        if (node.layout.wrap)
            ss << ind << var << ".style.flexWrap = 'wrap';\n";

        // Sizing
        if (node.layout.width_mode == SizingMode::fill)
            ss << ind << var << ".style.flexGrow = '1';\n";
        if (node.layout.height_mode == SizingMode::fill)
            ss << ind << var << ".style.flexGrow = '1';\n";
    }

    // Apply visual styles
    auto& s = node.style;
    auto emit_str = [&](const char* prop, const std::optional<std::string>& val) {
        if (val) ss << ind << var << ".style." << prop << " = '" << *val << "';\n";
    };
    auto emit_px = [&](const char* prop, const std::optional<float>& val) {
        if (val) ss << ind << var << ".style." << prop << " = '" << format_px(*val) << "';\n";
    };
    auto emit_float = [&](const char* prop, const std::optional<float>& val) {
        if (val) ss << ind << var << ".style." << prop << " = '" << *val << "';\n";
    };

    emit_str("backgroundColor", s.background_color);
    if (s.background_gradient)
        ss << ind << var << ".style.background = '" << *s.background_gradient << "';\n";
    emit_str("color", s.color);
    emit_float("opacity", s.opacity);
    emit_px("borderRadius", s.border_radius);
    emit_str("border", s.border);
    emit_str("boxShadow", s.box_shadow);
    emit_str("filter", s.filter);
    emit_str("fontFamily", s.font_family);
    emit_px("fontSize", s.font_size);
    if (s.font_weight)
        ss << ind << var << ".style.fontWeight = '" << *s.font_weight << "';\n";
    emit_str("fontStyle", s.font_style);
    emit_str("textAlign", s.text_align);
    emit_px("letterSpacing", s.letter_spacing);
    emit_float("lineHeight", s.line_height);
    emit_str("textTransform", s.text_transform);
    emit_str("overflow", s.overflow);
    emit_str("cursor", s.cursor);
    emit_str("position", s.position);
    emit_px("top", s.top);
    emit_px("left", s.left);
    emit_px("right", s.right);
    emit_px("bottom", s.bottom);
    if (s.z_index)
        ss << ind << var << ".style.zIndex = '" << *s.z_index << "';\n";
    emit_str("transform", s.transform);
    emit_px("width", s.width);
    emit_px("height", s.height);
    emit_px("minWidth", s.min_width);
    emit_px("minHeight", s.min_height);
    emit_px("maxWidth", s.max_width);
    emit_px("maxHeight", s.max_height);

    // Text content
    if (!node.text_content.empty())
        ss << ind << var << ".textContent = '" << node.text_content << "';\n";

    // Append to parent
    if (!parent_var.empty())
        ss << ind << parent_var << ".appendChild(" << var << ");\n";

    ss << "\n";

    // Recurse into children
    for (auto& child : node.children)
        generate_node(ss, child, opts, depth + 1, var_counter, var);
}

// ── Native Pulp API code generator ──────────────────────────────────────
// Uses createCol/createRow/createKnob/setFlex — the native widget bridge API.
// Encodes Yoga layout constraints learned from testing:
//   - Every container MUST have explicit height, min_height, or flex_grow
//   - Labels inside containers need min_height (14px text, 12px small)
//   - Faders need min_width >= 40 for thumb rendering
//   - Meters need min_width >= 20 for bar visibility
//   - Knobs need width and height >= 56 for arc rendering

// Minimum sizes for Yoga layout (prevents zero-height collapse)
static constexpr float kMinLabelHeight = 14.0f;
static constexpr float kMinSmallLabelHeight = 12.0f;
static constexpr float kMinRowHeight = 16.0f;
static constexpr float kMinKnobSize = 56.0f;
static constexpr float kMinFaderWidth = 40.0f;
static constexpr float kMinFaderHeight = 80.0f;
static constexpr float kMinMeterWidth = 20.0f;
static constexpr float kMinMeterHeight = 80.0f;

// Compute the actual rendered height of a node from its Pencil/IR data.
// Uses exact child dimensions — no guessing.
static float compute_node_height(const IRNode& node);

static float compute_container_height(const IRNode& node) {
    bool is_row = (node.layout.direction == LayoutDirection::row);
    float gap = node.layout.gap;
    float pad = node.layout.padding_top + node.layout.padding_bottom;

    if (is_row) {
        float max_h = 0;
        for (auto& child : node.children)
            max_h = std::max(max_h, compute_node_height(child));
        return std::max(max_h + pad, kMinRowHeight);
    } else {
        float h = 0;
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (i > 0) h += gap;
            h += compute_node_height(node.children[i]);
        }
        return std::max(h + pad, kMinRowHeight);
    }
}

static float compute_node_height(const IRNode& node) {
    // Audio widget frames: the generated wrapper column contains the widget +
    // separate labels. Compute from children's actual dimensions.
    if (node.audio_widget != AudioWidgetType::none) {
        float widget_h = 0;
        float labels_h = 0;
        float gap = node.layout.gap > 0 ? node.layout.gap : 8;
        int n_items = 0;

        for (auto& c : node.children) {
            if (c.type == "ellipse" || c.type == "rectangle") {
                widget_h = std::max(widget_h, c.style.height.value_or(kMinFaderHeight));
                n_items++;
            } else if (c.type == "text" || c.type == "label") {
                labels_h += kMinLabelHeight;
                n_items++;
            }
        }
        // Minimums if no children found
        if (widget_h == 0) widget_h = (node.audio_widget == AudioWidgetType::knob) ? kMinKnobSize : kMinFaderHeight;
        if (n_items == 0) n_items = 1;

        return widget_h + labels_h + gap * static_cast<float>(n_items);
    }

    // Non-audio node with explicit height
    if (node.style.height) return *node.style.height;

    // Text/label: font-dependent height
    if (node.type == "text" || node.type == "label")
        return node.style.font_size.value_or(14.0f) * 1.4f;

    // Container with children: compute recursively
    if (!node.children.empty())
        return compute_container_height(node);

    // Leaf node without dimensions
    return kMinRowHeight;
}

static void generate_native_node(std::ostringstream& ss, const IRNode& node,
                                  const CodeGenOptions& opts, int depth,
                                  int& var_counter, const std::string& parent_id) {
    std::string id = sanitize_var(node.name.empty() ? node.type : node.name);
    if (depth > 0) id += std::to_string(var_counter++);
    else id = opts.root_variable;

    std::string ind = indent(depth, opts.indent_spaces);
    std::string pid = parent_id.empty() ? "''" : ("'" + parent_id + "'");

    // Audio widgets use native widget API
    if (node.audio_widget != AudioWidgetType::none) {
        auto wtype = node.audio_widget;

        // Extract label text, value text, and stroke color from child nodes
        std::string label_text = node.audio_label;
        std::string value_text;
        std::string stroke_color;  // Per-widget stroke color from child ellipse
        for (auto& child : node.children) {
            if (child.type == "text" || child.type == "label") {
                if (label_text.empty() && !child.text_content.empty())
                    label_text = child.text_content;
                else if (!child.text_content.empty() && child.text_content != label_text)
                    value_text = child.text_content;
            }
            // Extract stroke color from child ellipse/rectangle for per-knob coloring
            if ((child.type == "ellipse" || child.type == "rectangle") && stroke_color.empty()) {
                if (child.attributes.count("stroke_color"))
                    stroke_color = child.attributes.at("stroke_color");
            }
        }

        if (opts.include_comments && !label_text.empty())
            ss << ind << "// " << label_text << "\n";

        // Helper: emit setWidgetStyle if preview mode
        auto emit_style = [&](const std::string& wid) {
            if (opts.preview_mode)
                ss << ind << "setWidgetStyle('" << wid << "', 'minimal');\n";
        };

        // Create a wrapper column for the widget + value label
        std::string col_id = id + "_col";
        ss << ind << "createCol('" << col_id << "', " << pid << ");\n";
        ss << ind << "setFlex('" << col_id << "', 'align_items', 'center');\n";
        ss << ind << "setFlex('" << col_id << "', 'gap', 4);\n";

        if (wtype == AudioWidgetType::knob) {
            // shape_width/height = child ellipse size (widget), frame width = column container
            float shape_w = kMinKnobSize, shape_h = kMinKnobSize;
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            float frame_w = node.style.width.value_or(shape_w + 20);
            float col_h = shape_h + 20 + (value_text.empty() ? 0 : 16);
            ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
            ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            ss << ind << "createKnob('" << id << "', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << shape_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            // Clear built-in label — use separate Yoga-positioned labels for exact placement
            ss << ind << "setLabel('" << id << "', ' ');\n";
            ss << ind << "setValue('" << id << "', " << node.audio_default << ");\n";
            emit_style(id);
            // Per-knob stroke color from child ellipse (used by minimal paint path)
            if (!stroke_color.empty())
                ss << ind << "setBorder('" << id << "', '" << stroke_color << "', 2.5, " << (shape_w * 0.5f) << ");\n";
            // Labels below knob — center-aligned, Yoga-positioned
            if (!label_text.empty()) {
                std::string lbl_id = id + "_lbl";
                ss << ind << "createLabel('" << lbl_id << "', '" << label_text << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << lbl_id << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << lbl_id << "', 11);\n";
                ss << ind << "setTextColor('" << lbl_id << "', '#a6adc8');\n";
                ss << ind << "setTextAlign('" << lbl_id << "', 'center');\n";
            }
            if (!value_text.empty()) {
                std::string val_id = id + "_val";
                ss << ind << "createLabel('" << val_id << "', '" << value_text << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << val_id << "', 'height', " << kMinSmallLabelHeight << ");\n";
                ss << ind << "setFontSize('" << val_id << "', 10);\n";
                ss << ind << "setTextColor('" << val_id << "', '#6c7086');\n";
                ss << ind << "setTextAlign('" << val_id << "', 'center');\n";
            }
        }
        else if (wtype == AudioWidgetType::fader) {
            // shape_width/height from child rectangle, frame width for column
            float frame_w = node.style.width.value_or(kMinFaderWidth);
            float shape_w = frame_w;
            float shape_h = kMinFaderHeight;
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            // Use frame width for column, but ensure widget is at least usable
            float widget_w = std::max(shape_w, 6.0f);
            float col_h = shape_h + 20;
            ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
            ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            ss << ind << "createFader('" << id << "', 'vertical', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << widget_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            // Fader label overlaps track when rendered inside bounds — use separate label
            ss << ind << "setLabel('" << id << "', ' ');\n";  // Clear built-in label
            ss << ind << "setValue('" << id << "', " << node.audio_default << ");\n";
            emit_style(id);
            if (!label_text.empty()) {
                std::string lbl_id = id + "_lbl";
                ss << ind << "createLabel('" << lbl_id << "', '" << label_text << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << lbl_id << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << lbl_id << "', 11);\n";
                ss << ind << "setTextColor('" << lbl_id << "', '#a6adc8');\n";
                ss << ind << "setTextAlign('" << lbl_id << "', 'center');\n";
            }
        }
        else if (wtype == AudioWidgetType::meter) {
            float frame_w = node.style.width.value_or(kMinMeterWidth);
            float shape_w = frame_w;
            float shape_h = kMinMeterHeight;
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            float widget_w = std::max(shape_w, 8.0f);
            float col_h = shape_h + 20;
            ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
            ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            ss << ind << "createMeter('" << id << "', 'vertical', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << widget_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            ss << ind << "setMeterLevel('" << id << "', -6);\n";
            emit_style(id);
            // Meter has no setLabel — always add a separate label
            if (!label_text.empty()) {
                std::string lbl_id = id + "_lbl";
                ss << ind << "createLabel('" << lbl_id << "', '" << label_text << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << lbl_id << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << lbl_id << "', 11);\n";
                ss << ind << "setTextColor('" << lbl_id << "', '#a6adc8');\n";
                ss << ind << "setTextAlign('" << lbl_id << "', 'center');\n";
            }
        }
        else if (wtype == AudioWidgetType::xy_pad) {
            float sz = std::max(node.style.width.value_or(100.0f), 80.0f);
            ss << ind << "setFlex('" << col_id << "', 'height', " << (sz + 20) << ");\n";
            ss << ind << "createXYPad('" << id << "', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << sz << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << sz << ");\n";
        }
        else if (wtype == AudioWidgetType::waveform || wtype == AudioWidgetType::spectrum) {
            float w = node.style.width.value_or(200.0f);
            float h = node.style.height.value_or(80.0f);
            ss << ind << "setFlex('" << col_id << "', 'height', " << (h + 20) << ");\n";
            auto fn = (wtype == AudioWidgetType::waveform) ? "createWaveform" : "createSpectrum";
            ss << ind << fn << "('" << id << "', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << h << ");\n";
        }

        ss << "\n";
        return;
    }

    // Container or text node
    bool is_container = !node.children.empty() || node.type == "frame";
    bool is_text = (node.type == "text" || node.type == "label");
    bool is_row = (node.layout.direction == LayoutDirection::row);

    if (is_text) {
        // Text node → createLabel with explicit height (Yoga requirement)
        ss << ind << "createLabel('" << id << "', '" << node.text_content << "', " << pid << ");\n";

        float font_h = node.style.font_size.value_or(14.0f);
        float label_h = std::max(font_h * 1.4f, kMinLabelHeight);
        ss << ind << "setFlex('" << id << "', 'height', " << label_h << ");\n";

        if (node.style.font_size)
            ss << ind << "setFontSize('" << id << "', " << *node.style.font_size << ");\n";
        if (node.style.font_weight)
            ss << ind << "setFontWeight('" << id << "', '" << *node.style.font_weight << "');\n";
        if (node.style.color)
            ss << ind << "setTextColor('" << id << "', '" << *node.style.color << "');\n";

        ss << "\n";
        return;
    }

    if (is_container) {
        // Container → createRow or createCol
        if (opts.include_comments && !node.name.empty() && depth > 0)
            ss << ind << "// " << node.name << "\n";

        ss << ind << (is_row ? "createRow" : "createCol")
           << "('" << id << "', " << pid << ");\n";

        // Yoga: every container MUST have explicit height
        // Priority: _layoutHeight (from snapshot_layout) > style.height > fill > computed
        if (node.attributes.count("_layoutHeight")) {
            int lh = std::stoi(node.attributes.at("_layoutHeight"));
            if (lh > 0) ss << ind << "setFlex('" << id << "', 'height', " << lh << ");\n";
        } else if (node.style.height) {
            ss << ind << "setFlex('" << id << "', 'height', " << *node.style.height << ");\n";
        } else if (node.layout.height_mode == SizingMode::fill) {
            ss << ind << "setFlex('" << id << "', 'flex_grow', 1);\n";
        } else {
            float est = compute_container_height(node);
            ss << ind << "setFlex('" << id << "', 'height', " << est << ");\n";
        }

        // Width: use _layoutWidth if available, then style.width
        if (node.attributes.count("_layoutWidth")) {
            int lw = std::stoi(node.attributes.at("_layoutWidth"));
            if (lw > 0) ss << ind << "setFlex('" << id << "', 'width', " << lw << ");\n";
        } else if (node.style.width) {
            ss << ind << "setFlex('" << id << "', 'width', " << *node.style.width << ");\n";
        }

        if (node.layout.gap > 0)
            ss << ind << "setFlex('" << id << "', 'gap', " << node.layout.gap << ");\n";

        // Padding
        bool uniform = (node.layout.padding_top == node.layout.padding_right &&
                         node.layout.padding_right == node.layout.padding_bottom &&
                         node.layout.padding_bottom == node.layout.padding_left);
        if (uniform && node.layout.padding_top > 0)
            ss << ind << "setFlex('" << id << "', 'padding', " << node.layout.padding_top << ");\n";
        else {
            if (node.layout.padding_top > 0)
                ss << ind << "setFlex('" << id << "', 'padding_top', " << node.layout.padding_top << ");\n";
            if (node.layout.padding_right > 0)
                ss << ind << "setFlex('" << id << "', 'padding_right', " << node.layout.padding_right << ");\n";
            if (node.layout.padding_bottom > 0)
                ss << ind << "setFlex('" << id << "', 'padding_bottom', " << node.layout.padding_bottom << ");\n";
            if (node.layout.padding_left > 0)
                ss << ind << "setFlex('" << id << "', 'padding_left', " << node.layout.padding_left << ");\n";
        }

        if (node.layout.justify != LayoutAlign::flex_start)
            ss << ind << "setFlex('" << id << "', 'justify_content', '" << align_to_css(node.layout.justify) << "');\n";
        if (node.layout.align != LayoutAlign::stretch)
            ss << ind << "setFlex('" << id << "', 'align_items', '" << align_to_css(node.layout.align) << "');\n";

        // Visual styles
        if (node.style.background_color)
            ss << ind << "setBackground('" << id << "', '" << *node.style.background_color << "');\n";
        if (node.style.border_radius)
            ss << ind << "setCornerRadius('" << id << "', 'All', " << *node.style.border_radius << ");\n";

        ss << "\n";

        // Recurse children
        // For space_between rows, right-align the last text child
        bool is_space_between = is_row && node.layout.justify == LayoutAlign::space_between;
        std::string last_text_child_id;
        for (auto& child : node.children) {
            // Capture the var_counter before generation to reconstruct this child's ID
            if (is_space_between && (child.type == "text" || child.type == "label")) {
                std::string child_id = sanitize_var(child.name.empty() ? child.type : child.name);
                child_id += std::to_string(var_counter);  // counter value before this child runs
                last_text_child_id = child_id;
            }
            generate_native_node(ss, child, opts, depth + 1, var_counter, id);
        }
        // Right-align the last label in space_between rows
        if (!last_text_child_id.empty()) {
            std::string child_ind = indent(depth + 1, opts.indent_spaces);
            ss << child_ind << "setTextAlign('" << last_text_child_id << "', 'right');\n\n";
        }

        return;
    }

    // Generic frame without children (divider, spacer, etc.)
    ss << ind << "createRow('" << id << "', " << pid << ");\n";
    if (node.style.height)
        ss << ind << "setFlex('" << id << "', 'height', " << *node.style.height << ");\n";
    else
        ss << ind << "setFlex('" << id << "', 'height', 1);\n";
    if (node.style.background_color)
        ss << ind << "setBackground('" << id << "', '" << *node.style.background_color << "');\n";
    ss << "\n";
}

// ── Public code generation ──────────────────────────────────────────────

std::string generate_pulp_js(const DesignIR& ir, const CodeGenOptions& opts) {
    std::ostringstream ss;

    if (opts.include_comments) {
        ss << "// Generated by Pulp import-design from " << design_source_name(ir.source) << "\n";
        if (!ir.source_file.empty())
            ss << "// Source: " << ir.source_file << "\n";
        ss << "\n";
    }

    ss << "setTheme('dark');\n\n";

    // Token assignments
    if (opts.include_tokens && (!ir.tokens.colors.empty() ||
                                 !ir.tokens.dimensions.empty() ||
                                 !ir.tokens.strings.empty())) {
        if (opts.include_comments)
            ss << "// Design tokens\n";

        if (opts.mode == CodeGenMode::native) {
            for (auto& [name, value] : ir.tokens.colors)
                ss << "setColorToken('" << name << "', '" << value << "');\n";
            for (auto& [name, value] : ir.tokens.dimensions)
                ss << "setDimensionToken('" << name << "', " << value << ");\n";
        } else {
            for (auto& [name, value] : ir.tokens.colors)
                ss << "theme.colors[\"" << name << "\"] = '" << value << "';\n";
            for (auto& [name, value] : ir.tokens.dimensions)
                ss << "theme.dimensions[\"" << name << "\"] = " << value << ";\n";
            for (auto& [name, value] : ir.tokens.strings)
                ss << "theme.strings[\"" << name << "\"] = '" << value << "';\n";
        }

        ss << "\n";
    }

    if (opts.mode == CodeGenMode::native) {
        // Native Pulp API
        int var_counter = 0;
        generate_native_node(ss, ir.root, opts, 0, var_counter, "");
        ss << "void 0;\n";
    } else {
        // Web-compat DOM API
        int var_counter = 0;
        generate_node(ss, ir.root, opts, 0, var_counter, "");
        ss << "document.body.appendChild(" << opts.root_variable << ");\n";
    }

    return ss.str();
}

// ── W3C Design Tokens ───────────────────────────────────────────────────

static Color parse_hex_color_str(const std::string& hex) {
    if (hex.empty() || hex[0] != '#') return {};
    try {
        auto val = std::stoul(hex.substr(1), nullptr, 16);
        if (hex.size() == 7)
            return color_from_hex(static_cast<uint32_t>(val));
        if (hex.size() == 9)
            return color_from_hex_alpha(static_cast<uint32_t>(val));
    } catch (...) {}
    return {};
}

Theme parse_w3c_tokens(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // W3C/DTCG Design Tokens Format:
    // - Top-level groups with $type (inherited by children)
    // - Nested tokens with $value/$type
    // - Alias references: { "$value": "{color.primary}" } resolve to other tokens
    // - $description stored but not used in Theme (available for documentation)

    // Evaluate simple math expressions: "8 * 2" → "16", "{spacing.base} * 2" after alias resolution
    auto eval_math = [](const std::string& expr) -> std::string {
        // Only handle simple "number op number" patterns
        auto trim = [](const std::string& s) -> std::string {
            auto a = s.find_first_not_of(" \t");
            auto b = s.find_last_not_of(" \t");
            return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        std::string s = trim(expr);
        // Strip trailing units (px, rem, em)
        std::string unit;
        for (auto& u : {"px", "rem", "em"}) {
            if (s.size() > 2 && s.substr(s.size() - std::strlen(u)) == u) {
                unit = u;
                s = trim(s.substr(0, s.size() - std::strlen(u)));
                break;
            }
        }
        // Find operator
        for (char op : {'*', '+', '-', '/'}) {
            auto pos = s.find(op);
            if (pos != std::string::npos && pos > 0 && pos < s.size() - 1) {
                try {
                    float a = std::stof(trim(s.substr(0, pos)));
                    float b = std::stof(trim(s.substr(pos + 1)));
                    float result = 0;
                    if (op == '*') result = a * b;
                    else if (op == '+') result = a + b;
                    else if (op == '-') result = a - b;
                    else if (op == '/' && b != 0) result = a / b;
                    // Return as clean number string
                    if (result == std::floor(result))
                        return std::to_string(static_cast<int>(result));
                    std::ostringstream oss;
                    oss << result;
                    return oss.str();
                } catch (...) {}
            }
        }
        return expr;  // Not a math expression, return as-is
    };

    // First pass: collect all raw token values (including unresolved aliases)
    struct RawToken { std::string type; std::string value; };
    std::unordered_map<std::string, RawToken> raw_tokens;

    std::function<void(const choc::value::ValueView&, const std::string&, const std::string&)> walk;
    walk = [&](const choc::value::ValueView& obj, const std::string& prefix,
               const std::string& inherited_type) {
        if (!obj.isObject()) return;

        // Group-level $type applies to all children without their own $type
        auto group_type = get_string(obj, "$type", "");
        if (group_type.empty()) group_type = inherited_type;

        for (uint32_t i = 0; i < obj.size(); ++i) {
            auto member = obj.getObjectMemberAt(i);
            std::string key(member.name);
            if (key.empty() || key[0] == '$') continue;

            auto& val = member.value;
            std::string full_name = prefix.empty() ? key : prefix + "." + key;

            // Leaf token: has $value
            if (val.isObject() && val.hasObjectMember("$value")) {
                auto type_str = get_string(val, "$type", "");
                if (type_str.empty()) type_str = group_type;

                auto dollar_val = val["$value"];

                // Composite tokens: typography, shadow, border have object $value
                if (dollar_val.isObject()) {
                    if (type_str == "typography") {
                        // Flatten: typography.fontFamily, typography.fontSize, etc.
                        if (dollar_val.hasObjectMember("fontFamily"))
                            raw_tokens[full_name + ".fontFamily"] = { "fontFamily", std::string(dollar_val["fontFamily"].toString()) };
                        if (dollar_val.hasObjectMember("fontSize"))
                            raw_tokens[full_name + ".fontSize"] = { "dimension", std::string(dollar_val["fontSize"].toString()) };
                        if (dollar_val.hasObjectMember("fontWeight"))
                            raw_tokens[full_name + ".fontWeight"] = { "number", std::string(dollar_val["fontWeight"].toString()) };
                        if (dollar_val.hasObjectMember("lineHeight"))
                            raw_tokens[full_name + ".lineHeight"] = { "number", std::string(dollar_val["lineHeight"].toString()) };
                        if (dollar_val.hasObjectMember("letterSpacing"))
                            raw_tokens[full_name + ".letterSpacing"] = { "dimension", std::string(dollar_val["letterSpacing"].toString()) };
                    } else if (type_str == "shadow") {
                        // Flatten shadow: offsetX, offsetY, blur, spread, color
                        if (dollar_val.hasObjectMember("color"))
                            raw_tokens[full_name + ".color"] = { "color", std::string(dollar_val["color"].toString()) };
                        if (dollar_val.hasObjectMember("offsetX"))
                            raw_tokens[full_name + ".offsetX"] = { "dimension", std::string(dollar_val["offsetX"].toString()) };
                        if (dollar_val.hasObjectMember("offsetY"))
                            raw_tokens[full_name + ".offsetY"] = { "dimension", std::string(dollar_val["offsetY"].toString()) };
                        if (dollar_val.hasObjectMember("blur"))
                            raw_tokens[full_name + ".blur"] = { "dimension", std::string(dollar_val["blur"].toString()) };
                        if (dollar_val.hasObjectMember("spread"))
                            raw_tokens[full_name + ".spread"] = { "dimension", std::string(dollar_val["spread"].toString()) };
                    } else if (type_str == "border") {
                        if (dollar_val.hasObjectMember("color"))
                            raw_tokens[full_name + ".color"] = { "color", std::string(dollar_val["color"].toString()) };
                        if (dollar_val.hasObjectMember("width"))
                            raw_tokens[full_name + ".width"] = { "dimension", std::string(dollar_val["width"].toString()) };
                        if (dollar_val.hasObjectMember("style"))
                            raw_tokens[full_name + ".style"] = { "string", std::string(dollar_val["style"].toString()) };
                    } else {
                        // Unknown composite — store as string
                        raw_tokens[full_name] = { type_str, choc::json::toString(dollar_val) };
                    }
                } else {
                    // Simple scalar $value
                    auto value_str = std::string(dollar_val.toString());
                    raw_tokens[full_name] = { type_str, value_str };
                }
            } else if (val.isObject()) {
                // Nested group — recurse with inherited type
                walk(val, full_name, group_type);
            }
        }
    };

    walk(root, "", "");

    // Second pass: resolve aliases (values like "{color.primary}" or "{spacing.base} * 2")
    // Handles both whole-value aliases and partial aliases embedded in expressions
    // Supports up to 10 levels of chained aliases to prevent infinite loops
    auto resolve_value = [&](const std::string& value) -> std::string {
        std::string resolved = value;
        std::unordered_set<std::string> visited;  // Cycle detection
        for (int depth = 0; depth < 10; ++depth) {
            // Find any {reference.path} pattern in the string
            auto open = resolved.find('{');
            auto close = resolved.find('}');
            if (open == std::string::npos || close == std::string::npos || close <= open)
                break;

            auto ref = resolved.substr(open + 1, close - open - 1);
            if (visited.count(ref))
                break;  // Circular alias — stop resolving
            visited.insert(ref);

            auto it = raw_tokens.find(ref);
            if (it != raw_tokens.end()) {
                // Replace {ref} with resolved value
                resolved = resolved.substr(0, open) + it->second.value + resolved.substr(close + 1);
                continue;  // May contain more aliases
            }
            break;  // Reference not found
        }
        return resolved;
    };

    // Third pass: resolve aliases, evaluate math, store into Theme
    for (auto& [name, token] : raw_tokens) {
        auto value_str = eval_math(resolve_value(token.value));
        auto& type_str = token.type;

        if (type_str == "color") {
            if (!value_str.empty() && value_str[0] == '#') {
                theme.colors[name] = parse_hex_color_str(value_str);
            }
        } else if (type_str == "dimension") {
            float v = 0;
            try { v = std::stof(value_str); } catch (...) {}
            theme.dimensions[name] = v;
        } else if (type_str == "fontFamily" || type_str == "string") {
            theme.strings[name] = value_str;
        } else if (type_str == "number") {
            float v = 0;
            try { v = std::stof(value_str); } catch (...) {}
            theme.dimensions[name] = v;
        } else {
            // Infer type from resolved value
            if (!value_str.empty() && value_str[0] == '#') {
                theme.colors[name] = parse_hex_color_str(value_str);
            } else {
                try {
                    float v = std::stof(value_str);
                    theme.dimensions[name] = v;
                } catch (...) {
                    theme.strings[name] = value_str;
                }
            }
        }
    }

    return theme;
}

std::string export_w3c_tokens(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n";

    // Group tokens by prefix (before the dot)
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> color_groups;
    for (auto& [name, color] : theme.colors) {
        auto dot = name.find('.');
        std::string group = dot != std::string::npos ? name.substr(0, dot) : "color";
        std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;

        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());

        color_groups[group].emplace_back(key, buf);
    }

    std::map<std::string, std::vector<std::pair<std::string, float>>> dim_groups;
    for (auto& [name, value] : theme.dimensions) {
        auto dot = name.find('.');
        std::string group = dot != std::string::npos ? name.substr(0, dot) : "dimension";
        std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;
        dim_groups[group].emplace_back(key, value);
    }

    bool first_group = true;

    // Colors
    for (auto& [group, entries] : color_groups) {
        if (!first_group) ss << ",\n";
        first_group = false;
        ss << "  \"" << group << "\": {\n";
        bool first = true;
        for (auto& [key, hex] : entries) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    \"" << key << "\": { \"$value\": \"" << hex << "\", \"$type\": \"color\" }";
        }
        ss << "\n  }";
    }

    // Dimensions
    for (auto& [group, entries] : dim_groups) {
        if (!first_group) ss << ",\n";
        first_group = false;
        ss << "  \"" << group << "\": {\n";
        bool first = true;
        for (auto& [key, val] : entries) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    \"" << key << "\": { \"$value\": \"" << val << "\", \"$type\": \"dimension\" }";
        }
        ss << "\n  }";
    }

    // Strings
    if (!theme.strings.empty()) {
        if (!first_group) ss << ",\n";
        ss << "  \"string\": {\n";
        bool first = true;
        for (auto& [name, value] : theme.strings) {
            if (!first) ss << ",\n";
            first = false;
            auto dot = name.find('.');
            std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;
            ss << "    \"" << key << "\": { \"$value\": \"" << value << "\", \"$type\": \"string\" }";
        }
        ss << "\n  }";
    }

    ss << "\n}\n";
    return ss.str();
}

Theme ir_tokens_to_theme(const IRTokens& tokens) {
    Theme theme;
    for (auto& [name, hex] : tokens.colors)
        theme.colors[name] = parse_hex_color_str(hex);
    theme.dimensions = tokens.dimensions;
    theme.strings = tokens.strings;
    return theme;
}

IRTokens theme_to_ir_tokens(const Theme& theme) {
    IRTokens tokens;
    for (auto& [name, color] : theme.colors) {
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        tokens.colors[name] = buf;
    }
    tokens.dimensions = theme.dimensions;
    tokens.strings = theme.strings;
    return tokens;
}

// ── Figma Variables sync ────────────────────────────────────────────────

Theme parse_figma_variables(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // Figma Variables JSON structure (from MCP get_variable_defs):
    // { "variables": [ { "name": "color/primary", "resolvedValue": "#89B4FA",
    //                     "type": "COLOR" }, ... ],
    //   "collections": [ { "name": "Tokens", "modes": [...] } ] }
    // OR flat array of variables

    auto parse_vars = [&](const choc::value::ValueView& vars) {
        for (uint32_t i = 0; i < vars.size(); ++i) {
            auto v = vars[static_cast<int>(i)];
            auto name = get_string(v, "name", "");
            auto type = get_string(v, "type", "");
            if (name.empty()) continue;

            // Figma uses slash-separated paths: "color/primary" → "color.primary"
            std::string dotted = name;
            for (auto& c : dotted) if (c == '/') c = '.';

            auto resolved = get_string(v, "resolvedValue", "");
            if (resolved.empty() && v.hasObjectMember("value"))
                resolved = get_string(v, "value", "");

            if (type == "COLOR" || type == "color") {
                if (!resolved.empty() && resolved[0] == '#')
                    theme.colors[dotted] = parse_hex_color_str(resolved);
            } else if (type == "FLOAT" || type == "float" || type == "number") {
                try { theme.dimensions[dotted] = std::stof(resolved); } catch (...) {}
            } else if (type == "STRING" || type == "string") {
                theme.strings[dotted] = resolved;
            } else {
                // Infer from value
                if (!resolved.empty() && resolved[0] == '#')
                    theme.colors[dotted] = parse_hex_color_str(resolved);
                else {
                    try { theme.dimensions[dotted] = std::stof(resolved); }
                    catch (...) { theme.strings[dotted] = resolved; }
                }
            }
        }
    };

    if (root.isObject() && root.hasObjectMember("variables") && root["variables"].isArray())
        parse_vars(root["variables"]);
    else if (root.isArray())
        parse_vars(root);

    return theme;
}

std::string export_figma_variables(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n  \"variables\": [\n";

    bool first = true;
    auto emit = [&](const std::string& name, const std::string& type, const std::string& value) {
        if (!first) ss << ",\n";
        first = false;
        // Convert dot-separated to slash-separated for Figma
        std::string figma_name = name;
        for (auto& c : figma_name) if (c == '.') c = '/';
        ss << "    { \"name\": \"" << figma_name << "\", \"type\": \"" << type
           << "\", \"value\": \"" << value << "\" }";
    };

    for (auto& [name, color] : theme.colors) {
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        emit(name, "COLOR", buf);
    }
    for (auto& [name, value] : theme.dimensions) {
        std::ostringstream vs;
        if (value == std::floor(value)) vs << static_cast<int>(value);
        else vs << value;
        emit(name, "FLOAT", vs.str());
    }
    for (auto& [name, value] : theme.strings)
        emit(name, "STRING", value);

    ss << "\n  ]\n}\n";
    return ss.str();
}

// ── Stitch Design System sync ──────────────────────────────────────────

Theme parse_stitch_design_system(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // Stitch Design System JSON (from MCP list_design_systems):
    // { "name": "My Theme",
    //   "colors": { "primary": "#89B4FA", "background": "#1E1E2E", ... },
    //   "fonts": { "heading": "Inter", "body": "Roboto" },
    //   "roundness": "medium",
    //   "spacing": 8 }

    if (root.hasObjectMember("colors")) {
        auto colors = root["colors"];
        for (uint32_t i = 0; i < colors.size(); ++i) {
            auto m = colors.getObjectMemberAt(i);
            auto hex = std::string(m.value.toString());
            if (!hex.empty() && hex[0] == '#')
                theme.colors[std::string("color.") + std::string(m.name)] = parse_hex_color_str(hex);
        }
    }

    if (root.hasObjectMember("fonts")) {
        auto fonts = root["fonts"];
        for (uint32_t i = 0; i < fonts.size(); ++i) {
            auto m = fonts.getObjectMemberAt(i);
            theme.strings[std::string("font.") + std::string(m.name)] = std::string(m.value.toString());
        }
    }

    if (root.hasObjectMember("roundness")) {
        auto r = get_string(root, "roundness", "medium");
        float radius = 8.0f;
        if (r == "none") radius = 0;
        else if (r == "small") radius = 4;
        else if (r == "medium") radius = 8;
        else if (r == "large") radius = 16;
        else if (r == "full") radius = 999;
        else { try { radius = std::stof(r); } catch (...) {} }
        theme.dimensions["roundness"] = radius;
    }

    if (root.hasObjectMember("spacing")) {
        theme.dimensions["spacing.base"] = get_float(root, "spacing", 8);
    }

    return theme;
}

std::string export_stitch_design_system(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n";

    // Colors
    ss << "  \"colors\": {\n";
    bool first = true;
    for (auto& [name, color] : theme.colors) {
        if (!first) ss << ",\n";
        first = false;
        // Strip "color." prefix for Stitch
        auto key = name;
        if (key.substr(0, 6) == "color.") key = key.substr(6);
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        ss << "    \"" << key << "\": \"" << buf << "\"";
    }
    ss << "\n  },\n";

    // Fonts
    ss << "  \"fonts\": {\n";
    first = true;
    for (auto& [name, value] : theme.strings) {
        if (name.find("font.") != 0) continue;
        if (!first) ss << ",\n";
        first = false;
        auto key = name.substr(5);
        ss << "    \"" << key << "\": \"" << value << "\"";
    }
    ss << "\n  },\n";

    // Roundness
    float roundness = 8;
    if (theme.dimensions.count("roundness"))
        roundness = theme.dimensions.at("roundness");
    std::string r_name = "medium";
    if (roundness <= 0) r_name = "none";
    else if (roundness <= 4) r_name = "small";
    else if (roundness <= 8) r_name = "medium";
    else if (roundness <= 16) r_name = "large";
    else r_name = "full";
    ss << "  \"roundness\": \"" << r_name << "\",\n";

    // Spacing
    float spacing = 8;
    if (theme.dimensions.count("spacing.base"))
        spacing = theme.dimensions.at("spacing.base");
    ss << "  \"spacing\": " << static_cast<int>(spacing) << "\n";

    ss << "}\n";
    return ss.str();
}

} // namespace pulp::view
