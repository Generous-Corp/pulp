#pragma once

// Private implementation: included only by the style loader translation unit.
#include "browser_capture_canvas.hpp"

#include <choc/containers/choc_Value.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace pulp::import_design::detail {

// This is an optional paint proof, not snapshot validation. Unsupported or
// incomplete evidence must preserve the importer's existing output.
inline std::optional<DocumentCanvasPaint>
extract_document_canvas_paint(const choc::value::ValueView& snapshot) {
    try {
        const auto member = [](const choc::value::ValueView& value, const char* key) {
            return value.isObject() && value.hasObjectMember(key) ? value[key]
                                                                  : choc::value::ValueView{};
        };
        const auto number = [](const choc::value::ValueView& value) {
            return value.isInt() || value.isFloat()
                       ? value.getWithDefault<double>(std::numeric_limits<double>::quiet_NaN())
                       : std::numeric_limits<double>::quiet_NaN();
        };
        const auto index = [&](const choc::value::ValueView& value, uint32_t size) {
            const double n = number(value);
            return std::isfinite(n) && n >= 0 && n < size && std::trunc(n) == n
                       ? static_cast<int>(n)
                       : -1;
        };
        const auto docs = member(snapshot, "documents");
        if (!docs.isArray() || docs.size() != 1)
            return std::nullopt;
        const auto doc = docs[0];
        const auto names = member(snapshot, "computedStyleNames");
        const auto strings = member(snapshot, "strings");
        if (!names.isArray() || !strings.isArray())
            return std::nullopt;
        std::unordered_set<std::string> properties;
        for (const auto name : names)
            if (!name.isString() || name.toString().empty() ||
                !properties.insert(name.toString()).second)
                return std::nullopt;
        for (const auto value : strings)
            if (!value.isString())
                return std::nullopt;
        const auto nodes = member(doc, "nodes");
        const auto parents = member(nodes, "parentIndex");
        const auto types = member(nodes, "nodeType");
        const auto tags = member(nodes, "nodeName");
        const auto ids = member(nodes, "backendNodeId");
        if (!parents.isArray() || !types.isArray() || !tags.isArray() || !ids.isArray() ||
            parents.size() == 0 || parents.size() != types.size() ||
            parents.size() != tags.size() || parents.size() != ids.size())
            return std::nullopt;
        const auto count = parents.size();
        int html = -1, body = -1, document_node = -1;
        std::unordered_set<double> backend_ids;
        for (uint32_t i = 0; i < count; ++i) {
            const auto at = static_cast<int>(i);
            const auto parent = number(parents[at]);
            const auto type = number(types[at]);
            const auto id = number(ids[at]);
            const auto tag = index(tags[at], strings.size());
            // CDP's preorder guarantees each ancestor precedes its children.
            if (tag < 0 || !std::isfinite(id) || id < 0 || id > std::numeric_limits<int>::max() ||
                std::trunc(id) != id || !backend_ids.insert(id).second || !std::isfinite(type) ||
                type < 1 || type > 12 || std::trunc(type) != type || !std::isfinite(parent) ||
                std::trunc(parent) != parent || (i == 0 ? parent != -1 : parent < 0 || parent >= i))
                return std::nullopt;
            if (type == 9) {
                if (document_node != -1 || i != 0)
                    return std::nullopt;
                document_node = at;
            }
            if (type != 1)
                continue; // A doctype can also be named HTML.
            auto tag_name = strings[tag].toString();
            std::transform(tag_name.begin(), tag_name.end(), tag_name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (tag_name == "html") {
                if (html != -1)
                    return std::nullopt;
                html = at;
            }
            if (tag_name == "body") {
                if (body != -1)
                    return std::nullopt;
                body = at;
            }
        }
        if (document_node != 0 || html < 0 || body < 0 || number(parents[html]) != document_node ||
            number(parents[body]) != html)
            return std::nullopt;
        // Absence of a painted pseudo row is not evidence of absent pseudos.
        // Validate the raw sparse tables, including entries with no layout.
        for (const char* key : {"pseudoType", "pseudoIdentifier", "contentDocumentIndex"}) {
            const auto sparse = member(nodes, key);
            // CDP RareStringData/RareIntegerData fields are optional; an
            // omitted sparse field has no entries, unlike a missing CSS value.
            if (!nodes.hasObjectMember(key))
                continue;
            const auto indices = member(sparse, "index");
            const auto values = member(sparse, "value");
            if (!indices.isArray() || !values.isArray() || indices.size() != values.size())
                return std::nullopt;
            std::unordered_set<int> seen;
            for (uint32_t i = 0; i < indices.size(); ++i) {
                const int node = index(indices[static_cast<int>(i)], count);
                if (node < 0 || !seen.insert(node).second)
                    return std::nullopt;
                if (std::string_view(key) == "contentDocumentIndex")
                    return std::nullopt;
                if (index(values[static_cast<int>(i)], strings.size()) < 0 || node == html ||
                    node == body || number(parents[node]) == html || number(parents[node]) == body)
                    return std::nullopt;
            }
        }
        const auto layout = member(doc, "layout");
        const auto layout_nodes = member(layout, "nodeIndex");
        const auto rows = member(layout, "styles");
        const auto bounds = member(layout, "bounds");
        if (!layout_nodes.isArray() || !rows.isArray() || !bounds.isArray() ||
            layout_nodes.size() != rows.size() || rows.size() != bounds.size())
            return std::nullopt;
        std::map<std::string, std::string> html_style, body_style;
        std::unordered_set<int> laid_out;
        int html_row = -1;
        for (uint32_t i = 0; i < rows.size(); ++i) {
            const int at = static_cast<int>(i);
            const int node = index(layout_nodes[at], count);
            const auto row = rows[at];
            const auto box = bounds[at];
            if (node < 0 || !laid_out.insert(node).second || !row.isArray() ||
                (row.size() != names.size() && !(number(types[node]) == 9 && row.size() == 0)) ||
                !box.isArray() || box.size() != 4)
                return std::nullopt;
            for (int j = 0; j < 4; ++j)
                if (!std::isfinite(number(box[j])) || (j >= 2 && number(box[j]) < 0))
                    return std::nullopt;
            for (uint32_t j = 0; j < row.size(); ++j) {
                const auto value = index(row[static_cast<int>(j)], strings.size());
                if (value < 0)
                    return std::nullopt;
                if (node == html || node == body)
                    (node == html ? html_style
                                  : body_style)[names[static_cast<int>(j)].toString()] =
                        strings[value].toString();
            }
            if (node == html)
                html_row = at;
        }
        if (html_row < 0 || body_style.empty())
            return std::nullopt;
        const std::map<std::string, std::string> safe{{"background-image", "none"},
                                                      {"background-blend-mode", "normal"},
                                                      {"mix-blend-mode", "normal"},
                                                      {"filter", "none"},
                                                      {"backdrop-filter", "none"},
                                                      {"transform", "none"},
                                                      {"clip-path", "none"},
                                                      {"mask-image", "none"},
                                                      {"opacity", "1"}};
        for (const auto* style : {&html_style, &body_style})
            for (const auto& [key, value] : safe) {
                const auto found = style->find(key);
                if (found == style->end() || found->second != value)
                    return std::nullopt;
            }
        const auto color = [](const std::string& text) -> std::optional<std::array<double, 4>> {
            if (text.find_first_not_of("rgba0123456789., ()") != std::string::npos)
                return std::nullopt;
            std::array<double, 4> rgba{0, 0, 0, 1};
            int end = 0;
            const bool parsed = std::sscanf(text.c_str(), "rgb(%lf, %lf, %lf)%n", &rgba[0],
                                            &rgba[1], &rgba[2], &end) == 3 &&
                                end == static_cast<int>(text.size());
            if (!parsed) {
                end = 0;
                if (std::sscanf(text.c_str(), "rgba(%lf, %lf, %lf, %lf)%n", &rgba[0], &rgba[1],
                                &rgba[2], &rgba[3], &end) != 4 ||
                    end != static_cast<int>(text.size()))
                    return std::nullopt;
            }
            for (int i = 0; i < 4; ++i)
                if (!std::isfinite(rgba[i]) || rgba[i] < 0 || rgba[i] > (i == 3 ? 1 : 255) ||
                    (i < 3 && std::trunc(rgba[i]) != rgba[i]))
                    return std::nullopt;
            return rgba;
        };
        const auto html_color = color(html_style["background-color"]);
        const auto body_color = color(body_style["background-color"]);
        if (!html_color || !body_color || (*html_color)[3] != 1 ||
            ((*body_color)[3] != 0 && *body_color != *html_color))
            return std::nullopt;
        const double width = number(member(doc, "contentWidth"));
        const double height = number(member(doc, "contentHeight"));
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
            number(member(doc, "scrollOffsetX")) != 0 || number(member(doc, "scrollOffsetY")) != 0)
            return std::nullopt;
        const auto box = bounds[html_row];
        if (number(box[0]) != 0 || number(box[1]) != 0 || number(box[2]) <= 0 ||
            number(box[3]) <= 0 || number(box[2]) > width || number(box[3]) > height)
            return std::nullopt;
        return DocumentCanvasPaint{"rgb(" + std::to_string(static_cast<int>((*html_color)[0])) +
                                       ", " + std::to_string(static_cast<int>((*html_color)[1])) +
                                       ", " + std::to_string(static_cast<int>((*html_color)[2])) +
                                       ")",
                                   width, height};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace pulp::import_design::detail
