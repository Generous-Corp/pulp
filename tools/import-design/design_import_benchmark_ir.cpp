#include "tools/import-design/design_import_benchmark_fixture.hpp"

#include <pulp/runtime/crypto.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
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
                                          std::vector<std::string>* warning_codes,
                                          const std::filesystem::path& asset_base_directory) {
    NativeMaterializeOptions options;
    options.asset_base_directory = asset_base_directory;
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

HashedFile read_hashed_file(const std::filesystem::path& path, std::string_view what) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + std::string(what) + " " + path.string());
    HashedFile file;
    file.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    file.sha256 = pulp::runtime::sha256_hex(file.text);
    file.file_name = path.filename().string();
    return file;
}

LoadedIr load_ir(const std::filesystem::path& path) {
    auto file = read_hashed_file(path, "DesignIR");
    LoadedIr loaded;
    loaded.ir = parse_design_ir_json(file.text);
    loaded.sha256 = std::move(file.sha256);
    loaded.file_name = std::move(file.file_name);
    loaded.base_dir = path.parent_path();
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

std::vector<std::string> slot_texts(std::size_t count, int frame) {
    std::vector<std::string> texts;
    texts.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        texts.push_back(slot_text(i, frame));
    return texts;
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
    root_ = materialize_checked(ir, loaded_.width, loaded_.height, "IR-bound", &warning_codes_,
                                loaded_.base_dir);
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

namespace {

// Frames the runtime drains after mounting, matching pulp-screenshot's
// default, so effects, animation frames and timers that run once after the
// first commit are not billed to the first measured update.
constexpr int kMountSettleFrames = 64;

std::string js_string(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '\'':
            out += "\\'";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out + "'";
}

} // namespace

IrLiveFixture::IrLiveFixture(const std::filesystem::path& ir_path,
                             const std::filesystem::path& bundle_path)
    : loaded_(load_ir(ir_path)) {
    std::string bundle;
    if (!bundle_path.empty()) {
        auto file = read_hashed_file(bundle_path, "materialized bundle");
        bundle = std::move(file.text);
        bundle_sha256_ = std::move(file.sha256);
        bundle_file_name_ = std::move(file.file_name);
    }
    root_ = std::make_unique<View>();
    root_->set_bounds({0, 0, loaded_.width, loaded_.height});
    engine_ = std::make_unique<ScriptEngine>();
    store_ = std::make_unique<state::StateStore>();
    bridge_ = std::make_unique<WidgetBridge>(*engine_, *root_, *store_);
    bridge_->install_runtime_import_handlers();

    // The mount order a live import uses: the behavior realm first (the
    // captured document may replace document.body while it bootstraps), then
    // the DesignIR as the visible tree, then canvas bindings and a settle.
    if (!bundle.empty()) {
        bridge_->set_script_base_dir(bundle_path.parent_path());
        bridge_->load_script(bundle);
    }
    auto ir = loaded_.ir;
    enrich_imported_image_asset_metadata(ir, ir.asset_manifest, loaded_.base_dir.string());
    CodeGenOptions codegen;
    codegen.mode = CodeGenMode::bridge_native_js;
    // A bundle already owns `root`; mount the DesignIR under an id its tree
    // cannot alias.
    codegen.root_variable = bundle.empty() ? "root" : "__pulp_design_ir_root__";
    bridge_->set_script_base_dir(loaded_.base_dir);
    bridge_->load_script(generate_pulp_js(ir, codegen));
    const std::string settle = "if (typeof __pulpRuntimeSettle__ === 'function') "
                               "__pulpRuntimeSettle__(" +
                               std::to_string(kMountSettleFrames) + ");";
    const std::string bind = "if (typeof __pulpBindMaterializedCanvases__ === 'function') "
                             "__pulpBindMaterializedCanvases__();";
    if (!bundle.empty())
        bridge_->load_script(bind);
    bridge_->load_script(settle);
    if (!bundle.empty())
        bridge_->load_script(bind);
    root_->layout_children();

    const auto labels = resolve_slots();
    std::ostringstream script;
    script << "function __pulpBenchSlots(frame) {\n";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        slot_instance_ids_.push_back(labels[i]->import_binding_instance_id());
        script << "  setText(" << js_string(labels[i]->id()) << ", 'slot " << i
               << " frame ' + frame);\n";
    }
    script << "}\n";
    bridge_->load_script(script.str());
}

IrLiveFixture::~IrLiveFixture() {
    // The bridge holds callbacks into the tree and the engine; release it
    // before either.
    bridge_.reset();
    engine_.reset();
}

std::uint64_t IrLiveFixture::bridge_call_count() const {
    return bridge_->bridge_call_count();
}

void IrLiveFixture::step(int frame) {
    engine_->evaluate("__pulpBenchSlots(" + std::to_string(frame) + ");");
    ++js_evaluations_;
    // One host frame tick: the runtime's animation frames, timers and any
    // commit the writes scheduled run here, as they would in an editor.
    bridge_->service_frame_callbacks();
}

std::vector<std::vector<Label*>> IrLiveFixture::find_slot_labels() const {
    std::vector<std::vector<Label*>> found(loaded_.slot_anchors.size());
    const auto visit = [&](auto& self, View& view) -> void {
        for (std::size_t i = 0; i < loaded_.slot_anchors.size(); ++i)
            if (view.anchor_id() == loaded_.slot_anchors[i])
                if (auto* label = dynamic_cast<Label*>(&view))
                    found[i].push_back(label);
        for (std::size_t c = 0; c < view.child_count(); ++c)
            self(self, *view.child_at(c));
    };
    visit(visit, *root_);
    return found;
}

// Every slot must resolve to exactly one Label carrying a bridge id; an
// anchor the mounted tree drops or duplicates fails the fixture rather than
// measuring a different tree than the DesignIR describes.
std::vector<Label*> IrLiveFixture::resolve_slots() const {
    const auto found = find_slot_labels();
    std::vector<Label*> labels;
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (found[i].size() != 1)
            throw std::runtime_error("live text slot " + loaded_.slot_anchors[i] + " resolved to " +
                                     std::to_string(found[i].size()) +
                                     " Labels in the mounted tree; it needs exactly one");
        if (found[i].front()->id().empty())
            throw std::runtime_error("live text slot " + loaded_.slot_anchors[i] +
                                     " has no bridge id to write through");
        labels.push_back(found[i].front());
    }
    return labels;
}

// Reads every slot back through the anchors of the mounted tree, as the
// IR-bound lanes do.
void IrLiveFixture::verify(int frame) {
    const auto found = find_slot_labels();
    for (std::size_t i = 0; i < found.size(); ++i) {
        auto* label = found[i].size() == 1 ? found[i].front() : nullptr;
        if (!label || label->import_binding_instance_id() != slot_instance_ids_[i]) {
            ++identity_changes_;
            if (label)
                slot_instance_ids_[i] = label->import_binding_instance_id();
        }
        if (!label || label->text() != slot_text(i, frame))
            ++text_mismatches_;
    }
}

} // namespace pulp::design_import_bench
