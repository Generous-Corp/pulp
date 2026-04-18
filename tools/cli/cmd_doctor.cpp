// cmd_doctor.cpp — pulp doctor command

#include "cli_common.hpp"

#include <iostream>

int cmd_doctor(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto active_root = resolve_active_project_root(&standalone_mode);
    auto root = standalone_mode ? fs::path{} : active_root;

    // First positional argument selects a sub-mode for mobile dev-env
    // checks (#8 / #355). `pulp doctor` (no positional) keeps the
    // existing universal checks; `pulp doctor android` and
    // `pulp doctor ios` are the per-mobile-target lanes.
    std::string mode;  // empty = default
    bool fix_mode = false;
    bool ci_mode = false;
    bool dry_run = false;
    for (auto& arg : args) {
        if (arg == "--fix") fix_mode = true;
        else if (arg == "--ci") ci_mode = true;
        else if (arg == "--dry-run") dry_run = true;
        else if (arg.rfind("--", 0) == 0) {
            std::cerr << "pulp doctor: unknown flag: " << arg << "\n";
            std::cerr << "Usage: pulp doctor [android|ios] [--fix] [--ci] [--dry-run]\n";
            return 2;
        } else if (mode.empty()) {
            mode = arg;
        }
    }

    if (!mode.empty() && mode != "android" && mode != "ios") {
        std::cerr << "pulp doctor: unknown subcommand '" << mode << "'\n";
        std::cerr << "Usage: pulp doctor [android|ios] [--fix] [--ci] [--dry-run]\n";
        return 2;
    }

    if (!ci_mode) {
        std::cout << color::bold();
        if (mode == "android")    std::cout << "Pulp Doctor — Android";
        else if (mode == "ios")   std::cout << "Pulp Doctor — iOS";
        else                      std::cout << "Pulp Doctor";
        std::cout << color::reset() << "\n";
        std::cout << "===========\n\n";
        if (mode.empty()) {
            if (standalone_mode && !active_root.empty()) {
                std::cout << color::dim() << "(SDK mode project — checking system tools for an installed SDK workflow)" << color::reset() << "\n\n";
            } else if (root.empty()) {
                std::cout << color::dim() << "(Not in a Pulp project — checking system tools only)" << color::reset() << "\n\n";
            } else {
                std::cout << color::dim() << "(Source-tree mode — checking the active Pulp checkout)" << color::reset() << "\n\n";
            }
        } else {
            std::cout << color::dim()
                      << "(mobile dev-env check — install hints follow each fail)"
                      << color::reset() << "\n\n";
        }
    }

    std::vector<DoctorCheck> checks;
    if (mode == "android")    checks = run_doctor_android_checks();
    else if (mode == "ios")   checks = run_doctor_ios_checks();
    else                      checks = run_doctor_checks(active_root, standalone_mode);

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
