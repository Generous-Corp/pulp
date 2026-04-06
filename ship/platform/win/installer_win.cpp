#include <pulp/ship/installer.hpp>

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <filesystem>

namespace pulp::ship {

std::string generate_nsis_script(const InstallerConfig& config) {
    std::ostringstream nsi;

    // Determine install directories
    std::string install_root = config.per_user_install
        ? "$LOCALAPPDATA" : "$PROGRAMFILES";
    std::string vst3_dir = config.per_user_install
        ? "$LOCALAPPDATA\\Programs\\Common\\VST3"
        : "$COMMONFILES\\VST3";
    std::string clap_dir = config.per_user_install
        ? "$LOCALAPPDATA\\Programs\\Common\\CLAP"
        : "$COMMONFILES\\CLAP";

    nsi << "!include \"MUI2.nsh\"\n\n";

    // Metadata
    nsi << "Name \"" << config.product_name << "\"\n";
    nsi << "OutFile \"" << config.output_path << "\"\n";
    nsi << "InstallDir \"" << install_root << "\\" << config.publisher
        << "\\" << config.product_name << "\"\n";
    std::string reg_hive = config.per_user_install ? "HKCU" : "HKLM";
    nsi << "InstallDirRegKey " << reg_hive << " \"Software\\" << config.publisher
        << "\\" << config.product_name << "\" \"\"\n";

    if (config.per_user_install) {
        nsi << "RequestExecutionLevel user\n";
    } else {
        nsi << "RequestExecutionLevel admin\n";
    }

    nsi << "\n";

    // Version info
    nsi << "!define PRODUCT_NAME \"" << config.product_name << "\"\n";
    nsi << "!define PRODUCT_VERSION \"" << config.version << "\"\n";
    nsi << "!define PRODUCT_PUBLISHER \"" << config.publisher << "\"\n";
    if (!config.url.empty()) {
        nsi << "!define PRODUCT_WEB_SITE \"" << config.url << "\"\n";
    }
    nsi << "\n";

    // Icon
    if (!config.icon_path.empty()) {
        nsi << "!define MUI_ICON \"" << config.icon_path << "\"\n";
        nsi << "!define MUI_UNICON \"" << config.icon_path << "\"\n\n";
    }

    // Pages
    if (!config.license_path.empty()) {
        nsi << "!insertmacro MUI_PAGE_LICENSE \"" << config.license_path << "\"\n";
    }
    nsi << "!insertmacro MUI_PAGE_DIRECTORY\n";
    nsi << "!insertmacro MUI_PAGE_COMPONENTS\n";
    nsi << "!insertmacro MUI_PAGE_INSTFILES\n";
    nsi << "!insertmacro MUI_PAGE_FINISH\n\n";

    // Uninstaller pages
    nsi << "!insertmacro MUI_UNPAGE_CONFIRM\n";
    nsi << "!insertmacro MUI_UNPAGE_INSTFILES\n\n";

    // Language
    nsi << "!insertmacro MUI_LANGUAGE \"English\"\n\n";

    // Sections for each plugin format
    for (const auto& plugin : config.plugins) {
        std::string section_name;
        std::string out_dir;

        if (plugin.format == "vst3") {
            section_name = config.product_name + " (VST3)";
            out_dir = vst3_dir;
        } else if (plugin.format == "clap") {
            section_name = config.product_name + " (CLAP)";
            out_dir = clap_dir;
        } else if (plugin.format == "standalone") {
            section_name = config.product_name + " (Standalone)";
            out_dir = "$INSTDIR";
        } else {
            section_name = config.product_name + " (" + plugin.format + ")";
            out_dir = "$INSTDIR";
        }

        nsi << "Section \"" << section_name << "\"\n";
        nsi << "  SetOutPath \"" << out_dir << "\"\n";

        // Check if source is a directory (bundle) or single file
        namespace fs = std::filesystem;
        if (fs::is_directory(plugin.source_path)) {
            nsi << "  File /r \"" << plugin.source_path << "\"\n";
        } else {
            nsi << "  File \"" << plugin.source_path << "\"\n";
        }

        nsi << "SectionEnd\n\n";
    }

    // Uninstaller section
    nsi << "Section \"Uninstall\"\n";
    for (const auto& plugin : config.plugins) {
        std::string out_dir;
        if (plugin.format == "vst3") {
            out_dir = vst3_dir;
        } else if (plugin.format == "clap") {
            out_dir = clap_dir;
        } else {
            out_dir = "$INSTDIR";
        }

        namespace fs = std::filesystem;
        auto filename = fs::path(plugin.source_path).filename().string();
        nsi << "  RMDir /r \"" << out_dir << "\\" << filename << "\"\n";
    }
    nsi << "  Delete \"$INSTDIR\\uninstall.exe\"\n";
    nsi << "  RMDir \"$INSTDIR\"\n";
    nsi << "  DeleteRegKey " << reg_hive << " \"Software\\" << config.publisher
        << "\\" << config.product_name << "\"\n";
    nsi << "  DeleteRegKey " << reg_hive << " \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << config.product_name << "\"\n";
    nsi << "SectionEnd\n\n";

    // Write uninstaller during install
    nsi << "Section \"-Post\"\n";
    nsi << "  WriteUninstaller \"$INSTDIR\\uninstall.exe\"\n";
    nsi << "  WriteRegStr " << reg_hive << " \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << config.product_name << "\" \"DisplayName\" \"" << config.product_name << "\"\n";
    nsi << "  WriteRegStr " << reg_hive << " \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << config.product_name << "\" \"UninstallString\" \"$INSTDIR\\uninstall.exe\"\n";
    nsi << "  WriteRegStr " << reg_hive << " \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << config.product_name << "\" \"DisplayVersion\" \"" << config.version << "\"\n";
    nsi << "  WriteRegStr " << reg_hive << " \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        << config.product_name << "\" \"Publisher\" \"" << config.publisher << "\"\n";
    nsi << "SectionEnd\n";

    return nsi.str();
}

bool create_nsis_installer(const InstallerConfig& config) {
    // Generate the NSIS script
    auto script = generate_nsis_script(config);

    // Write to a temporary .nsi file
    namespace fs = std::filesystem;
    auto nsi_path = fs::path(config.output_path).parent_path() / (config.product_name + ".nsi");
    {
        std::ofstream out(nsi_path);
        if (!out) return false;
        out << script;
    }

    // Invoke makensis
    std::string cmd = "makensis /V2 \"" + nsi_path.string() + "\"";
    int result = std::system(cmd.c_str());

    // Clean up the .nsi file
    fs::remove(nsi_path);

    return result == 0;
}

} // namespace pulp::ship
