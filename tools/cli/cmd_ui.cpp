// `pulp ui` — deterministic owned-source UI build/check seam.
//
// The source emitter is intentionally kept in a dependency-free Python tool
// while the UI compiler/runtime contract is still being defined. Keeping the
// command in the primary CLI makes the future emitter a stable replacement
// behind one user-facing surface, without coupling this command to Skia or a
// JavaScript package manager.

#include "cli_common.hpp"

#include <iostream>

int cmd_ui(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: pulp ui <build|check> [options]\n\n"
                     "Build or verify a deterministic owned UI source snapshot.\n"
                     "  build  emit source files and ui-build-manifest.json\n"
                     "  check  verify source/output bytes against the manifest\n\n"
                     "Options:\n"
                     "  --source <dir>  source root (default: native-ui/src)\n"
                     "  --out <dir>     output root (default: build/native-ui)\n"
                     "  --json          emit a machine-readable receipt\n";
        return 0;
    }
    return delegate_to_python_script("tools/ui-build/ui_build.py", args);
}
