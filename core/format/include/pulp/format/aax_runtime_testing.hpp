#pragma once

#include <pulp/format/aax_editor.hpp>
#include <pulp/format/aax_model.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <AAX_Enums.h>
#include <acfunknown.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// A stand-in for the pieces of a host the AAX runtime's lifecycle tests need:
// the algorithm's private-data block that `ResetFieldData` constructs, and the
// instance-init callback the host drives. Nothing here is reachable from a
// shipped plug-in; the per-plugin wrapper never includes this header.

namespace pulp::format::aax::testing {

/// Size of the private-data block the algorithm registers with the host.
std::size_t private_data_block_size();

/// Run the model's `ResetFieldData` for the private-data field against
/// `block`, as the host does when it instantiates the algorithm.
AAX_Result reset_private_data(IACFUnknown* model, void* block, std::size_t size);

/// Drive the algorithm's instance-init callback for `block`.
int32_t instance_init_for_test(void* block, AAX_EComponentInstanceInitAction action);

/// The Processor that renders audio for `block`, or null when none exists.
Processor* algorithm_processor(void* block);

/// Its store, or null.
state::StateStore* algorithm_store(void* block);

/// The model's editor-facing surface.
EditorHost* editor_host(IACFUnknown* model);

/// Layout selection as the model performs it from the controller's stems.
std::size_t select_layout_for_test(const std::vector<ComponentLayout>& components,
                                   StemKind input_stem, StemKind output_stem);
/// Adopt a stem choice the way ResetFieldData does when the host's controller
/// answers, including the control-thread prepare.
void adopt_layout_for_test(void* block, StemKind input_stem, StemKind output_stem,
                           double sample_rate);
std::size_t instance_component_index(void* block);
/// Run one render callback over the instance with host-shaped pointer arrays.
bool render_for_test(void* block, float** inputs, float** outputs, int frames, double sample_rate);

} // namespace pulp::format::aax::testing
