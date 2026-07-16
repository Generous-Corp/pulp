#pragma once

#include <AAX.h>
#include <AAX_Callbacks.h>

class IACFUnknown;

namespace pulp::format::aax {

/// Create the AAX custom editor object. Registered with the host under
/// `kAAX_ProcPtrID_Create_EffectGUI`; without that registration Pro Tools has
/// no plug-in GUI to instantiate and falls back to its auto-generated
/// parameter strip.
///
/// The returned object is owned by the host. The signature matches
/// `AAXCreateObjectProc` so it can be handed to `AAX_IEffectDescriptor::AddProcPtr`.
IACFUnknown* AAX_CALLBACK create_effect_gui();

} // namespace pulp::format::aax
