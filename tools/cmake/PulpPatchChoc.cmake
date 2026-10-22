function(pulp_patch_choc_v8 choc_source_dir)
    set(_pulp_choc_v8_header "${choc_source_dir}/choc/javascript/choc_javascript_V8.h")

    if(NOT EXISTS "${_pulp_choc_v8_header}")
        message(WARNING "Pulp: CHOC V8 header not found for compatibility patch: ${_pulp_choc_v8_header}")
        return()
    endif()

    file(READ "${_pulp_choc_v8_header}" _pulp_choc_v8)
    set(_pulp_choc_v8_original "${_pulp_choc_v8}")

    string(REPLACE
"#include \"choc_javascript.h\""
"#include \"choc_javascript.h\"
#include <v8-version.h>"
        _pulp_choc_v8 "${_pulp_choc_v8}")
    string(REPLACE
"#include \"choc_javascript.h\"
#include <v8-version.h>
#include <v8-version.h>"
"#include \"choc_javascript.h\"
#include <v8-version.h>"
        _pulp_choc_v8 "${_pulp_choc_v8}")

    string(REPLACE
"        isolate->SetHostImportModuleDynamicallyCallback (+[] (v8::Local<v8::Context> c,
                                                              v8::Local<v8::Data> /*hostOptions*/,
                                                              v8::Local<v8::Value> /*resourceName*/,
                                                              v8::Local<v8::String> specifier,
                                                              v8::Local<v8::FixedArray> /*importAssertions*/)
        {
            return static_cast<V8Context*> (c->GetIsolate()->GetData (0))
                ->loadDynamicModule (c, specifier);
        });"
"        isolate->SetHostImportModuleDynamicallyCallback (+[] (v8::Local<v8::Context> c,
                                                              v8::Local<v8::Data> /*hostOptions*/,
                                                              v8::Local<v8::Value> /*resourceName*/,
                                                              v8::Local<v8::String> specifier,
                                                              v8::Local<v8::FixedArray> /*importAssertions*/)
            -> v8::MaybeLocal<v8::Promise>
        {
            return static_cast<V8Context*> (v8::Isolate::GetCurrent()->GetData (0))
                ->loadDynamicModule (c, specifier);
        });"
        _pulp_choc_v8 "${_pulp_choc_v8}")
    string(REPLACE
"            return static_cast<V8Context*> (c.GetIsolate()->GetData (0))"
"            return static_cast<V8Context*> (v8::Isolate::GetCurrent()->GetData (0))"
        _pulp_choc_v8 "${_pulp_choc_v8}")

    # The replacement keeps the original as its #else branch, so it would
    # nest again on a second run; apply it only to an unpatched header.
    string(FIND "${_pulp_choc_v8}" "auto len = s.Utf8LengthV2 (isolate);" _pulp_choc_v8_utf8_patched)
    if(_pulp_choc_v8_utf8_patched EQUAL -1)
    string(REPLACE
"        auto len = s.Utf8Length (isolate);
        result.resize (static_cast<std::string::size_type> (len));
        s.WriteUtf8 (isolate, result.data(), len);"
"       #if defined(V8_MAJOR_VERSION) && V8_MAJOR_VERSION >= 14
        auto len = s.Utf8LengthV2 (isolate);
        result.resize (static_cast<std::string::size_type> (len));
        s.WriteUtf8V2 (isolate, result.data(), static_cast<size_t> (len), v8::String::WriteFlags::kNone);
       #else
        auto len = s.Utf8Length (isolate);
        result.resize (static_cast<std::string::size_type> (len));
        s.WriteUtf8 (isolate, result.data(), len);
       #endif"
        _pulp_choc_v8 "${_pulp_choc_v8}")
    endif()
    string(REPLACE
"       #if V8_MAJOR_VERSION >= 14"
"       #if defined(V8_MAJOR_VERSION) && V8_MAJOR_VERSION >= 14"
        _pulp_choc_v8 "${_pulp_choc_v8}")

    string(REPLACE
"        auto ok = mod.ToLocalChecked()->InstantiateModule (localContext, +[] (v8::Local<v8::Context> c, v8::Local<v8::String> specifier,
                                                                              v8::Local<v8::FixedArray>, v8::Local<v8::Module>)
        {
            return static_cast<V8Context*> (c->GetIsolate()->GetData (0))
                ->loadStaticModule (c, specifier);
        });"
"        auto ok = mod.ToLocalChecked()->InstantiateModule (localContext, +[] (v8::Local<v8::Context> c, v8::Local<v8::String> specifier,
                                                                              v8::Local<v8::FixedArray>, v8::Local<v8::Module>)
            -> v8::MaybeLocal<v8::Module>
        {
            return static_cast<V8Context*> (v8::Isolate::GetCurrent()->GetData (0))
                ->loadStaticModule (c, specifier);
        });"
        _pulp_choc_v8 "${_pulp_choc_v8}")

    # v8ToChoc collects property values inside an Array::Iterate callback and
    # converts them after it returns. Since V8 15.4 Iterate runs the callback in
    # its own HandleScope, so those Locals dangle and the next conversion reads
    # freed handle slots (SIGBUS in KeyAccumulator). Iterate also forbids the JS
    # execution a getter can trigger through Get(). Walk the names by index.
    set(_pulp_choc_v8_iterate_original
"                using IteratorType = decltype (iterator);

                propNames.ToLocalChecked()->Iterate (c, +[] (uint32_t, v8::Local<v8::Value> key, void* i)
                {
                    (*static_cast<IteratorType*> (i)) (key);
                    return v8::Array::CallbackResult::kContinue;
                },
                std::addressof (iterator));")
    set(_pulp_choc_v8_iterate_patched
"                auto names = propNames.ToLocalChecked();

                for (uint32_t i = 0; i < names->Length(); ++i)
                {
                    auto key = names->Get (c, i);

                    if (! key.IsEmpty())
                        iterator (key.ToLocalChecked());
                }")
    string(FIND "${_pulp_choc_v8}" "${_pulp_choc_v8_iterate_patched}" _pulp_choc_v8_iterate_done)
    if(_pulp_choc_v8_iterate_done EQUAL -1)
        string(FIND "${_pulp_choc_v8}" "${_pulp_choc_v8_iterate_original}" _pulp_choc_v8_iterate_at)
        if(_pulp_choc_v8_iterate_at EQUAL -1)
            message(FATAL_ERROR
                "Pulp: CHOC V8 v8ToChoc property walk no longer matches the reviewed source; update or retire Pulp's Array::Iterate patch")
        endif()
        string(REPLACE "${_pulp_choc_v8_iterate_original}" "${_pulp_choc_v8_iterate_patched}"
            _pulp_choc_v8 "${_pulp_choc_v8}")
    endif()

    if(NOT _pulp_choc_v8 STREQUAL _pulp_choc_v8_original)
        file(WRITE "${_pulp_choc_v8_header}" "${_pulp_choc_v8}")
        message(STATUS "Pulp: applied CHOC V8 compatibility patch")
    endif()
endfunction()

function(pulp_patch_choc_json_number_exponent choc_source_dir)
    set(_pulp_choc_json_header
        "${choc_source_dir}/choc/text/choc_JSON.h")
    if(NOT EXISTS "${_pulp_choc_json_header}")
        message(FATAL_ERROR
            "Pulp: CHOC JSON header not found for standards patch: ${_pulp_choc_json_header}")
    endif()

    file(READ "${_pulp_choc_json_header}" _pulp_choc_json)
    set(_pulp_choc_json_original
"                    hadExponent = true;
                    popIf ('-');
                    continue;")
    set(_pulp_choc_json_patched
"                    hadExponent = true;
                    if (! popIf ('-'))
                        popIf ('+');
                    continue;")

    string(FIND "${_pulp_choc_json}" "${_pulp_choc_json_patched}"
        _pulp_choc_json_patched_position)
    if(NOT _pulp_choc_json_patched_position EQUAL -1)
        return()
    endif()

    string(FIND "${_pulp_choc_json}" "${_pulp_choc_json_original}"
        _pulp_choc_json_original_position)
    if(_pulp_choc_json_original_position EQUAL -1)
        message(FATAL_ERROR
            "Pulp: CHOC JSON exponent parser no longer matches the reviewed source; update or retire Pulp's standards patch")
    endif()

    string(REPLACE "${_pulp_choc_json_original}" "${_pulp_choc_json_patched}"
        _pulp_choc_json "${_pulp_choc_json}")
    file(WRITE "${_pulp_choc_json_header}" "${_pulp_choc_json}")
    message(STATUS "Pulp: applied CHOC JSON positive-exponent patch")
endfunction()
