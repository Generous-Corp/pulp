// cmd_projects.cpp — `pulp projects {list,add,remove}` commands.
//
// Issue #499 / #552 Slice 1b. Thin CLI-side wrapper over
// projects_registry.{hpp,cpp}. The registry is also written to from
// `cmd_create` on successful scaffold; these commands exist so users
// can manage projects created outside `pulp create` (clones, manual
// checkouts) and prune stale entries.

#include "cli_common.hpp"
#include "projects_registry.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace prjreg = pulp::cli::projects_registry;

namespace {

void print_help() {
    std::cout <<
        "pulp projects — manage the ~/.pulp/projects.json registry\n\n"
        "Usage:\n"
        "  pulp projects list               Show registered projects\n"
        "  pulp projects add [<path>]       Register a project (defaults to CWD)\n"
        "  pulp projects remove <path>      Remove a project by path\n"
        "\n"
        "The registry is authoritative. `pulp create` registers new\n"
        "projects automatically on successful scaffold. `pulp doctor\n"
        "--versions --scan-parents` does an opt-in ancestor walk and\n"
        "surfaces projects that aren't registered without mutating the\n"
        "registry — that's a diagnostic escape hatch, not the default.\n";
}

int do_list() {
    auto reg = prjreg::registry_path();
    auto projects = prjreg::read_registry(reg);

    std::cout << "Registry: " << reg.string() << "\n";
    if (projects.empty()) {
        std::cout << "  (no projects registered)\n"
                  << "  Run `pulp projects add` in a project directory to register it,\n"
                  << "  or use `pulp doctor --versions --scan-parents` to surface\n"
                  << "  ancestors without modifying the registry.\n";
        return 0;
    }

    std::cout << "  " << projects.size() << " project"
              << (projects.size() == 1 ? "" : "s") << ":\n";
    for (const auto& p : projects) {
        bool missing = !fs::exists(p.path);
        std::cout << "  - " << (p.name.empty() ? p.path.filename().string() : p.name);
        if (missing) std::cout << " " << color::yellow() << "(missing)" << color::reset();
        std::cout << "\n      " << color::dim() << p.path.string() << color::reset();
        if (!p.registered_at.empty()) {
            std::cout << "  registered " << p.registered_at;
        }
        std::cout << "\n";
    }
    return 0;
}

int do_add(const std::vector<std::string>& args) {
    fs::path target;
    if (args.empty()) {
        target = fs::current_path();
    } else {
        target = args[0];
        if (target.is_relative()) target = fs::current_path() / target;
    }

    if (!fs::exists(target)) {
        std::cerr << "pulp projects add: path does not exist: " << target.string() << "\n";
        return 1;
    }
    if (!fs::is_directory(target)) {
        std::cerr << "pulp projects add: path is not a directory: " << target.string() << "\n";
        return 1;
    }

    // Name hint: CMake project(NAME) if we can read it, else basename.
    std::string name;
    std::string sdk_ver = read_project_cmake_version(target);
    (void)sdk_ver;
    name = target.filename().string();

    auto reg = prjreg::registry_path();
    prjreg::add_project(reg, target, name);
    std::cout << color::green() << "Registered" << color::reset()
              << " " << name << " at " << target.string() << "\n";
    return 0;
}

int do_remove(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "pulp projects remove: a path argument is required\n";
        return 1;
    }
    fs::path target = args[0];
    if (target.is_relative()) target = fs::current_path() / target;

    auto reg = prjreg::registry_path();
    if (prjreg::remove_project(reg, target)) {
        std::cout << color::green() << "Removed" << color::reset()
                  << " " << target.string() << "\n";
        return 0;
    }
    std::cerr << "pulp projects remove: not found in registry: " << target.string() << "\n";
    return 1;
}

}  // namespace

int cmd_projects(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_help();
        return args.empty() ? 1 : 0;
    }

    const auto& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "list" || sub == "ls") return do_list();
    if (sub == "add")                 return do_add(rest);
    if (sub == "remove" || sub == "rm") return do_remove(rest);

    std::cerr << "pulp projects: unknown subcommand '" << sub << "'\n";
    print_help();
    return 2;
}
