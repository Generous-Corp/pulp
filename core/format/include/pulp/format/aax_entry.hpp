#pragma once

#include <pulp/format/aax_effect_gui.hpp>
#include <pulp/format/aax_model.hpp>

#include <AAX.h>
#include <AAX_Callbacks.h>

#include <string_view>

class AAX_ICollection;
class IACFUnknown;

namespace pulp::format::aax {

struct EntryConfig {
    ProcessorFactory factory = nullptr;
    PluginCodes codes{};
};

inline EntryConfig make_entry_config(ProcessorFactory factory,
                                     std::string_view manufacturer_code,
                                     std::string_view product_code,
                                     std::string_view native_code)
{
    return {
        .factory = factory,
        .codes = {
            .manufacturer_id = fourcc(manufacturer_code),
            .product_id = fourcc(product_code),
            .native_id_base = fourcc(native_code),
        },
    };
}

IACFUnknown* create_effect_parameters(const EntryConfig& config);

/// Describe the plugin to the host. @p create_proc builds the data model;
/// @p create_gui_proc builds the custom editor and is registered under
/// `kAAX_ProcPtrID_Create_EffectGUI`. Passing null for @p create_gui_proc
/// registers no editor, leaving the host on its auto-generated parameter strip.
AAX_Result get_effect_descriptions(AAX_ICollection* out_collection,
                                   const EntryConfig& config,
                                   AAXCreateObjectProc create_proc,
                                   AAXCreateObjectProc create_gui_proc = nullptr);

} // namespace pulp::format::aax

#if !defined(PULP_MANUFACTURER_CODE)
#error "PULP_MANUFACTURER_CODE must be defined for AAX targets"
#endif

#if !defined(PULP_AAX_PRODUCT_CODE)
#error "PULP_AAX_PRODUCT_CODE must be defined for AAX targets"
#endif

#if !defined(PULP_AAX_NATIVE_CODE)
#error "PULP_AAX_NATIVE_CODE must be defined for AAX targets"
#endif

// Shared body of the AAX entry-point macros. @p gui_proc_expr is the value
// handed to `get_effect_descriptions` as the editor's create proc: null selects
// the host's parameter strip, a proc address selects the custom editor.
#define PULP_AAX_PLUGIN_IMPL_(factory_fn, gui_proc_expr) \
    namespace { \
        static const ::pulp::format::aax::EntryConfig& _pulp_aax_entry_config() { \
            static const auto config = ::pulp::format::aax::make_entry_config( \
                factory_fn, \
                PULP_MANUFACTURER_CODE, \
                PULP_AAX_PRODUCT_CODE, \
                PULP_AAX_NATIVE_CODE); \
            return config; \
        } \
        static IACFUnknown* AAX_CALLBACK _pulp_aax_create_effect_parameters() { \
            return ::pulp::format::aax::create_effect_parameters(_pulp_aax_entry_config()); \
        } \
        [[maybe_unused]] static IACFUnknown* AAX_CALLBACK _pulp_aax_create_effect_gui() { \
            return ::pulp::format::aax::create_effect_gui(); \
        } \
    } \
    AAX_Result GetEffectDescriptions(AAX_ICollection* outCollection) { \
        return ::pulp::format::aax::get_effect_descriptions( \
            outCollection, \
            _pulp_aax_entry_config(), \
            &_pulp_aax_create_effect_parameters, \
            (gui_proc_expr)); \
    }

// Generate an AAX entry point that leaves the host on its auto-generated
// parameter strip. This is the default because the strip is what Pro Tools
// draws with no editor registered, and it is the behavior every plugin had
// before the custom editor existed: rebuilding must never silently trade it
// for a different UI. Pair with PULP_AAX_PLUGIN_WITH_GUI to opt in.
//
// The unused editor thunk below is discarded, so this macro links without the
// editor shell (and its view stack) at all.
#define PULP_AAX_PLUGIN(factory_fn) \
    PULP_AAX_PLUGIN_IMPL_(factory_fn, nullptr)

// Generate an AAX entry point that registers Pulp's custom editor, so the host
// instantiates the plugin's own `create_view()` UI instead of the parameter
// strip. Opt in deliberately: the editor has not been validated in Pro Tools
// itself, so a plugin choosing it is choosing an unproven path over a working
// one. The only difference from PULP_AAX_PLUGIN is the editor create proc —
// the data model, codes, and descriptor are identical.
#define PULP_AAX_PLUGIN_WITH_GUI(factory_fn) \
    PULP_AAX_PLUGIN_IMPL_(factory_fn, &_pulp_aax_create_effect_gui)
