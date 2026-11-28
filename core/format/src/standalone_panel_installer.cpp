#include <pulp/format/standalone_panel.hpp>

// Kept in its own file so an executable's own definition of
// install_standalone_panels() replaces this one at link time instead of
// colliding with it: the archive member is only pulled in when the symbol is
// still undefined.
namespace pulp::format::detail {

void install_standalone_panels() {}

} // namespace pulp::format::detail
