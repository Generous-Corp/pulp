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

    if(NOT _pulp_choc_v8 STREQUAL _pulp_choc_v8_original)
        file(WRITE "${_pulp_choc_v8_header}" "${_pulp_choc_v8}")
        message(STATUS "Pulp: applied CHOC V8 compatibility patch")
    endif()
endfunction()
