// cmd_doctor.cpp — pulp doctor command

#include "cli_common.hpp"

#include <iostream>

int cmd_doctor(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto active_root = resolve_active_project_root(&standalone_mode);
    auto root = standalone_mode ? fs::path{} : active_root;

    bool fix_mode = false;
    bool ci_mode = false;
    bool dry_run = false;
    for (auto& arg : args) {
        if (arg == "--fix") fix_mode = true;
        if (arg == "--ci") ci_mode = true;
        if (arg == "--dry-run") dry_run = true;
    }

    if (!ci_mode) {
        std::cout << color::bold() << "Pulp Doctor" << color::reset() << "\n";
        std::cout << "===========\n\n";
        if (standalone_mode && !active_root.empty()) {
            std::cout << color::dim() << "(SDK mode project — checking system tools for an installed SDK workflow)" << color::reset() << "\n\n";
        } else if (root.empty()) {
            std::cout << color::dim() << "(Not in a Pulp project — checking system tools only)" << color::reset() << "\n\n";
        } else {
            std::cout << color::dim() << "(Source-tree mode — checking the active Pulp checkout)" << color::reset() << "\n\n";
        }
    }

    auto checks = run_doctor_checks(active_root, standalone_mode);

    int pass_count = 0, fail_count = 0;
    for (auto& c : checks) {
        if (c.passed) {
            ++pass_count;
            if (!ci_mode) {
                std::string msg = c.name;
                if (!c.detail.empty()) msg += " — " + c.detail;
                print_ok(msg);
            }
        } else {
            ++fail_count;
            if (ci_mode) {
                std::cerr << "FAIL: " << c.name;
                if (!c.detail.empty()) std::cerr << " — " << c.detail;
                if (!c.fix.empty()) std::cerr << " [fix: " << c.fix << "]";
                std::cerr << "\n";
            } else {
                std::string msg = c.name;
                if (!c.detail.empty()) msg += " — " + c.detail;
                print_fail(msg);
                if (!c.fix.empty()) {
                    if (fix_mode && !dry_run) {
                        std::cout << "    " << color::cyan() << "Fixing:" << color::reset() << " " << c.fix << "\n";
                        int rc = std::system(c.fix.c_str());
                        if (rc == 0) {
                            print_ok("Fixed");
                            --fail_count;
                            ++pass_count;
                        } else {
                            std::cout << "    Fix failed (exit " << rc << "). Run manually:\n";
                            std::cout << "      " << color::yellow() << c.fix << color::reset() << "\n";
                        }
                    } else if (dry_run) {
                        std::cout << "    " << color::dim() << "[dry-run] Would run: " << c.fix << color::reset() << "\n";
                    } else {
                        std::cout << "    Fix: " << color::yellow() << c.fix << color::reset() << "\n";
                    }
                }
            }
        }
    }

    if (!ci_mode) {
        std::cout << "\n  " << color::bold() << pass_count << "/" << (pass_count + fail_count)
                  << " checks passed" << color::reset();
        if (fail_count > 0) {
            std::cout << " — run " << color::cyan() << "`pulp doctor --fix`" << color::reset() << " to resolve";
        }
        std::cout << "\n";
    }

    return fail_count > 0 ? 1 : 0;
}
