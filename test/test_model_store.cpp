// Generic pulp::runtime model registry/store — proves a non-audio consumer
// can link the runtime header and that list/install/activate round-trips, with
// subsystem isolation. (The audio CLI's behavior is covered by test_audio_tools.cpp.)
#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/model_registry.hpp>
#include <pulp/runtime/model_store.hpp>

#include <filesystem>
#include <fstream>
#include <vector>

using namespace pulp::runtime;
namespace fs = std::filesystem;

TEST_CASE("resolve_checkpoint_url: hf:// resolution + path hardening", "[runtime][model]") {
    REQUIRE(resolve_checkpoint_url("hf://user/repo/file.pt") ==
            "https://huggingface.co/user/repo/resolve/main/file.pt");
    REQUIRE(resolve_checkpoint_url("hf://user/repo/path/to/w.mlxfn") ==
            "https://huggingface.co/user/repo/resolve/main/path/to/w.mlxfn");
    REQUIRE(resolve_checkpoint_url("https://example.com/m.bin") == "https://example.com/m.bin");
    REQUIRE(resolve_checkpoint_url("hf://user/repo/../escape").empty());   // `..` in file_path rejected
    REQUIRE(resolve_checkpoint_url("hf://user/../evil/file.pt").empty());  // `..` in user_repo rejected
    REQUIRE(resolve_checkpoint_url("hf://user/repo/a/../b.pt").empty());   // nested `..` rejected
    REQUIRE(resolve_checkpoint_url("hf://user/repo/%2e%2e/x.pt").empty()); // encoded `..` rejected
    REQUIRE(resolve_checkpoint_url("hf://%2e%2e/repo/x.pt").empty());      // encoded `..` in user_repo
    REQUIRE(resolve_checkpoint_url("hf://user/repo").empty());             // no file part
    REQUIRE(resolve_checkpoint_url("ftp://x/y").empty());                  // unsupported scheme
}

TEST_CASE("model path builders reject traversal in subsystem/model_id", "[runtime][model]") {
    const fs::path home = fs::temp_directory_path() / "pulp-mm-pathguard";
    // Safe literals resolve normally.
    REQUIRE(model_state_path("magenta", home) == home / "magenta" / "model-state.json");
    REQUIRE(model_install_path("magenta", "m1", home) == home / "magenta" / "models" / "m1.json");

    // Unsafe subsystem values are rejected (empty path returned).
    REQUIRE(model_state_path("..", home).empty());
    REQUIRE(model_state_path("../secrets", home).empty());
    REQUIRE(model_state_path("a/b", home).empty());
    REQUIRE(model_state_path("", home).empty());
    REQUIRE(model_install_path("..", "m1", home).empty());
    REQUIRE(model_install_path("magenta", "../etc", home).empty());
    REQUIRE(model_install_path("magenta", "a/b", home).empty());
    REQUIRE(model_install_path("magenta", "", home).empty());
#ifdef _WIN32
    REQUIRE(model_install_path("magenta", "a\\b", home).empty());  // backslash separator
    REQUIRE(model_state_path("c:evil", home).empty());             // drive-relative
#endif
}

TEST_CASE("remove_model: return value reflects what was actually removed", "[runtime][model]") {
    const fs::path home = fs::temp_directory_path() / "pulp-mm-remove";
    fs::remove_all(home);
    std::string err;

    // Nothing installed → no-op, returns false (no error).
    REQUIRE_FALSE(remove_model("magenta", "ghost", err, home));
    REQUIRE(err.empty());

    // Invalid subsystem/model_id → false with an error, never touches the FS.
    REQUIRE_FALSE(remove_model("..", "m1", err, home));
    REQUIRE_FALSE(err.empty());
    err.clear();
    REQUIRE_FALSE(remove_model("magenta", "../escape", err, home));
    REQUIRE_FALSE(err.empty());

    // Something present → removes it and returns true.
    const fs::path meta = model_install_path("magenta", "m1", home);
    fs::create_directories(meta.parent_path());
    std::ofstream(meta) << R"({"model_id":"m1"})";
    REQUIRE(fs::exists(meta));
    err.clear();
    REQUIRE(remove_model("magenta", "m1", err, home));
    REQUIRE(err.empty());
    REQUIRE_FALSE(fs::exists(meta));

    fs::remove_all(home);
}

TEST_CASE("generic model store: list / install / activate round-trip + isolation",
          "[runtime][model]") {
    const fs::path home = fs::temp_directory_path() / "pulp-mm-pr1-test";
    fs::remove_all(home);

    std::vector<ModelEntry> registry = {
        ModelEntry{.model_id = "m1", .display_name = "Model One", .backend = "mlx",
                   .checkpoint_ref = "hf://a/b/w.mlxfn"},
    };

    // Nothing installed yet.
    auto before = list_models(registry, "magenta", home);
    REQUIRE(before.error.empty());
    REQUIRE(before.models.size() == 1);
    REQUIRE(before.models[0].status == "not_installed");
    REQUIRE_FALSE(before.models[0].active);
    REQUIRE(read_active_model_id("magenta", home).empty());

    // Simulate an install: a checkpoint file + the per-model install metadata.
    const fs::path ckpt = home / "magenta" / "models" / "w.mlxfn";
    fs::create_directories(ckpt.parent_path());
    std::ofstream(ckpt) << "weights-bytes";

    const fs::path meta = model_install_path("magenta", "m1", home);
    REQUIRE(meta == home / "magenta" / "models" / "m1.json");
    fs::create_directories(meta.parent_path());
    std::ofstream(meta) << R"({"model_id":"m1","backend":"mlx","checkpoint_ref":"hf://a/b/w.mlxfn",)"
                        << R"("resolved_checkpoint_path":")" << ckpt.generic_string() << R"("})";

    auto inst = read_installed_model("magenta", "m1", home);
    REQUIRE(inst.metadata_found);
    REQUIRE(inst.checkpoint_exists);
    REQUIRE(inst.loadable());

    auto act = activate_model(registry, "magenta", "m1", home);
    INFO("activate error: " << act.error);
    REQUIRE(act.ok);
    REQUIRE(act.active_model_id == "m1");
    REQUIRE(read_active_model_id("magenta", home) == "m1");

    auto after = list_models(registry, "magenta", home);
    REQUIRE(after.models[0].status == "installed");
    REQUIRE(after.models[0].active);

    // Subsystem isolation: the "audio" subsystem shares the home but sees no active model.
    REQUIRE(read_active_model_id("audio", home).empty());

    // Activating an unknown / uninstalled model fails cleanly.
    REQUIRE_FALSE(activate_model(registry, "magenta", "nope", home).ok);

    fs::remove_all(home);
}
