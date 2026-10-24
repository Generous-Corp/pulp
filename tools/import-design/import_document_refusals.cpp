// SPDX-License-Identifier: MIT
#include "import_document_refusals.hpp"

#include <pulp/view/design_import.hpp>

#include <cstddef>

namespace pulp::import_design {

namespace {

// Captured states nobody can render are a hard error, not a diagnostic. Only
// a faithful_svg node lowers alternate_frames to DesignFrameView::add_frame,
// so on any other node the extra states are dropped and the import "succeeds"
// with a single frame; the user asked for N states and silently got one.
// Refusing here is what keeps that from shipping as a working command.
bool refuse_unrenderable_states(const pulp::view::IRNode& root, std::ostream& err) {
    const auto dropped = pulp::view::find_unrenderable_alternate_frames(root);
    if (dropped.empty())
        return false;
    std::size_t total = 0;
    for (const auto& d : dropped)
        total += d.alternates;
    err << "Error: " << total << " captured state" << (total == 1 ? "" : "s")
        << " cannot be rendered and would be "
           "dropped:\n";
    for (const auto& d : dropped)
        err << "       - " << d.node_name << ": " << d.alternates << " alternate frame"
            << (d.alternates == 1 ? "" : "s") << " (" << d.reason << ")\n";
    err << "       Multi-state capture needs a faithful-vector export. The Figma "
           "REST lane produces one:\n"
           "         python3 tools/import-design/figma_rest_export.py --file-key "
           "<KEY> --node <A> --out a.pulp.json --faithful-vector\n"
           "         python3 tools/import-design/figma_rest_export.py --file-key "
           "<KEY> --node <B> --out b.pulp.json --faithful-vector\n"
           "         pulp import-design --from figma-plugin --file a.pulp.json "
           "--file b.pulp.json ...\n";
    return true;
}

// A repeated stable anchor is a hard error for the same reason: every emitter
// addresses a node by its anchor, so the repeat silently loses its binding (the
// generated C++ binder requires exactly one match and binds neither node), its
// tweaks and its lock-to-source edits.
bool refuse_duplicate_anchors(const pulp::view::IRNode& root, std::ostream& err) {
    const auto duplicates = pulp::view::find_duplicate_anchors(root);
    if (duplicates.empty())
        return false;
    err << "Error: " << duplicates.size() << " node"
        << (duplicates.size() == 1 ? " repeats" : "s repeat")
        << " an earlier node's stable anchor, so bindings, tweaks and "
           "lock-to-source could not tell them apart:\n";
    for (const auto& d : duplicates)
        err << "       - '" << d.node_name << "' at " << d.path << " has anchor '" << d.anchor
            << "', already used by '" << d.first_node_name << "' at " << d.first_path << "\n";
    err << "       Give each node its own source identity (a Figma or Pencil node "
           "id, or a distinct data-pulp-key) and import again.\n";
    return true;
}

} // namespace

bool refuse_unimportable_document(const pulp::view::IRNode& root, std::ostream& err) {
    return refuse_unrenderable_states(root, err) || refuse_duplicate_anchors(root, err);
}

} // namespace pulp::import_design
