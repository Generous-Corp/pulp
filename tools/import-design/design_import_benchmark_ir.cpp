#include "tools/import-design/design_import_benchmark_fixture.hpp"

#include <pulp/runtime/crypto.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pulp::design_import_bench {

using namespace pulp::view;

std::unique_ptr<View> materialize_checked(const DesignIR& ir, float width, float height,
                                          std::string_view lane,
                                          std::vector<std::string>* warning_codes) {
    NativeMaterializeOptions options;
    std::vector<ImportDiagnostic> diagnostics;
    options.diagnostics_out = &diagnostics;
    auto root = build_native_view_tree(ir, ir.asset_manifest, options);
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == ImportDiagnosticSeverity::error) {
            std::ostringstream msg;
            msg << lane << " materialization reported an error";
            if (!diagnostic.code.empty())
                msg << "; code=" << diagnostic.code;
            if (!diagnostic.message.empty())
                msg << ": " << diagnostic.message;
            throw std::runtime_error(msg.str());
        }
        if (warning_codes && std::find(warning_codes->begin(), warning_codes->end(),
                                       diagnostic.code) == warning_codes->end())
            warning_codes->push_back(diagnostic.code);
    }
    if (!root)
        throw std::runtime_error(std::string(lane) + " materialization returned null root");
    root->set_bounds({0, 0, width, height});
    root->layout_children();
    return root;
}

LoadedIr load_ir(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read DesignIR " + path.string());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    LoadedIr loaded;
    loaded.ir = parse_design_ir_json(text);
    loaded.sha256 = pulp::runtime::sha256_hex(text);
    loaded.file_name = path.filename().string();
    if (loaded.ir.root.style.width && *loaded.ir.root.style.width > 0.0f)
        loaded.width = *loaded.ir.root.style.width;
    if (loaded.ir.root.style.height && *loaded.ir.root.style.height > 0.0f)
        loaded.height = *loaded.ir.root.style.height;
    const auto collect = [&](auto& self, const IRNode& node) -> void {
        if (node.type == "text" && !node.text_content.empty() && node.stable_anchor_id &&
            !node.stable_anchor_id->empty())
            loaded.slot_anchors.push_back(*node.stable_anchor_id);
        for (const auto& child : node.children)
            self(self, child);
    };
    collect(collect, loaded.ir.root);
    auto sorted = loaded.slot_anchors;
    std::sort(sorted.begin(), sorted.end());
    if (auto dup = std::adjacent_find(sorted.begin(), sorted.end()); dup != sorted.end())
        throw std::runtime_error("DesignIR " + loaded.file_name + " repeats text slot anchor " +
                                 *dup + "; a keyed slot needs a unique anchor");
    if (loaded.slot_anchors.empty())
        throw std::runtime_error("DesignIR " + loaded.file_name + " has no anchored text slots");
    return loaded;
}

std::string slot_text(std::size_t slot, int frame) {
    return "slot " + std::to_string(slot) + " frame " + std::to_string(frame);
}

IrBoundFixture::IrBoundFixture(const std::filesystem::path& path, bool rebuild_each_step)
    : loaded_(load_ir(path)), rebuild_each_step_(rebuild_each_step) {
    materialize(loaded_.ir);
    for (std::size_t i = 0; i < loaded_.slot_anchors.size(); ++i) {
        auto* label = resolved_[i];
        if (!label)
            throw std::runtime_error("text slot " + loaded_.slot_anchors[i] +
                                     " did not materialize as a Label");
        slots_.push_back({label->import_binding_instance_id()});
    }
}

void IrBoundFixture::step(int frame) {
    if (rebuild_each_step_) {
        auto ir = loaded_.ir;
        std::size_t slot = 0;
        const auto write = [&](auto& self, IRNode& node) -> void {
            if (node.type == "text" && node.stable_anchor_id &&
                slot < loaded_.slot_anchors.size() &&
                *node.stable_anchor_id == loaded_.slot_anchors[slot])
                node.text_content = slot_text(slot++, frame);
            for (auto& child : node.children)
                self(self, child);
        };
        write(write, ir.root);
        materialize(ir);
    } else {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            retained_[i]->set_text(slot_text(i, frame));
        root_->layout_children();
    }
}

// Reads every slot back through the anchors of the CURRENT tree, so a slot
// that rebound to another View, or kept stale text, is counted.
void IrBoundFixture::verify(int frame) {
    resolve_slots();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto* label = resolved_[i];
        if (!label || label->import_binding_instance_id() != slots_[i].instance_id) {
            ++identity_changes_;
            if (label)
                slots_[i].instance_id = label->import_binding_instance_id();
        }
        if (!label || label->text() != slot_text(i, frame))
            ++text_mismatches_;
    }
}

// Builds the tree and resolves every slot anchor in one walk. The retained
// lane keeps writing through the Labels resolved at construction;
// `resolved_` always describes the current tree.
void IrBoundFixture::materialize(const DesignIR& ir) {
    root_ = materialize_checked(ir, loaded_.width, loaded_.height, "IR-bound", &warning_codes_);
    resolve_slots();
    // Only the retained lane writes through the Labels of its first tree; the
    // rebuild lane replaces that tree, so it keeps no such pointers.
    if (!rebuild_each_step_ && retained_.empty())
        retained_ = resolved_;
}

// Resolves every slot anchor against the CURRENT tree.
void IrBoundFixture::resolve_slots() {
    resolved_.assign(loaded_.slot_anchors.size(), nullptr);
    const auto visit = [&](auto& self, View& view) -> void {
        for (std::size_t i = 0; i < loaded_.slot_anchors.size(); ++i)
            if (view.anchor_id() == loaded_.slot_anchors[i])
                resolved_[i] = dynamic_cast<Label*>(&view);
        for (std::size_t c = 0; c < view.child_count(); ++c)
            self(self, *view.child_at(c));
    };
    visit(visit, *root_);
}

} // namespace pulp::design_import_bench
