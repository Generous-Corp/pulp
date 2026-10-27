#!/usr/bin/env bash
# Path lists handed to native Windows LLVM tools.
#
# MSYS bash rewrites POSIX paths in argv and path-like environment variables
# when it starts a native Windows program, but it never rewrites the contents
# of a file. llvm-profdata --input-files and llvm-cov @response files are read
# by the native tool itself, so a /d/a/... entry reaches it untranslated and
# fails with "no such file or directory". coverage_native_path_list rewrites
# one path per line from stdin into mixed form (D:/a/...) under MSYS, and
# passes the lines through unchanged everywhere else.

coverage_native_path_list() {
    if [[ -n "${MSYSTEM:-}" ]] && command -v cygpath >/dev/null 2>&1; then
        cygpath -m -f -
    else
        cat
    fi
}
