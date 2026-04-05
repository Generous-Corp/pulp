#pragma once

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
AAX_Result get_effect_descriptions(AAX_ICollection* out_collection,
                                   const EntryConfig& config,
                                   AAXCreateObjectProc create_proc);

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

#define PULP_AAX_PLUGIN(factory_fn) \
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
    } \
    AAX_Result GetEffectDescriptions(AAX_ICollection* outCollection) { \
        return ::pulp::format::aax::get_effect_descriptions( \
            outCollection, \
            _pulp_aax_entry_config(), \
            &_pulp_aax_create_effect_parameters); \
    }
