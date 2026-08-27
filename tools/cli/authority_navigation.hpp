#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::authority {

enum class Context { source, installed };

struct NativeRoute {
    std::string source;
    std::optional<std::string> installed;
};

struct Authority {
    std::string id;
    std::vector<std::string> aliases;
    std::string plane;
    std::string native_owner;
    std::string source_location;
    std::optional<std::string> installed_location;
    NativeRoute query_or_validator;
    std::string coverage_semantics;
    std::string absence_semantics;
    std::vector<std::string> does_not_prove;
};

struct ResolvedRegistry {
    std::filesystem::path path;
    std::filesystem::path schema_path;
    std::filesystem::path context_root;
    Context context = Context::source;
    std::string resolution_source;
    std::optional<std::string> sdk_version;
};

struct Registry {
    int revision = 0;
    std::string sha256;
    ResolvedRegistry resolved;
    std::vector<Authority> authorities;
};

struct LoadResult {
    std::optional<Registry> registry;
    std::string error;
};

std::optional<ResolvedRegistry> resolve_registry(const std::filesystem::path& cwd,
                                                 const std::filesystem::path& executable,
                                                 const std::filesystem::path& selected_sdk = {},
                                                 const std::string& selected_sdk_version = {},
                                                 const std::string& selected_sdk_source = {});
LoadResult load_registry(const ResolvedRegistry& resolved);
const Authority* find(const Registry& registry, const std::string& token);
std::string render_list(const Registry& registry, bool json);
std::optional<std::string> render_query(const Registry& registry, const std::string& token,
                                        bool json);

} // namespace pulp::cli::authority
