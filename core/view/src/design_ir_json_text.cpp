// The browser's captured line layout on a DesignIR text node: its line boxes
// and the basis that says when they still hold.
#include "design_ir_json_primitives.hpp"

#include <choc/text/choc_JSON.h>

namespace pulp::view {

using namespace ir_json;

void parse_ir_text_line_layout(IRNode& node, const choc::value::ValueView& obj) {
    if (!obj.isObject())
        return;
    if (obj.hasObjectMember("lineBoxes")) {
        const auto boxes = obj["lineBoxes"];
        if (boxes.isArray()) {
            for (uint32_t i = 0; i < boxes.size(); ++i) {
                const auto entry = boxes[static_cast<int>(i)];
                if (!entry.isObject())
                    continue;
                IRTextLineBox box;
                box.left = get_float(entry, "left");
                box.top = get_float(entry, "top");
                box.width = get_float(entry, "width");
                box.height = get_float(entry, "height");
                box.start = static_cast<int>(get_float(entry, "start"));
                box.length = static_cast<int>(get_float(entry, "length"));
                node.text_line_boxes.push_back(box);
            }
        }
    }
    if (obj.hasObjectMember("lineBoxBasis")) {
        const auto basis = obj["lineBoxBasis"];
        if (basis.isObject()) {
            IRTextLayoutBasis parsed;
            parsed.width = get_float(basis, "width");
            parsed.resolved_face = get_string(basis, "resolvedFace");
            if (basis.hasObjectMember("ascent") && basis.hasObjectMember("descent")) {
                const LineMetrics m{get_float(basis, "ascent"), get_float(basis, "descent")};
                if (m.valid())
                    parsed.line_metrics = m;
            }
            node.text_layout_basis = std::move(parsed);
        }
    }
    // Boxes without a basis cannot be validated, so they are not a cache.
    if (!node.text_layout_basis)
        node.text_line_boxes.clear();
}

void write_ir_text_line_layout(std::ostringstream& out, bool& first, const IRNode& node) {
    if (!node.text_line_boxes.empty() && node.text_layout_basis) {
        write_key(out, first, "lineBoxes");
        out << '[';
        for (size_t i = 0; i < node.text_line_boxes.size(); ++i) {
            if (i)
                out << ',';
            const auto& b = node.text_line_boxes[i];
            out << '{';
            bool bf = true;
            write_float_member(out, bf, "left", b.left);
            write_float_member(out, bf, "top", b.top);
            write_float_member(out, bf, "width", b.width);
            write_float_member(out, bf, "height", b.height);
            write_int_member(out, bf, "start", b.start);
            write_int_member(out, bf, "length", b.length);
            out << '}';
        }
        out << ']';
        write_key(out, first, "lineBoxBasis");
        out << '{';
        bool basis_first = true;
        write_float_member(out, basis_first, "width", node.text_layout_basis->width);
        write_string_member(out, basis_first, "resolvedFace",
                            node.text_layout_basis->resolved_face);
        if (const auto& m = node.text_layout_basis->line_metrics) {
            write_float_member(out, basis_first, "ascent", m->ascent);
            write_float_member(out, basis_first, "descent", m->descent);
        }
        out << '}';
    }
}

} // namespace pulp::view
