// `pulp ui` — deterministic owned-source UI build/check/lint seam.
//
// The source emitter is a dependency-free Python tool. Inside a checkout that
// carries tools/ui-build it runs from there; everywhere else `pulp ui`
// forwards its arguments unchanged to `vellum ui`, which owns the tool.

#include "cli_common.hpp"

#include <iostream>

int cmd_ui(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout
            << "Usage: pulp ui <build|check|lint> [options]\n\n"
               "Build, verify, or lint a deterministic owned UI source snapshot.\n"
               "  build  emit source files and ui-build-manifest.json\n"
               "  check  verify source/output bytes against the manifest\n"
               "  lint   check semantic source and captured corpus output\n\n"
               "Options:\n"
               "  --source <dir>  source root (default: native-ui/src)\n"
               "  --out <dir>     output root (default: build/native-ui)\n"
               "  --manifest <file>  corpus manifest (lint only)\n"
               "  --enforce-size  enforce advisory component/function size limits (lint only)\n"
               "  --json          emit a machine-readable receipt\n";
        return 0;
    }
    // A Pulp checkout that still carries the tool runs it; otherwise the
    // command belongs to the Vellum CLI, which accepts the same arguments.
    const fs::path script = "tools/ui-build/ui_build.py";
    const auto root = find_project_root();
    if (!root.empty() && fs::exists(root / script)) {
        return delegate_to_python_script(script, args);
    }
    return forward_to_vellum("ui", "ui", args);
}
