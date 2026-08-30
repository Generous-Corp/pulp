# PulpDocumentTypes.cmake — declare the file types a Pulp app owns.
#
# `pulp_declare_standalone_document_type()` is called AFTER the app target
# exists (after `pulp_add_plugin(... FORMATS standalone)` or `pulp_add_app()`).
# That ordering is deliberate: a directory-scoped variable read from inside the
# target-creating call would have to be set before a line that does not mention
# it, which is an invisible rule. `MACOSX_BUNDLE_INFO_PLIST` may be set on an
# existing target at any point before generate, so a post-hoc function is both
# simpler and honest about when it takes effect.
#
# CMake cannot append keys to the Info.plist it synthesizes from the
# MACOSX_BUNDLE_* properties, so this configures `PulpInfoPlist.standalone.in`
# instead. That template carries CMake's own default key set verbatim, still
# spelled as `${MACOSX_BUNDLE_*}` placeholders — CMake configures the custom
# plist a SECOND time at generate time with those properties bound, so an app
# that adopts the template keeps every value it had (icon file included, no
# matter whether `pulp_app_icon()` ran before or after this function). Only the
# two document-type blocks are substituted here, with `@ONLY`.

# ── XML/plist escaping ──────────────────────────────────────────────────────
# `$` and `@` are escaped as numeric character references, which is not an XML
# requirement: the emitted plist is configured HERE and again by CMake at
# generate time, so a literal `${...}` or `@...@` inside a caller's DESCRIPTION
# would be eaten by one of those two passes. `&#36;` / `&#64;` survive both and
# decode back to the literal character when the plist is parsed.
function(_pulp_plist_escape out_var text)
    string(REPLACE "&" "&amp;" text "${text}")
    string(REPLACE "<" "&lt;" text "${text}")
    string(REPLACE ">" "&gt;" text "${text}")
    string(REPLACE "\"" "&quot;" text "${text}")
    string(REPLACE "@" "&#64;" text "${text}")
    string(REPLACE "$" "&#36;" text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# Emit `<array><string>…</string></array>` at the given tab depth.
function(_pulp_plist_string_array out_var indent)
    set(_pad "")
    foreach(_i RANGE 1 ${indent})
        string(APPEND _pad "\t")
    endforeach()
    set(_xml "${_pad}<array>\n")
    foreach(_value IN LISTS ARGN)
        _pulp_plist_escape(_escaped "${_value}")
        string(APPEND _xml "${_pad}\t<string>${_escaped}</string>\n")
    endforeach()
    string(APPEND _xml "${_pad}</array>\n")
    set(${out_var} "${_xml}" PARENT_SCOPE)
endfunction()

# ── pulp_declare_standalone_document_type ───────────────────────────────────
# Usage (call AFTER the target is created):
#
#   pulp_declare_standalone_document_type(MyApp_Standalone
#       UTI         com.example.myapp.project
#       EXTENSION   myproj
#       MIME        application/vnd.example.myapp+zip
#       DESCRIPTION "MyApp Project"
#       CONFORMS_TO public.data public.archive
#       ROLE        Editor      # Editor | Viewer | Shell | None
#       RANK        Owner)      # Owner | Default | Alternate | None
#
# Emits BOTH `CFBundleDocumentTypes` (what this app opens) and
# `UTExportedTypeDeclarations` (this app DEFINES the type). Declaring a
# document type for a UTI that nothing exports gets no document icon and
# unreliable Launch Services routing, so the pair is not separable here.
#
# May be called repeatedly for one target; each call adds a type. Arguments are
# validated on every platform so a typo fails the same way everywhere, but on
# non-Apple platforms the function then returns without touching the target —
# a cross-platform CMakeLists needs no `if(APPLE)` around the call.
function(pulp_declare_standalone_document_type target)
    cmake_parse_arguments(DOC
        ""
        "UTI;DESCRIPTION;ROLE;RANK"
        "EXTENSION;MIME;CONFORMS_TO"
        ${ARGN})

    set(_where "pulp_declare_standalone_document_type(${target})")

    if(DOC_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${_where}: unrecognized argument(s): ${DOC_UNPARSED_ARGUMENTS}")
    endif()
    if(DOC_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "${_where}: keyword(s) given with no value: ${DOC_KEYWORDS_MISSING_VALUES}")
    endif()

    # A document type that is merely ignored is the worst outcome available:
    # everything builds, the app installs, and double-click does nothing. Every
    # check below therefore fails configure rather than dropping the type.
    if(NOT DOC_UTI)
        message(FATAL_ERROR "${_where}: UTI is required (e.g. UTI com.example.myapp.project)")
    endif()
    if(NOT DOC_EXTENSION)
        message(FATAL_ERROR "${_where}: EXTENSION is required (e.g. EXTENSION myproj)")
    endif()
    # Launch Services matches on a reverse-DNS identifier; a bare word such as
    # `myproj` parses fine and then never routes.
    if(NOT DOC_UTI MATCHES "^[A-Za-z0-9][A-Za-z0-9+.-]*$" OR NOT DOC_UTI MATCHES "\\.")
        message(FATAL_ERROR
            "${_where}: UTI '${DOC_UTI}' is not a reverse-DNS identifier. "
            "Use something like com.example.myapp.project.")
    endif()

    set(_extensions "")
    foreach(_extension IN LISTS DOC_EXTENSION)
        # A leading dot is unambiguous intent, so normalize rather than reject.
        string(REGEX REPLACE "^\\." "" _extension "${_extension}")
        if(NOT _extension MATCHES "^[A-Za-z0-9][A-Za-z0-9_-]*$")
            message(FATAL_ERROR
                "${_where}: EXTENSION '${_extension}' is not a bare filename extension "
                "(letters, digits, '_' and '-'; no dot, no glob, no leading '*').")
        endif()
        list(APPEND _extensions "${_extension}")
    endforeach()

    if(NOT DOC_ROLE)
        set(DOC_ROLE "Editor")
    endif()
    if(NOT DOC_ROLE MATCHES "^(Editor|Viewer|Shell|None)$")
        message(FATAL_ERROR
            "${_where}: ROLE must be Editor, Viewer, Shell, or None (got '${DOC_ROLE}')")
    endif()

    if(NOT DOC_RANK)
        set(DOC_RANK "Owner")
    endif()
    # RANK None is the shape a secondary/helper app needs: it can open the type
    # but must never become the system default handler for it.
    if(NOT DOC_RANK MATCHES "^(Owner|Default|Alternate|None)$")
        message(FATAL_ERROR
            "${_where}: RANK must be Owner, Default, Alternate, or None (got '${DOC_RANK}')")
    endif()

    if(NOT DOC_DESCRIPTION)
        set(DOC_DESCRIPTION "${DOC_UTI}")
    endif()
    if(NOT DOC_CONFORMS_TO)
        set(DOC_CONFORMS_TO "public.data")
    endif()

    # Documented no-op: Windows/Linux/Android have no Info.plist to extend.
    if(NOT APPLE)
        return()
    endif()

    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "${_where}: no such target. Call this AFTER pulp_add_plugin()/pulp_add_app(), "
            "and name the app target (e.g. MyPlugin_Standalone, not MyPlugin).")
    endif()
    get_target_property(_target_type ${target} TYPE)
    get_target_property(_is_bundle ${target} MACOSX_BUNDLE)
    if(NOT _is_bundle)
        # An unset property reads back as <var>-NOTFOUND; report the state, not
        # CMake's sentinel.
        set(_is_bundle "FALSE")
    endif()
    if(NOT _target_type STREQUAL "EXECUTABLE" OR NOT _is_bundle)
        message(FATAL_ERROR
            "${_where}: target is not a macOS application bundle "
            "(TYPE=${_target_type}, MACOSX_BUNDLE=${_is_bundle}). Document types can only be "
            "declared on a bundled executable.")
    endif()

    set(_plist_dir "${CMAKE_BINARY_DIR}/PulpDocumentTypes")
    set(_plist "${_plist_dir}/${target}-Info.plist")

    # Refuse to silently overwrite a hand-authored plist — the caller's keys
    # would vanish, and nothing about the build would say so.
    get_target_property(_existing_plist ${target} MACOSX_BUNDLE_INFO_PLIST)
    if(_existing_plist AND NOT _existing_plist STREQUAL "${_plist}")
        message(FATAL_ERROR
            "${_where}: target already has MACOSX_BUNDLE_INFO_PLIST set to "
            "'${_existing_plist}'. Declare the document type in that plist directly, or drop "
            "the custom plist and let this function generate one.")
    endif()

    get_target_property(_declared_utis ${target} PULP_DOCUMENT_TYPE_UTIS)
    if(NOT _declared_utis)
        set(_declared_utis "")
    endif()
    if("${DOC_UTI}" IN_LIST _declared_utis)
        message(FATAL_ERROR
            "${_where}: UTI '${DOC_UTI}' is already declared on this target. "
            "Pass every extension of one type in a single call "
            "(EXTENSION a b), rather than declaring the UTI twice.")
    endif()
    list(APPEND _declared_utis "${DOC_UTI}")

    _pulp_plist_escape(_uti_xml "${DOC_UTI}")
    _pulp_plist_escape(_description_xml "${DOC_DESCRIPTION}")
    _pulp_plist_escape(_role_xml "${DOC_ROLE}")
    _pulp_plist_escape(_rank_xml "${DOC_RANK}")

    # ── CFBundleDocumentTypes entry: what this app opens ────────────────────
    _pulp_plist_string_array(_content_types_xml 4 "${DOC_UTI}")
    _pulp_plist_string_array(_type_extensions_xml 4 ${_extensions})
    set(_document_entry "\t\t<dict>\n")
    string(APPEND _document_entry "\t\t\t<key>CFBundleTypeName</key>\n")
    string(APPEND _document_entry "\t\t\t<string>${_description_xml}</string>\n")
    string(APPEND _document_entry "\t\t\t<key>CFBundleTypeRole</key>\n")
    string(APPEND _document_entry "\t\t\t<string>${_role_xml}</string>\n")
    string(APPEND _document_entry "\t\t\t<key>LSHandlerRank</key>\n")
    string(APPEND _document_entry "\t\t\t<string>${_rank_xml}</string>\n")
    string(APPEND _document_entry "\t\t\t<key>LSItemContentTypes</key>\n")
    string(APPEND _document_entry "${_content_types_xml}")
    # Legacy extension list kept alongside LSItemContentTypes: harmless on
    # current macOS, and still consulted by older open/save paths.
    string(APPEND _document_entry "\t\t\t<key>CFBundleTypeExtensions</key>\n")
    string(APPEND _document_entry "${_type_extensions_xml}")
    string(APPEND _document_entry "\t\t</dict>\n")

    # ── UTExportedTypeDeclarations entry: this app DEFINES the type ─────────
    _pulp_plist_string_array(_conforms_xml 4 ${DOC_CONFORMS_TO})
    _pulp_plist_string_array(_tag_extensions_xml 5 ${_extensions})
    set(_exported_entry "\t\t<dict>\n")
    string(APPEND _exported_entry "\t\t\t<key>UTTypeIdentifier</key>\n")
    string(APPEND _exported_entry "\t\t\t<string>${_uti_xml}</string>\n")
    string(APPEND _exported_entry "\t\t\t<key>UTTypeDescription</key>\n")
    string(APPEND _exported_entry "\t\t\t<string>${_description_xml}</string>\n")
    string(APPEND _exported_entry "\t\t\t<key>UTTypeConformsTo</key>\n")
    string(APPEND _exported_entry "${_conforms_xml}")
    string(APPEND _exported_entry "\t\t\t<key>UTTypeTagSpecification</key>\n")
    string(APPEND _exported_entry "\t\t\t<dict>\n")
    string(APPEND _exported_entry "\t\t\t\t<key>public.filename-extension</key>\n")
    string(APPEND _exported_entry "${_tag_extensions_xml}")
    if(DOC_MIME)
        _pulp_plist_string_array(_mime_xml 5 ${DOC_MIME})
        string(APPEND _exported_entry "\t\t\t\t<key>public.mime-type</key>\n")
        string(APPEND _exported_entry "${_mime_xml}")
    endif()
    string(APPEND _exported_entry "\t\t\t</dict>\n")
    string(APPEND _exported_entry "\t\t</dict>\n")

    # Accumulate as plain strings, never as a CMake list: a `;` anywhere in a
    # caller's DESCRIPTION would otherwise become an element separator.
    get_target_property(_document_entries ${target} PULP_DOCUMENT_TYPES_XML)
    if(NOT _document_entries)
        set(_document_entries "")
    endif()
    get_target_property(_exported_entries ${target} PULP_EXPORTED_TYPES_XML)
    if(NOT _exported_entries)
        set(_exported_entries "")
    endif()
    string(APPEND _document_entries "${_document_entry}")
    string(APPEND _exported_entries "${_exported_entry}")

    set_target_properties(${target} PROPERTIES
        PULP_DOCUMENT_TYPE_UTIS "${_declared_utis}"
        PULP_DOCUMENT_TYPES_XML "${_document_entries}"
        PULP_EXPORTED_TYPES_XML "${_exported_entries}")

    set(PULP_DOCUMENT_TYPES_BLOCK
        "\t<key>CFBundleDocumentTypes</key>\n\t<array>\n${_document_entries}\t</array>\n")
    set(PULP_EXPORTED_TYPES_BLOCK
        "\t<key>UTExportedTypeDeclarations</key>\n\t<array>\n${_exported_entries}\t</array>\n")

    file(MAKE_DIRECTORY "${_plist_dir}")
    # CMAKE_CURRENT_FUNCTION_LIST_DIR (not CMAKE_CURRENT_LIST_DIR, which inside a
    # function body resolves to the CALLER's directory) so the template resolves
    # both in the source tree and in an installed SDK's lib/cmake/Pulp.
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpInfoPlist.standalone.in"
        "${_plist}"
        @ONLY)
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${_plist}")
endfunction()
