// pulp-format: the Processor vtable anchor.
//
// Processor::create_view() is the first non-inline, non-pure virtual declared
// on Processor, which makes it the class's key function: this translation
// unit is where the compiler emits `vtable for Processor` and its typeinfo.
// Two consequences bind the target layout, and neither is optional:
//
//  1. This TU must live in pulp-format-core. A core library without the
//     Processor vtable cannot link anything that derives from Processor,
//     which is every plugin, plus pulp-sequence.
//  2. The emitted vtable holds slots pointing at Processor's other
//     out-of-line virtuals -- process_f64 (processor_f64.cpp) and
//     create_ara_document_controller (ara.cpp) -- so this object carries
//     undefined references to both. Those two files must therefore stay in
//     pulp-format-core alongside this one; classifying either into
//     pulp-format-view would leave the core vtable unresolvable.
//
// This TU includes pulp/view/view.hpp, and that include is COMPILE-ONLY: it
// adds no view symbol to pulp-format-core's link closure. Both halves of that
// matter, and both are measured.
//
// Why the include is required, and why a forward declaration is not enough:
// create_view() returns std::unique_ptr<view::View> by value, and libstdc++
// instantiates ~unique_ptr<View> in the return path, which needs sizeof(View).
// GCC therefore rejects this file against a forward declaration alone --
//   unique_ptr.h: invalid application of 'sizeof' to incomplete type
//     'pulp::view::View'
// -- while Apple Clang with libc++ accepts it, because it does not instantiate
// the destructor there. Defining and calling have different completeness
// requirements, and the requirement is also toolchain-dependent; do not
// re-derive it from one compiler.
//
// Why it costs nothing at link: view::View's destructor is virtual, so
// destroying through unique_ptr<view::View> dispatches through the vptr rather
// than naming ~View. This object file carries ZERO undefined pulp::view::
// symbols, so pulp-format-core still links with no view archive and consumers
// inherit none. If ~View were ever made non-virtual, that would stop being
// true and this file would drag the view layer into every core consumer.
//
// The consequence for the split is narrow and worth stating plainly:
// pulp-format-core LINKS without the view layer, but does not COMPILE without
// view headers. The Skia/Dawn closure the split exists to remove stays
// removed; only this one translation unit needs the headers, privately.
//
// The SettingsSection special members ALSO need view::View complete
// (SettingsSection owns a unique_ptr<view::View>), so they live in
// settings_section.cpp on the view side of the split.
#include <pulp/format/format.hpp>
#include <pulp/view/view.hpp>

namespace pulp::format {

std::unique_ptr<view::View> Processor::create_view() {
    return nullptr;
}

} // namespace pulp::format
