// SPDX-License-Identifier: MIT
#pragma once

#include <pulp/view/design_ir.hpp>

#include <ostream>

namespace pulp::import_design {

/// Refuses a parsed document the emitters would turn into output that looks
/// complete and is not: captured states nobody can render, and nodes that
/// repeat an earlier node's explicit stable anchor (a source id or an authored
/// anchor). Run resolve_anchor_collisions() first: a repeated anchor derived
/// from content is disambiguated, not refused. Prints one error naming every
/// offending node to @p err and returns true when the import must stop (exit
/// 2, before anything is written); returns false and prints nothing otherwise.
bool refuse_unimportable_document(const pulp::view::IRNode& root, std::ostream& err);

} // namespace pulp::import_design
