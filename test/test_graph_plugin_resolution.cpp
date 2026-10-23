// GraphSerializer plugin-node resolution.
//
// A .pulpgraph names each plugin node's last known path. Loading that path
// directly would run whatever library the file points at, so from_json only
// loads a path that lies inside a known plugin root and lists anything else
// in missing_plugins without opening it. Re-finding a plugin by (format,
// unique_id) means enumerating every bundle in the roots, so it is an opt-in
// scan mode, never the default. The probe fixture writes a marker file when
// it is loaded, which is how these cases tell "refused" from "opened and
// failed".

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/scan_blacklist.hpp>
#include <pulp/host/scan_cache.hpp>
#include <pulp/host/signal_graph.hpp>

#include "support/unique_temp_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>

#ifndef PULP_SCAN_FIXTURE_PROBE_CLAP
#error "PULP_SCAN_FIXTURE_PROBE_CLAP must point at the probe CLAP fixture"
#endif
#ifndef PULP_SCAN_FIXTURE_ABORT_CLAP
#error "PULP_SCAN_FIXTURE_ABORT_CLAP must point at the aborting CLAP fixture"
#endif
#ifndef PULP_SCAN_WORKER_PATH
#error "PULP_SCAN_WORKER_PATH must point at the pulp-scan-worker binary"
#endif
#ifndef PULP_TEST_NO_EDITOR_CLAP
#error "PULP_TEST_NO_EDITOR_CLAP must point at the editor-less link-floor CLAP"
#endif

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using namespace pulp::host;

namespace {

constexpr const char* kProbeId = "dev.pulp.test.scan-probe";
constexpr const char* kNoEditorId = "dev.pulp.test.no-editor-link-floor";

struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const char* stem)
        : path(pulp::test::make_unique_temp_dir(std::string("pulp-graph-plugin-resolution-") +
                                                stem)) {}
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

class ScopedEnv {
  public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str()))
            previous_ = value;
    }
    ~ScopedEnv() {
        if (previous_)
            set(*previous_);
        else
            unset();
    }
    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

// A bundle is a flat file (the fixtures, Linux and Windows plugins) or a
// directory (a macOS bundle); copy either shape.
fs::path copy_bundle(const fs::path& source, const fs::path& directory) {
    fs::create_directories(directory);
    const auto destination = directory / source.filename();
    fs::copy(source, destination, fs::copy_options::recursive);
    return destination;
}

std::string graph_with_plugin(const std::string& unique_id, const std::string& last_path) {
    PluginInfo info;
    info.name = "Probe";
    info.manufacturer = "Pulp";
    info.version = "1.0.0";
    info.unique_id = unique_id;
    info.path = last_path;
    info.format = PluginFormat::CLAP;
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "In");
    const auto plugin = graph.add_unresolved_plugin_node(info, 2, 2, "Probe");
    const auto output = graph.add_output_node(2, "Out");
    REQUIRE(plugin != 0);
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    return GraphSerializer::to_json(graph);
}

const GraphNode* plugin_node(const SignalGraph& graph) {
    for (const auto& node : graph.nodes()) {
        if (node.type == NodeType::Plugin)
            return &node;
    }
    return nullptr;
}

// Every case keeps the load off the machine's installed plugins and points
// the identity scan at the worker this tree built.
GraphLoadOptions hermetic_options() {
    GraphLoadOptions options;
    options.include_default_plugin_roots = false;
    return options;
}

bool no_editor_clap_available() {
    std::error_code ec;
    return fs::exists(PULP_TEST_NO_EDITOR_CLAP, ec);
}

} // namespace

TEST_CASE("from_json refuses a plugin last_path outside the known roots and never opens it",
          "[host][graph][serializer][plugin-resolution]") {
    ScratchDir scratch("unrooted");
    ScopedEnv worker("PULP_SCAN_WORKER");
    worker.set(PULP_SCAN_WORKER_PATH);
    const auto outside = copy_bundle(PULP_SCAN_FIXTURE_PROBE_CLAP, scratch.path / "outside");
    const auto marker = scratch.path / "dlopen-marker";
    ScopedEnv marker_env("PULP_TEST_DLOPEN_MARKER");
    marker_env.set(marker.string());

    SignalGraph graph;
    const auto result = GraphSerializer::from_json(
        graph, graph_with_plugin(kProbeId, outside.string()), hermetic_options());

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.size() == 1);
    REQUIRE_THAT(result.missing_plugins.front(),
                 ContainsSubstring(std::string("clap:") + kProbeId));
    REQUIRE_THAT(result.missing_plugins.front(),
                 ContainsSubstring("outside the known plugin roots"));
    REQUIRE_FALSE(fs::exists(marker));

    // The node survives with no slot so connections keep their ids.
    const auto* node = plugin_node(graph);
    REQUIRE(node != nullptr);
    REQUIRE(node->plugin == nullptr);
    REQUIRE(node->plugin_info.unique_id == kProbeId);
    REQUIRE(node->plugin_info.path == outside.string());
}

TEST_CASE("allow_unrooted_plugin_paths opts a graph back into loading its last_path",
          "[host][graph][serializer][plugin-resolution]") {
    ScratchDir scratch("allow-path");
    ScopedEnv worker("PULP_SCAN_WORKER");
    worker.set(PULP_SCAN_WORKER_PATH);
    const auto outside = copy_bundle(PULP_SCAN_FIXTURE_PROBE_CLAP, scratch.path / "outside");
    const auto marker = scratch.path / "dlopen-marker";
    ScopedEnv marker_env("PULP_TEST_DLOPEN_MARKER");
    marker_env.set(marker.string());

    auto options = hermetic_options();
    options.allow_unrooted_plugin_paths = true;

    SECTION("the probe is opened: its marker proves the dlopen happened") {
        SignalGraph graph;
        const auto result = GraphSerializer::from_json(
            graph, graph_with_plugin(kProbeId, outside.string()), options);
        REQUIRE(result.ok);
        REQUIRE(fs::exists(marker));
        // The probe lists a descriptor but creates no plugin, so the load
        // itself fails; that failure is reported as such, not as unrooted.
        REQUIRE(result.missing_plugins.size() == 1);
        REQUIRE_THAT(result.missing_plugins.front(), ContainsSubstring("failed to load"));
    }

    SECTION("a real plugin outside every root loads") {
        if (!no_editor_clap_available()) {
            SKIP("editor-less link-floor CLAP not built in this configuration");
        }
        const auto moved = copy_bundle(PULP_TEST_NO_EDITOR_CLAP, scratch.path / "private");
        SignalGraph graph;
        const auto result = GraphSerializer::from_json(
            graph, graph_with_plugin(kNoEditorId, moved.string()), options);
        REQUIRE(result.ok);
        REQUIRE(result.missing_plugins.empty());
        const auto* node = plugin_node(graph);
        REQUIRE(node != nullptr);
        REQUIRE(node->plugin != nullptr);
        REQUIRE(node->plugin->is_loaded());
    }
}

TEST_CASE("a plugin inside a registered root loads through its last_path",
          "[host][graph][serializer][plugin-resolution]") {
    if (!no_editor_clap_available()) {
        SKIP("editor-less link-floor CLAP not built in this configuration");
    }
    ScopedEnv worker("PULP_SCAN_WORKER");
    worker.set(PULP_SCAN_WORKER_PATH);
    const fs::path bundle = PULP_TEST_NO_EDITOR_CLAP;
    auto options = hermetic_options();
    options.plugin_roots.push_back(bundle.parent_path().string());

    SignalGraph graph;
    const auto result =
        GraphSerializer::from_json(graph, graph_with_plugin(kNoEditorId, bundle.string()), options);

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.empty());
    const auto* node = plugin_node(graph);
    REQUIRE(node != nullptr);
    REQUIRE(node->plugin != nullptr);
    REQUIRE(node->plugin->is_loaded());
    REQUIRE(node->plugin->info().unique_id == kNoEditorId);
}

TEST_CASE("default options never enumerate the plugin roots",
          "[host][graph][serializer][plugin-resolution]") {
    STATIC_REQUIRE(std::is_same_v<decltype(GraphLoadOptions::scan_mode), GraphPluginScanMode>);
    REQUIRE(GraphLoadOptions{}.scan_mode == GraphPluginScanMode::none);
    REQUIRE(GraphLoadOptions{}.load_plugins);
    REQUIRE_FALSE(GraphLoadOptions{}.allow_unrooted_plugin_paths);

    // The probe sits in a registered root under a stale last_path, a worker
    // is configured, and the identity matches: everything a scan would need.
    // With the default scan mode the root is still never enumerated.
    ScratchDir scratch("no-scan");
    ScopedEnv worker("PULP_SCAN_WORKER");
    worker.set(PULP_SCAN_WORKER_PATH);
    const auto root = scratch.path / "root";
    copy_bundle(PULP_SCAN_FIXTURE_PROBE_CLAP, root);
    const auto marker = scratch.path / "dlopen-marker";
    ScopedEnv marker_env("PULP_TEST_DLOPEN_MARKER");
    marker_env.set(marker.string());
    auto options = hermetic_options();
    options.plugin_roots.push_back(root.string());

    SignalGraph graph;
    const auto result = GraphSerializer::from_json(
        graph, graph_with_plugin(kProbeId, "/nonexistent/Moved Elsewhere.clap"), options);

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.size() == 1);
    REQUIRE_THAT(result.missing_plugins.front(),
                 ContainsSubstring("scanning them for this identity is disabled"));
    REQUIRE_FALSE(fs::exists(marker));
    REQUIRE(plugin_node(graph)->plugin == nullptr);
}

TEST_CASE("isolated_only resolution reports missing when no worker exists and runs nothing here",
          "[host][graph][serializer][plugin-resolution]") {
    ScratchDir scratch("isolated-no-worker");
    ScopedEnv worker("PULP_SCAN_WORKER");
    worker.unset();
    const auto root = scratch.path / "root";
    copy_bundle(PULP_SCAN_FIXTURE_PROBE_CLAP, root);
    const auto marker = scratch.path / "dlopen-marker";
    ScopedEnv marker_env("PULP_TEST_DLOPEN_MARKER");
    marker_env.set(marker.string());
    auto options = hermetic_options();
    options.plugin_roots.push_back(root.string());
    options.scan_mode = GraphPluginScanMode::isolated_only;
    options.scan_worker_path = (scratch.path / "no-such-worker").string();

    SignalGraph graph;
    const auto result = GraphSerializer::from_json(
        graph, graph_with_plugin(kProbeId, "/nonexistent/Moved Elsewhere.clap"), options);

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.size() == 1);
    REQUIRE_THAT(result.missing_plugins.front(), ContainsSubstring("no scan worker found"));
    REQUIRE_FALSE(fs::exists(marker));

    SECTION("allow_in_process is the mode that runs the entry point here") {
        options.scan_mode = GraphPluginScanMode::allow_in_process;
        SignalGraph other;
        const auto in_process = GraphSerializer::from_json(
            other, graph_with_plugin(kProbeId, "/nonexistent/Moved Elsewhere.clap"), options);
        REQUIRE(in_process.ok);
        REQUIRE(fs::exists(marker));
        // The probe was found by identity; it then fails to instantiate.
        REQUIRE(in_process.missing_plugins.size() == 1);
        REQUIRE_THAT(in_process.missing_plugins.front(), ContainsSubstring("failed to load"));
    }
}

TEST_CASE("load_plugins=false keeps every plugin node unresolved without opening anything",
          "[host][graph][serializer][plugin-resolution]") {
    ScratchDir scratch("no-load");
    const auto outside = copy_bundle(PULP_SCAN_FIXTURE_PROBE_CLAP, scratch.path / "outside");
    const auto marker = scratch.path / "dlopen-marker";
    ScopedEnv marker_env("PULP_TEST_DLOPEN_MARKER");
    marker_env.set(marker.string());
    auto options = hermetic_options();
    options.load_plugins = false;
    // Even the loosest path policy does not override it.
    options.allow_unrooted_plugin_paths = true;
    options.plugin_roots.push_back(outside.parent_path().string());

    SignalGraph graph;
    const auto result =
        GraphSerializer::from_json(graph, graph_with_plugin(kProbeId, outside.string()), options);

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.size() == 1);
    REQUIRE_THAT(result.missing_plugins.front(), ContainsSubstring("loading is disabled"));
    REQUIRE_FALSE(fs::exists(marker));
    const auto* node = plugin_node(graph);
    REQUIRE(node != nullptr);
    REQUIRE(node->plugin == nullptr);
    REQUIRE(node->plugin_info.unique_id == kProbeId);
}

TEST_CASE("a rooted last_path whose bundle holds a different plugin is refused",
          "[host][graph][serializer][plugin-resolution]") {
    if (!no_editor_clap_available()) {
        SKIP("editor-less link-floor CLAP not built in this configuration");
    }
    const fs::path bundle = PULP_TEST_NO_EDITOR_CLAP;
    auto options = hermetic_options();
    options.plugin_roots.push_back(bundle.parent_path().string());

    SignalGraph graph;
    const auto result = GraphSerializer::from_json(
        graph, graph_with_plugin("dev.pulp.test.some-other-plugin", bundle.string()), options);

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.size() == 1);
    REQUIRE_THAT(result.missing_plugins.front(),
                 ContainsSubstring(std::string("holds plugin '") + kNoEditorId + "'"));
    const auto* node = plugin_node(graph);
    REQUIRE(node != nullptr);
    REQUIRE(node->plugin == nullptr);
}

TEST_CASE("a symlink inside a root that points outside it is refused and never opened",
          "[host][graph][serializer][plugin-resolution]") {
    ScratchDir scratch("symlink-escape");
    const auto outside = copy_bundle(PULP_SCAN_FIXTURE_PROBE_CLAP, scratch.path / "outside");
    const auto root = scratch.path / "root";
    fs::create_directories(root);
    const auto link = root / "Linked.clap";
    std::error_code ec;
    if (fs::is_directory(outside))
        fs::create_directory_symlink(outside, link, ec);
    else
        fs::create_symlink(outside, link, ec);
    if (ec)
        SKIP("cannot create a symlink here: " << ec.message());
    const auto marker = scratch.path / "dlopen-marker";
    ScopedEnv marker_env("PULP_TEST_DLOPEN_MARKER");
    marker_env.set(marker.string());
    auto options = hermetic_options();
    options.plugin_roots.push_back(root.string());

    SignalGraph graph;
    const auto result =
        GraphSerializer::from_json(graph, graph_with_plugin(kProbeId, link.string()), options);

    // The path is lexically inside the root; its real target is not.
    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.size() == 1);
    REQUIRE_THAT(result.missing_plugins.front(),
                 ContainsSubstring("outside the known plugin roots"));
    REQUIRE_FALSE(fs::exists(marker));
    REQUIRE(plugin_node(graph)->plugin == nullptr);
}

TEST_CASE("an identity scan records crashing bundles into the caller's own blacklist",
          "[host][graph][serializer][plugin-resolution][blacklist]") {
    ScratchDir scratch("blacklist-writeback");
    const auto root = scratch.path / "root";
    const auto aborting = copy_bundle(PULP_SCAN_FIXTURE_ABORT_CLAP, root);
    ScanBlacklist blacklist;
    auto options = hermetic_options();
    options.plugin_roots.push_back(root.string());
    options.scan_mode = GraphPluginScanMode::isolated_only;
    options.scan_worker_path = PULP_SCAN_WORKER_PATH;
    options.blacklist = &blacklist;

    SignalGraph graph;
    const auto result = GraphSerializer::from_json(
        graph, graph_with_plugin("dev.pulp.test.never-installed", "/nonexistent/Gone.clap"),
        options);

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.size() == 1);
    // The verdict reached the object the caller handed in, not a copy.
    REQUIRE(blacklist.is_blacklisted(aborting.string()));
    REQUIRE_THAT(blacklist.get(aborting.string())->reason, ContainsSubstring("abnormally"));
}

TEST_CASE("an identity scan serves unchanged bundles from the caller's scan cache without a worker",
          "[host][graph][serializer][plugin-resolution][scan-cache]") {
    if (!no_editor_clap_available()) {
        SKIP("editor-less link-floor CLAP not built in this configuration");
    }
    const fs::path bundle = PULP_TEST_NO_EDITOR_CLAP;
    HostScanCache cache;
    auto options = hermetic_options();
    options.plugin_roots.push_back(bundle.parent_path().string());
    options.scan_mode = GraphPluginScanMode::isolated_only;
    options.scan_worker_path = PULP_SCAN_WORKER_PATH;
    options.scan_cache = &cache;

    SignalGraph first;
    const auto warm = GraphSerializer::from_json(
        first, graph_with_plugin(kNoEditorId, "/nonexistent/Moved Elsewhere.clap"), options);
    REQUIRE(warm.ok);
    REQUIRE(warm.missing_plugins.empty());
    REQUIRE(cache.get(bundle.string()).has_value());

    // No worker exists for the second load; the cache alone resolves it.
    options.scan_worker_path = (bundle.parent_path() / "no-such-worker").string();
    SignalGraph second;
    const auto cached = GraphSerializer::from_json(
        second, graph_with_plugin(kNoEditorId, "/nonexistent/Moved Elsewhere.clap"), options);
    REQUIRE(cached.ok);
    REQUIRE(cached.missing_plugins.empty());
    REQUIRE(plugin_node(second)->plugin != nullptr);

    // The same load without the cache has nothing to answer with.
    options.scan_cache = nullptr;
    SignalGraph third;
    const auto uncached = GraphSerializer::from_json(
        third, graph_with_plugin(kNoEditorId, "/nonexistent/Moved Elsewhere.clap"), options);
    REQUIRE(uncached.ok);
    REQUIRE(uncached.missing_plugins.size() == 1);
    REQUIRE_THAT(uncached.missing_plugins.front(), ContainsSubstring("no scan worker found"));
}

TEST_CASE("a stale last_path is re-resolved by (format, unique_id) in the registered roots",
          "[host][graph][serializer][plugin-resolution]") {
    if (!no_editor_clap_available()) {
        SKIP("editor-less link-floor CLAP not built in this configuration");
    }
    ScopedEnv worker("PULP_SCAN_WORKER");
    worker.set(PULP_SCAN_WORKER_PATH);
    const fs::path bundle = PULP_TEST_NO_EDITOR_CLAP;
    auto options = hermetic_options();
    options.plugin_roots.push_back(bundle.parent_path().string());
    options.scan_mode = GraphPluginScanMode::isolated_only;

    SignalGraph graph;
    const auto result = GraphSerializer::from_json(
        graph, graph_with_plugin(kNoEditorId, "/nonexistent/Moved Elsewhere.clap"), options);

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.empty());
    const auto* node = plugin_node(graph);
    REQUIRE(node != nullptr);
    REQUIRE(node->plugin != nullptr);
    REQUIRE(node->plugin->info().unique_id == kNoEditorId);
    REQUIRE(fs::equivalent(node->plugin->info().path, bundle));

    SECTION("an identity nobody installed stays missing, with the reason") {
        SignalGraph other;
        const auto missing = GraphSerializer::from_json(
            other, graph_with_plugin("dev.pulp.test.never-installed", "/nonexistent/Gone.clap"),
            options);
        REQUIRE(missing.ok);
        REQUIRE(missing.missing_plugins.size() == 1);
        REQUIRE_THAT(missing.missing_plugins.front(),
                     ContainsSubstring("no installed plugin matches this identity"));
    }
}

TEST_CASE("a blacklisted last_path inside a root is reported missing, not loaded",
          "[host][graph][serializer][plugin-resolution][blacklist]") {
    if (!no_editor_clap_available()) {
        SKIP("editor-less link-floor CLAP not built in this configuration");
    }
    ScopedEnv worker("PULP_SCAN_WORKER");
    worker.set(PULP_SCAN_WORKER_PATH);
    const fs::path bundle = PULP_TEST_NO_EDITOR_CLAP;
    ScanBlacklist blacklist;
    blacklist.blacklist(bundle.string(), "crashed a previous scan");
    auto options = hermetic_options();
    options.plugin_roots.push_back(bundle.parent_path().string());
    options.blacklist = &blacklist;

    SignalGraph graph;
    const auto result =
        GraphSerializer::from_json(graph, graph_with_plugin(kNoEditorId, bundle.string()), options);

    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.size() == 1);
    REQUIRE_THAT(result.missing_plugins.front(), ContainsSubstring("blacklisted"));
    const auto* node = plugin_node(graph);
    REQUIRE(node != nullptr);
    REQUIRE(node->plugin == nullptr);
}
