# PulpAuv3Entitlements.cmake — per-engine hardened-runtime entitlements for the
# macOS AUv3 .appex and its container .app.
#
# The entitlement templates under tools/templates/auv3/ carry two @ONLY
# placeholders this module fills before configure_file():
#
#   @PULP_AUV3_JIT_ENTITLEMENTS@
#       `com.apple.security.cs.allow-jit` — only when the configured JS engine
#       emits machine code at run time (PULP_JS_ENGINE is v8 or jsc). The
#       default QuickJS interpreter, Skia, and Dawn need no executable memory
#       of their own, so a QuickJS build carries no JIT entitlement at all.
#       `allow-unsigned-executable-memory` is never emitted: V8 and
#       JavaScriptCore allocate JIT pages with MAP_JIT, which `allow-jit`
#       covers, and the unsigned-memory entitlement would also license code
#       pages no engine asked for.
#
#   @PULP_AUV3_LIBRARY_VALIDATION_ENTITLEMENT@
#       `com.apple.security.cs.disable-library-validation` — only when
#       PULP_AUV3_DISABLE_LIBRARY_VALIDATION is ON. Library validation admits
#       code signed by Apple or by the same Team ID as the host process. The
#       shipped recipe signs the embedded AUv3Framework and every bundled
#       dylib inner-out with the one Developer ID identity, so validation
#       passes and the entitlement is unnecessary. Turn it on only when some
#       embedded code is signed by a different Team ID or carries no Team ID
#       at all: a vendor-signed prebuilt dylib you may not re-sign, or an
#       ad-hoc (`codesign -s -`) development build whose components have no
#       team identity. Leaving it on lets any signed-by-anyone dylib load into
#       the extension and the host.

option(PULP_AUV3_DISABLE_LIBRARY_VALIDATION
    "Emit com.apple.security.cs.disable-library-validation into the AUv3 appex/host entitlements. Needed only when an embedded framework or dylib is not signed with the appex's own Team ID (vendor-signed prebuilt, or ad-hoc dev signing)."
    OFF)

# Whether `engine` (a PULP_JS_ENGINE value) compiles JavaScript to machine
# code at run time and therefore needs com.apple.security.cs.allow-jit.
function(pulp_js_engine_needs_jit engine out_var)
    if(engine STREQUAL "v8" OR engine STREQUAL "jsc")
        set(${out_var} ON PARENT_SCOPE)
    else()
        set(${out_var} OFF PARENT_SCOPE)
    endif()
endfunction()

# Fill the two template placeholders in the caller's scope from the given
# engine and library-validation choice. Each value is either an empty string
# or a complete, indented `<key/><true/>` pair ready for a plist <dict>.
function(pulp_auv3_entitlement_fragments engine disable_library_validation)
    pulp_js_engine_needs_jit("${engine}" _needs_jit)
    set(_jit "")
    if(_needs_jit)
        set(_jit "    <key>com.apple.security.cs.allow-jit</key>\n    <true/>\n")
    endif()
    set(_lib "")
    if(disable_library_validation)
        set(_lib "    <key>com.apple.security.cs.disable-library-validation</key>\n    <true/>\n")
    endif()
    set(PULP_AUV3_JIT_ENTITLEMENTS "${_jit}" PARENT_SCOPE)
    set(PULP_AUV3_LIBRARY_VALIDATION_ENTITLEMENT "${_lib}" PARENT_SCOPE)
endfunction()
