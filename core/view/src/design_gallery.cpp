// design_gallery.cpp — design-system gallery card model + review-artifact emitters.

#include <pulp/design/design_gallery.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>

namespace pulp::design {

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// A line is a magic-comment line if, once leading whitespace is stripped, it
// begins with a `//`, `/*`, or `*` (continuation of a block comment). Tags are
// honored only on such lines so a `"@dsCard"` inside a string literal or prose
// cannot falsely opt a file into the gallery.
bool is_comment_line(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && is_ws(line[i])) ++i;
    if (i < line.size() && line[i] == '*') return true;
    return i + 1 < line.size() && line[i] == '/' && (line[i + 1] == '/' || line[i + 1] == '*');
}

// The line containing byte offset `pos` within `head`.
std::string_view line_at(std::string_view head, size_t pos) {
    auto begin = head.rfind('\n', pos);
    begin = (begin == std::string_view::npos) ? 0 : begin + 1;
    auto end = head.find('\n', pos);
    return head.substr(begin, (end == std::string_view::npos ? head.size() : end) - begin);
}

// The value of a whitespace-delimited `key=value` token within one line, or ""
// when the key is absent. The key must sit on a word boundary (start of line or
// after whitespace) so `notviewport=` does not match `viewport=`. The value runs
// to the next whitespace.
std::string_view value_after(std::string_view line, std::string_view key) {
    for (size_t p = line.find(key); p != std::string_view::npos; p = line.find(key, p + 1)) {
        if (p != 0 && !is_ws(line[p - 1])) continue;  // not a word boundary
        size_t v = p + key.size();
        size_t end = v;
        while (end < line.size() && !is_ws(line[end])) ++end;
        return line.substr(v, end - v);
    }
    return {};
}

// Parse a positive int from a numeric span; nullopt on any non-numeric input or
// a non-positive result.
std::optional<int> parse_positive(std::string_view s) {
    int v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || v <= 0) return std::nullopt;
    return v;
}

std::string html_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace

std::optional<GalleryCard> parse_gallery_card(std::string_view file_head,
                                              std::string_view path) {
    auto tag = file_head.find("@dsCard");
    if (tag == std::string_view::npos) return std::nullopt;

    // The tag is honored only as a magic comment; group= and viewport= belong to
    // its line, so a `viewport=` token in unrelated code below cannot leak in.
    std::string_view card_line = line_at(file_head, tag);
    if (!is_comment_line(card_line)) return std::nullopt;

    auto viewport = value_after(card_line, "viewport=");
    auto xsep = viewport.find('x');
    if (xsep == std::string_view::npos) return std::nullopt;  // viewport required
    auto w = parse_positive(viewport.substr(0, xsep));
    auto h = parse_positive(viewport.substr(xsep + 1));
    if (!w || !h) return std::nullopt;

    GalleryCard card;
    card.file = std::string(path);
    auto group = value_after(card_line, "group=");
    card.group = group.empty() ? std::string(kGalleryUngrouped) : std::string(group);
    card.width = *w;
    card.height = *h;
    // @startingPoint may sit on its own comment line in the head, but only counts
    // when it, too, is a magic comment — not a mention in a string or prose.
    if (auto sp = file_head.find("@startingPoint"); sp != std::string_view::npos)
        card.starting_point = is_comment_line(line_at(file_head, sp));
    return card;
}

std::string gallery_content_hash(std::string_view bytes) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a 64-bit offset basis
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ull;  // FNV prime
    }
    static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[i] = kHex[h & 0xF];
        h >>= 4;
    }
    return out;
}

void sort_cards(std::vector<GalleryCard>& cards) {
    std::stable_sort(cards.begin(), cards.end(), [](const GalleryCard& a, const GalleryCard& b) {
        if (a.group != b.group) return a.group < b.group;
        return a.file < b.file;
    });
}

std::string gallery_manifest_json(
    const std::vector<GalleryCard>& cards,
    const std::function<std::string(const GalleryCard&)>& png_rel) {
    std::vector<GalleryCard> sorted = cards;
    sort_cards(sorted);

    auto root = choc::value::createObject("");
    root.addMember("format_version", std::string(kGalleryFormatVersion));
    root.addMember("total", static_cast<int64_t>(sorted.size()));

    auto groups = choc::value::createEmptyArray();
    size_t i = 0;
    while (i < sorted.size()) {
        const std::string& gname = sorted[i].group;
        auto group_obj = choc::value::createObject("");
        group_obj.addMember("name", gname);

        auto card_arr = choc::value::createEmptyArray();
        int count = 0;
        while (i < sorted.size() && sorted[i].group == gname) {
            const GalleryCard& c = sorted[i];
            auto co = choc::value::createObject("");
            co.addMember("file", c.file);
            co.addMember("viewport", std::to_string(c.width) + "x" + std::to_string(c.height));
            co.addMember("width", static_cast<int64_t>(c.width));
            co.addMember("height", static_cast<int64_t>(c.height));
            co.addMember("starting_point", c.starting_point);
            co.addMember("content_hash", c.content_hash);
            co.addMember("png", png_rel ? png_rel(c) : std::string{});
            card_arr.addArrayElement(co);
            ++count;
            ++i;
        }
        group_obj.addMember("count", static_cast<int64_t>(count));
        group_obj.addMember("cards", card_arr);
        groups.addArrayElement(group_obj);
    }
    root.addMember("groups", groups);
    return choc::json::toString(root, /*pretty=*/true);
}

std::string gallery_html(
    const std::vector<GalleryCard>& cards,
    const std::function<std::string(const GalleryCard&)>& png_rel) {
    std::vector<GalleryCard> sorted = cards;
    sort_cards(sorted);

    std::string out;
    out += "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
    out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out += "<title>Design gallery</title>\n<style>\n";
    out += "  :root { color-scheme: light dark; }\n";
    out += "  body { font: 13px/1.4 system-ui, sans-serif; margin: 24px; }\n";
    out += "  h2 { margin: 28px 0 8px; font-size: 15px; }\n";
    out += "  .grid { display: flex; flex-wrap: wrap; gap: 16px; }\n";
    out += "  figure { margin: 0; }\n";
    out += "  figure img { display: block; border: 1px solid #8884; background: #8881; }\n";
    out += "  .miss { display: flex; align-items: center; justify-content: center;\n";
    out += "          border: 1px dashed #8886; color: #888; font-size: 11px; }\n";
    out += "  figcaption { margin-top: 4px; font-size: 11px; color: #888;\n";
    out += "               max-width: 100%; overflow-wrap: anywhere; }\n";
    out += "  .seed { color: #b5820a; font-weight: 600; }\n";
    out += "</style>\n</head>\n<body>\n";
    out += "<h1>Design gallery</h1>\n";

    if (sorted.empty()) {
        out += "<p>No <code>@dsCard</code>-tagged files found.</p>\n";
    }

    size_t i = 0;
    while (i < sorted.size()) {
        const std::string& gname = sorted[i].group;
        out += "<h2>" + html_escape(gname) + "</h2>\n<div class=\"grid\">\n";
        while (i < sorted.size() && sorted[i].group == gname) {
            const GalleryCard& c = sorted[i];
            std::string png = png_rel ? png_rel(c) : std::string{};
            std::string vp = std::to_string(c.width) + "x" + std::to_string(c.height);
            std::string wpx = std::to_string(c.width) + "px";
            std::string hpx = std::to_string(c.height) + "px";
            out += "<figure>\n";
            if (png.empty()) {
                out += "  <div class=\"miss\" style=\"width:" + wpx + ";height:" + hpx +
                       "\">not rendered</div>\n";
            } else {
                out += "  <img src=\"" + html_escape(png) + "\" width=\"" + std::to_string(c.width) +
                       "\" height=\"" + std::to_string(c.height) + "\" alt=\"" + html_escape(c.file) +
                       "\">\n";
            }
            out += "  <figcaption>" + html_escape(c.file) + "<br>" + vp;
            if (c.starting_point) out += " <span class=\"seed\">\xE2\x97\x86 start</span>";
            out += "</figcaption>\n</figure>\n";
            ++i;
        }
        out += "</div>\n";
    }
    out += "</body>\n</html>\n";
    return out;
}

}  // namespace pulp::design
