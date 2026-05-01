#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/system_volume.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#if defined(__linux__)
#include <sys/stat.h>
#endif

using Catch::Matchers::WithinAbs;

namespace {

#if defined(__linux__)

std::filesystem::path make_temp_root(const std::string& name) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                (name + "-" + std::to_string(stamp));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name_)) old_ = std::string(old);
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }

    ~ScopedEnvVar() {
#ifdef _WIN32
        _putenv_s(name_, old_ ? old_->c_str() : "");
#else
        if (old_) setenv(name_, old_->c_str(), 1);
        else unsetenv(name_);
#endif
    }

private:
    const char* name_;
    std::optional<std::string> old_;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

std::string prepend_path(const std::filesystem::path& dir) {
    std::string value = dir.string();
    if (const char* old = std::getenv("PATH")) {
        value += ':';
        value += old;
    }
    return value;
}

void write_fake_amixer(const std::filesystem::path& dir) {
    const auto script = dir / "amixer";
    std::ofstream f(script);
    f << "#!/bin/sh\n"
         "if [ -n \"$PULP_FAKE_AMIXER_LOG\" ]; then\n"
         "  printf '%s\\n' \"$*\" >> \"$PULP_FAKE_AMIXER_LOG\"\n"
         "fi\n"
         "if [ \"$1\" = \"sget\" ]; then\n"
         "  if [ \"$PULP_FAKE_AMIXER_MODE\" = \"empty\" ]; then exit 0; fi\n"
         "  if [ \"$PULP_FAKE_AMIXER_MODE\" = \"fail\" ]; then exit 3; fi\n"
         "  if [ \"$PULP_FAKE_AMIXER_MODE\" = \"malformed\" ]; then\n"
         "    echo 'Simple mixer control Master has no bracketed value'\n"
         "    exit 0\n"
         "  fi\n"
         "  echo '  Front Left: Playback 37 [37%] [-12.00dB] [on]'\n"
         "  exit 0\n"
         "fi\n"
         "if [ \"$1\" = \"sset\" ]; then\n"
         "  if [ \"$PULP_FAKE_AMIXER_MODE\" = \"fail\" ]; then exit 4; fi\n"
         "  exit 0\n"
         "fi\n"
         "exit 2\n";
    f.close();
    chmod(script.c_str(), 0755);
}

#endif

} // namespace

TEST_CASE("system volume Linux path parses amixer output",
          "[audio][system-volume]") {
#if defined(__linux__)
    auto root = make_temp_root("pulp-system-volume");
    write_fake_amixer(root);

    ScopedEnvVar path("PATH", prepend_path(root));
    ScopedEnvVar mode("PULP_FAKE_AMIXER_MODE", "");

    auto volume = pulp::audio::get_system_volume();
    REQUIRE(volume.has_value());
    REQUIRE_THAT(*volume, WithinAbs(0.37f, 0.001f));

    std::filesystem::remove_all(root);
#else
    SUCCEED("Linux amixer path is not active on this platform.");
#endif
}

TEST_CASE("system volume Linux path rejects empty or malformed amixer output",
          "[audio][system-volume]") {
#if defined(__linux__)
    auto root = make_temp_root("pulp-system-volume-empty");
    write_fake_amixer(root);

    ScopedEnvVar path("PATH", prepend_path(root));
    {
        ScopedEnvVar mode("PULP_FAKE_AMIXER_MODE", "empty");
        REQUIRE_FALSE(pulp::audio::get_system_volume().has_value());
    }
    {
        ScopedEnvVar mode("PULP_FAKE_AMIXER_MODE", "malformed");
        REQUIRE_FALSE(pulp::audio::get_system_volume().has_value());
    }
    {
        ScopedEnvVar mode("PULP_FAKE_AMIXER_MODE", "fail");
        REQUIRE_FALSE(pulp::audio::get_system_volume().has_value());
    }

    std::filesystem::remove_all(root);
#else
    SUCCEED("Linux amixer path is not active on this platform.");
#endif
}

TEST_CASE("system volume Linux setter invokes amixer with integer percent",
          "[audio][system-volume]") {
#if defined(__linux__)
    auto root = make_temp_root("pulp-system-volume-set");
    write_fake_amixer(root);
    const auto log = root / "amixer.log";

    ScopedEnvVar path("PATH", prepend_path(root));
    ScopedEnvVar log_path("PULP_FAKE_AMIXER_LOG", log.string());

    {
        ScopedEnvVar mode("PULP_FAKE_AMIXER_MODE", "");
        REQUIRE(pulp::audio::set_system_volume(0.42f));
        REQUIRE(read_file(log).find("sset Master 42%") != std::string::npos);
    }
    {
        ScopedEnvVar mode("PULP_FAKE_AMIXER_MODE", "fail");
        REQUIRE_FALSE(pulp::audio::set_system_volume(0.5f));
    }

    std::filesystem::remove_all(root);
#else
    SUCCEED("Linux amixer path is not active on this platform.");
#endif
}

TEST_CASE("system mute API reports unsupported on Linux",
          "[audio][system-volume]") {
#if defined(__linux__)
    REQUIRE_FALSE(pulp::audio::is_system_muted().has_value());
    REQUIRE_FALSE(pulp::audio::set_system_muted(true));
    REQUIRE_FALSE(pulp::audio::set_system_muted(false));
#else
    SUCCEED("Linux mute stubs are not active on this platform.");
#endif
}
