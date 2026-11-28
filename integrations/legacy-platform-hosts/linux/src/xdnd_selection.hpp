#pragma once

// X11-free selection-ownership rule for the XDND drag source.

namespace pulp::view::xdnd {

// Whether the drag source may clear XdndSelection on teardown: only while it
// is still the owner. Another client that took the selection mid-drag keeps it.
inline bool owns_selection(unsigned long current_owner, unsigned long source) {
    return source != 0 && current_owner == source;
}

} // namespace pulp::view::xdnd
