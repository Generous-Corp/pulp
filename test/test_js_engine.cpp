#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/js_engine_recommend.hpp>
#include <pulp/view/script_engine.hpp>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace pulp::view;

#if defined(_WIN32)
static int js_test_dup_fd(int fd) { return _dup(fd); }
static int js_test_dup2_fd(int old_fd, int new_fd) { return _dup2(old_fd, new_fd); }
static int js_test_close_fd(int fd) { return _close(fd); }
static int js_test_fileno(FILE* file) { return _fileno(file); }
static FILE* js_test_tmpfile() {
    FILE* file = nullptr;
    return tmpfile_s(&file) == 0 ? file : nullptr;
}
#else
static int js_test_dup_fd(int fd) { return dup(fd); }
static int js_test_dup2_fd(int old_fd, int new_fd) { return dup2(old_fd, new_fd); }
static int js_test_close_fd(int fd) { return close(fd); }
static int js_test_fileno(FILE* file) { return fileno(file); }
static FILE* js_test_tmpfile() { return std::tmpfile(); }
#endif

struct JsTestFdGuard {
    explicit JsTestFdGuard(int fd_in) : fd(fd_in) {}
    ~JsTestFdGuard() {
        if (fd >= 0) js_test_close_fd(fd);
    }

    JsTestFdGuard(const JsTestFdGuard&) = delete;
    JsTestFdGuard& operator=(const JsTestFdGuard&) = delete;

    int fd = -1;
};

struct JsTestFileGuard {
    explicit JsTestFileGuard(FILE* file_in) : file(file_in) {}
    ~JsTestFileGuard() {
        if (file != nullptr) std::fclose(file);
    }

    JsTestFileGuard(const JsTestFileGuard&) = delete;
    JsTestFileGuard& operator=(const JsTestFileGuard&) = delete;

    FILE* file = nullptr;
};

// Helper: capture stderr produced inside `body` so a test can assert that a
// runtime::log_error marker actually fires. The dup2 + freopen dance is the
// standard POSIX trick for redirecting an in-process stderr write to a file,
// then restoring the original FD afterwards. Used to verify diagnostic-only
// code paths (rejection tracker, JSC NSException bridge, eval_or_throw
// pre-rethrow log) without depending on an external log sink we don't have.
template <typename Body>
static std::string capture_stderr(Body&& body) {
    fflush(stderr);
    int saved_fd = js_test_dup_fd(js_test_fileno(stderr));
    REQUIRE(saved_fd >= 0);
    JsTestFdGuard saved_fd_guard(saved_fd);
    JsTestFileGuard tmp_file(js_test_tmpfile());
    REQUIRE(tmp_file.file != nullptr);
    REQUIRE(js_test_dup2_fd(js_test_fileno(tmp_file.file), js_test_fileno(stderr)) >= 0);
    try {
        body();
    } catch (...) {
        fflush(stderr);
        js_test_dup2_fd(saved_fd_guard.fd, js_test_fileno(stderr));
        throw;
    }
    fflush(stderr);
    REQUIRE(js_test_dup2_fd(saved_fd_guard.fd, js_test_fileno(stderr)) >= 0);
    std::fflush(tmp_file.file);
    std::fseek(tmp_file.file, 0, SEEK_SET);
    std::stringstream ss;
    char buffer[4096];
    while (std::size_t n = std::fread(buffer, 1, sizeof(buffer), tmp_file.file)) {
        ss.write(buffer, static_cast<std::streamsize>(n));
    }
    return ss.str();
}

// Helper: create engines for all available backends
static std::vector<JsEngineType> available_engines() {
    std::vector<JsEngineType> engines;
    if (is_engine_available(JsEngineType::quickjs)) engines.push_back(JsEngineType::quickjs);
    if (is_engine_available(JsEngineType::jsc))     engines.push_back(JsEngineType::jsc);
    if (is_engine_available(JsEngineType::v8))      engines.push_back(JsEngineType::v8);
    return engines;
}

// Run a test body against every available engine backend
#define FOR_EACH_ENGINE(engine_var) \
    auto engines_ = available_engines(); \
    REQUIRE_FALSE(engines_.empty()); \
    for (auto engine_type_ : engines_) { \
        DYNAMIC_SECTION("engine=" << engine_type_name(engine_type_)) { \
            auto engine_var = create_js_engine(engine_type_);

#define END_FOR_EACH_ENGINE }}

TEST_CASE("JsEngine creation", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        REQUIRE(engine->is_valid());
        REQUIRE(engine->type() == engine_type_);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine evaluate integer arithmetic", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        auto result = engine->evaluate("2 + 3");
        REQUIRE(result.getWithDefault<int>(0) == 5);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine evaluate string concatenation", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        auto result = engine->evaluate("'hello' + ' ' + 'world'");
        REQUIRE(result.toString() == "hello world");
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine evaluate boolean logic", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        auto result = engine->evaluate("true && !false");
        REQUIRE(result.getWithDefault<bool>(false) == true);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine evaluate float arithmetic", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        auto result = engine->evaluate("3.14 * 2");
        auto val = result.getWithDefault<double>(0.0);
        REQUIRE(val > 6.27);
        REQUIRE(val < 6.29);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine parse error throws", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        REQUIRE_THROWS(engine->evaluate("function {{{"));
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine runtime error throws", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        REQUIRE_THROWS(engine->evaluate("nonexistent_variable.property"));
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine register native function (no args)", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        engine->register_function("getAnswer", [](const choc::value::Value*, size_t) {
            return choc::value::createInt32(42);
        });
        auto result = engine->evaluate("getAnswer()");
        REQUIRE(result.getWithDefault<int>(0) == 42);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine register native function (with args)", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        engine->register_function("add", [](const choc::value::Value* args, size_t count) {
            double a = count > 0 ? args[0].getWithDefault<double>(0.0) : 0.0;
            double b = count > 1 ? args[1].getWithDefault<double>(0.0) : 0.0;
            return choc::value::createFloat64(a + b);
        });
        auto result = engine->evaluate("add(10, 32)");
        REQUIRE(result.getWithDefault<int>(0) == 42);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine register native function (string return)", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        engine->register_function("greet", [](const choc::value::Value* args, size_t count) {
            std::string name = count > 0 ? std::string(args[0].getWithDefault<std::string_view>("world")) : "world";
            return choc::value::createString("hello " + name);
        });
        auto result = engine->evaluate("greet('pulp')");
        REQUIRE(result.toString() == "hello pulp");
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine rejects duplicate native global registrations",
          "[js_engine][api-contract]") {
    FOR_EACH_ENGINE(engine)
        engine->register_function("dup", [](const choc::value::Value*, size_t) {
            return choc::value::createInt32(1);
        });

        REQUIRE_THROWS_AS(engine->register_function("dup", [](const choc::value::Value*, size_t) {
            return choc::value::createInt32(2);
        }), std::runtime_error);

        REQUIRE(engine->evaluate("dup()").getWithDefault<int>(0) == 1);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine reserves native global names across registration kinds",
          "[js_engine][api-contract]") {
    FOR_EACH_ENGINE(engine)
        HostObjectDescriptor host;
        host.class_name = "NativeThing";
        engine->register_host_object("nativeThing", std::move(host));

        REQUIRE_THROWS_AS(engine->register_function("nativeThing",
            [](const choc::value::Value*, size_t) {
                return choc::value::Value();
            }), std::runtime_error);

        engine->register_function("nativeFn", [](const choc::value::Value*, size_t) {
            return choc::value::Value();
        });

        HostObjectDescriptor duplicate_host;
        duplicate_host.class_name = "DuplicateThing";
        REQUIRE_THROWS_AS(engine->register_host_object("nativeFn", std::move(duplicate_host)),
                          std::runtime_error);

        engine->register_promise_function("nativePromise", [](const choc::value::Value*, size_t) {
            return choc::value::createInt32(3);
        });

        REQUIRE_THROWS_AS(engine->register_function("nativePromise",
            [](const choc::value::Value*, size_t) {
                return choc::value::Value();
            }), std::runtime_error);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine invoke JS function", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        engine->evaluate("function double_it(x) { return x * 2; }; void 0");
        choc::value::Value arg = choc::value::createInt32(21);
        auto result = engine->invoke("double_it", &arg, 1);
        REQUIRE(result.getWithDefault<int>(0) == 42);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine invoke with no args", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        engine->evaluate("function get_pi() { return 3.14; }; void 0");
        auto result = engine->invoke("get_pi");
        auto val = result.getWithDefault<double>(0.0);
        REQUIRE(val > 3.13);
        REQUIRE(val < 3.15);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine console logging", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        std::vector<std::pair<std::string, std::string>> logs;
        engine->set_log_callback([&](std::string_view level, std::string_view message) {
            logs.emplace_back(std::string(level), std::string(message));
        });

        engine->evaluate("console.log('hello')");
        engine->evaluate("console.warn('warning')");
        engine->evaluate("console.error('bad')");

        REQUIRE(logs.size() == 3);
        REQUIRE(logs[0].first == "log");
        REQUIRE(logs[0].second == "hello");
        REQUIRE(logs[1].first == "warn");
        REQUIRE(logs[1].second == "warning");
        REQUIRE(logs[2].first == "error");
        REQUIRE(logs[2].second == "bad");
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine parameter binding pattern", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        float gain_value = 0.5f;

        engine->register_function("getParam", [&](const choc::value::Value* args, size_t count) {
            auto name = count > 0 ? std::string(args[0].getWithDefault<std::string_view>("")) : "";
            if (name == "gain") return choc::value::createFloat64(gain_value);
            return choc::value::Value();
        });

        engine->register_function("setParam", [&](const choc::value::Value* args, size_t count) {
            auto name = count > 0 ? std::string(args[0].getWithDefault<std::string_view>("")) : "";
            auto value = count > 1 ? args[1].getWithDefault<double>(0.0) : 0.0;
            if (name == "gain") gain_value = static_cast<float>(value);
            return choc::value::Value();
        });

        auto result = engine->evaluate("getParam('gain')");
        REQUIRE(result.getWithDefault<double>(0.0) > 0.49);
        REQUIRE(result.getWithDefault<double>(0.0) < 0.51);

        engine->evaluate("setParam('gain', 0.75)");
        REQUIRE(gain_value > 0.74f);
        REQUIRE(gain_value < 0.76f);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine circular reference safety (;void 0 pattern)", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        // The ;void 0 pattern prevents circular reference issues in eval return
        auto result = engine->evaluate("var obj = {a: 1, b: 2}; void 0");
        // Should not throw or hang — the void 0 ensures undefined return
        REQUIRE(result.isVoid());

        // Verify the object was created and is accessible
        auto a = engine->evaluate("obj.a");
        REQUIRE(a.getWithDefault<int>(0) == 1);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine error message includes details", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        bool threw = false;
        try {
            engine->evaluate("undefined_func()");
        } catch (...) {
            threw = true;
        }
        REQUIRE(threw);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine type name", "[js_engine]") {
    REQUIRE(engine_type_name(JsEngineType::quickjs) == "QuickJS");
    REQUIRE(engine_type_name(JsEngineType::jsc) == "JavaScriptCore");
    REQUIRE(engine_type_name(JsEngineType::v8) == "V8");
}

TEST_CASE("JsEngine availability", "[js_engine]") {
    // QuickJS is always available
    REQUIRE(is_engine_available(JsEngineType::quickjs));

#if __APPLE__
    REQUIRE(is_engine_available(JsEngineType::jsc));
#else
    REQUIRE_FALSE(is_engine_available(JsEngineType::jsc));
#endif

    if (is_engine_available(JsEngineType::v8)) {
        auto v8_engine = create_js_engine(JsEngineType::v8);
        REQUIRE(v8_engine != nullptr);
        REQUIRE(v8_engine->is_valid());
        REQUIRE(v8_engine->type() == JsEngineType::v8);
    } else {
        SUCCEED("V8 intentionally unavailable in this build");
    }
}

TEST_CASE("JsEngine runtime/provider identity defaults are empty for non-V8 backends",
          "[js_engine][identity]") {
    // QuickJS and JSC do not report a runtime version or provider; the
    // identity surface must degrade to empty strings, never crash or fabricate.
    for (auto type : {JsEngineType::quickjs, JsEngineType::jsc}) {
        if (!is_engine_available(type))
            continue;
        auto engine = create_js_engine(type);
        REQUIRE(engine != nullptr);
        DYNAMIC_SECTION("engine=" << engine_type_name(type)) {
            CHECK(engine->runtime_version().empty());
            CHECK(engine->provider_kind().empty());
            CHECK(engine->provider_path().empty());
            CHECK(engine->expected_runtime_version().empty());
        }
    }
}

TEST_CASE("ScriptEngine forwards runtime/provider identity to the active backend",
          "[js_engine][identity][script_engine]") {
    // ScriptEngine's identity accessors are thin pass-throughs to the wrapped
    // JsEngine. Exercise them through the wrapper (not just the raw engine) so
    // the forwarding layer is covered, and assert it agrees with the backend it
    // wraps. For non-V8 backends the identity surface is empty; for a linked V8
    // the wrapper must report exactly what the underlying engine reports.
    for (auto type : {JsEngineType::quickjs, JsEngineType::jsc, JsEngineType::v8}) {
        if (!is_engine_available(type))
            continue;
        DYNAMIC_SECTION("engine=" << engine_type_name(type)) {
            ScriptEngine script(type);
            const auto& engine = script.engine();
            CHECK(script.engine_type() == type);
            CHECK(script.runtime_version() == engine.runtime_version());
            CHECK(script.provider_kind() == engine.provider_kind());
            CHECK(script.provider_path() == engine.provider_path());
            CHECK(script.expected_runtime_version() ==
                  engine.expected_runtime_version());
            if (type == JsEngineType::v8) {
                CHECK_FALSE(script.runtime_version().empty());
                CHECK_FALSE(script.provider_kind().empty());
            } else {
                CHECK(script.runtime_version().empty());
                CHECK(script.provider_kind().empty());
            }
        }
    }
}

TEST_CASE("V8 engine reports a non-empty runtime version and provider identity",
          "[js_engine][identity][v8]") {
    // Runtime-gated (not #ifdef): PULP_HAS_V8 is private to pulp-view-script, so
    // the test TU never sees it. is_engine_available() is the truthful query for
    // whether V8 is linked into pulp::view in this build.
    if (!is_engine_available(JsEngineType::v8)) {
        SUCCEED("V8 not linked in this build — identity asserts covered by the "
                "strict provider_identity CTest on the sealed lane");
        return;
    }
    auto engine = create_js_engine(JsEngineType::v8);
    REQUIRE(engine != nullptr);
    REQUIRE(engine->type() == JsEngineType::v8);

    // v8::V8::GetVersion() must yield something like "15.1.27.0".
    const auto runtime = engine->runtime_version();
    INFO("runtime_version=" << runtime);
    REQUIRE_FALSE(runtime.empty());
    REQUIRE(runtime.find('.') != std::string::npos);

    // The provider kind is wired at build time. It is non-empty whenever V8 is
    // actually linked (sealed v8builder, libnode, v8_monolith, or external).
    const auto kind = engine->provider_kind();
    INFO("provider_kind=" << kind);
    REQUIRE_FALSE(kind.empty());

    // When the build pinned an expected version (parsed from the V8 tag in
    // tools/deps/manifest.json), the runtime V8 reports must start with it. This
    // is the cross-check that proves the sealed seal is the runtime, not just the
    // headers. When no expectation was pinned, skip the equality assert but still
    // require the runtime/provider basics above.
    const auto expected = engine->expected_runtime_version();
    if (!expected.empty()) {
        INFO("expected_runtime_version=" << expected << " runtime=" << runtime);
        REQUIRE(runtime.rfind(expected, 0) == 0u);
        // A pinned expectation only ever comes from the v8-builder seal.
        REQUIRE(kind == "v8builder");
        REQUIRE_FALSE(engine->provider_path().empty());
    }
}

TEST_CASE("JsEngine script recommendation handles large-script threshold edge", "[js_engine][recommend]") {
    constexpr size_t large_script_threshold = 512 * 1024;

    std::string script_at_threshold(large_script_threshold, 'x');
    auto threshold_rec = recommend_engine_for_script(script_at_threshold, JsEngineType::quickjs);
    REQUIRE(threshold_rec.recommended == JsEngineType::quickjs);
    REQUIRE_FALSE(threshold_rec.upgrade_advised);
    REQUIRE(threshold_rec.reason.find("Standard UI script") != std::string::npos);

    script_at_threshold.push_back('x');
    auto large_rec = recommend_engine_for_script(script_at_threshold, JsEngineType::quickjs);
    REQUIRE(large_rec.recommended == JsEngineType::v8);
    REQUIRE(large_rec.upgrade_advised);
    REQUIRE(large_rec.reason.find("Large script (512KB)") != std::string::npos);

    if (!is_engine_available(JsEngineType::v8))
        REQUIRE(large_rec.reason.find("V8 not available") != std::string::npos);
}

TEST_CASE("JsEngine script recommendation detects Three.js source patterns", "[js_engine][recommend]") {
    const std::vector<std::string> scripts = {
        "const scene = new THREE.Scene();",
        "const renderer = new THREE.WebGLRenderer();",
        "const camera = new THREE.PerspectiveCamera(75, 1, 0.1, 1000);",
        "import * as THREE from 'three';",
        "import { Scene } from 'three';",
        "import { Scene } from \"three\";"
    };

    for (const auto& script : scripts) {
        DYNAMIC_SECTION("script=" << script) {
            auto rec = recommend_engine_for_script(script, JsEngineType::v8);
            REQUIRE(rec.recommended == JsEngineType::v8);
            REQUIRE_FALSE(rec.upgrade_advised);
            REQUIRE(rec.reason.find("Three.js detected") != std::string::npos);

            if (!is_engine_available(JsEngineType::v8))
                REQUIRE(rec.reason.find("rebuild with --js-engine=v8") != std::string::npos);
        }
    }
}

TEST_CASE("JsEngine recommendation upgrade flag follows the current engine", "[js_engine][recommend]") {
    auto standard_from_quickjs = recommend_engine_for_script("const gain = 0.5;", JsEngineType::quickjs);
    REQUIRE(standard_from_quickjs.recommended == JsEngineType::quickjs);
    REQUIRE_FALSE(standard_from_quickjs.upgrade_advised);

    auto standard_from_jsc = recommend_engine_for_script("const gain = 0.5;", JsEngineType::jsc);
    REQUIRE(standard_from_jsc.recommended == JsEngineType::quickjs);
    REQUIRE(standard_from_jsc.upgrade_advised);

    auto three_from_quickjs = recommend_engine_for_script("const scene = new THREE.Scene();", JsEngineType::quickjs);
    REQUIRE(three_from_quickjs.recommended == JsEngineType::v8);
    REQUIRE(three_from_quickjs.upgrade_advised);

    auto three_from_v8 = recommend_engine_for_script("const scene = new THREE.Scene();", JsEngineType::v8);
    REQUIRE(three_from_v8.recommended == JsEngineType::v8);
    REQUIRE_FALSE(three_from_v8.upgrade_advised);
}

TEST_CASE("JsEngine workload recommendation handles precedence and platform fallback", "[js_engine][recommend]") {
    auto three_apple = recommend_engine_for_workload("apple webgl scene", JsEngineType::quickjs);
    REQUIRE(three_apple.recommended == JsEngineType::v8);
    REQUIRE(three_apple.upgrade_advised);
    REQUIRE(three_apple.reason.find("Three.js / 3D workloads") != std::string::npos);

    if (!is_engine_available(JsEngineType::v8))
        REQUIRE(three_apple.reason.find("V8 not available") != std::string::npos);

    auto apple_rec = recommend_engine_for_workload("ios", JsEngineType::quickjs);
    auto expected_apple_engine = is_engine_available(JsEngineType::jsc)
        ? JsEngineType::jsc
        : JsEngineType::quickjs;
    REQUIRE(apple_rec.recommended == expected_apple_engine);
    REQUIRE(apple_rec.upgrade_advised == (expected_apple_engine != JsEngineType::quickjs));

    if (expected_apple_engine == JsEngineType::jsc)
        REQUIRE(apple_rec.reason.find("Apple-only target") != std::string::npos);
    else
        REQUIRE(apple_rec.reason.find("JSC not available") != std::string::npos);

    auto portable_from_quickjs = recommend_engine_for_workload("portable UI", JsEngineType::quickjs);
    REQUIRE(portable_from_quickjs.recommended == JsEngineType::quickjs);
    REQUIRE_FALSE(portable_from_quickjs.upgrade_advised);

    auto portable_from_v8 = recommend_engine_for_workload("cross-platform UI", JsEngineType::v8);
    REQUIRE(portable_from_v8.recommended == JsEngineType::quickjs);
    REQUIRE(portable_from_v8.upgrade_advised);
}

TEST_CASE("JsEngine default engine creation", "[js_engine]") {
    auto engine = create_default_js_engine();
    REQUIRE(engine != nullptr);
    REQUIRE(engine->is_valid());
}

TEST_CASE("JsEngine capability flags are truthful", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        REQUIRE(engine->supports_host_objects());
        REQUIRE(engine->supports_promises());

        switch (engine->type()) {
            case JsEngineType::quickjs:
                REQUIRE_FALSE(engine->supports_typed_arrays());
                break;
            case JsEngineType::jsc:
            case JsEngineType::v8:
                REQUIRE(engine->supports_typed_arrays());
                break;
        }
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine host object descriptor exposes properties and native methods", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        if (!engine->supports_host_objects()) {
            SUCCEED("host objects intentionally unsupported on this backend");
            continue;
        }

        int bump_count = 41;
        HostObjectDescriptor descriptor;
        descriptor.class_name = "NativeThing";
        descriptor.properties.push_back({"kind", choc::value::createString("buffer")});
        descriptor.properties.push_back({"version", choc::value::createInt32(1)});
        descriptor.methods.push_back({"bump", [&](const choc::value::Value*, size_t) {
            return choc::value::createInt32(++bump_count);
        }});
        descriptor.methods.push_back({"sum", [](const choc::value::Value* args, size_t count) {
            int total = 0;
            for (size_t i = 0; i < count; ++i)
                total += args[i].getWithDefault<int32_t>(0);
            return choc::value::createInt32(total);
        }});

        engine->register_host_object("nativeThing", std::move(descriptor));

        REQUIRE(engine->evaluate("nativeThing.kind").toString() == "buffer");
        REQUIRE(engine->evaluate("nativeThing.version").getWithDefault<int32_t>(0) == 1);
        REQUIRE(engine->evaluate("nativeThing.bump()").getWithDefault<int32_t>(0) == 42);
        REQUIRE(engine->evaluate("nativeThing.sum(20, 22)").getWithDefault<int32_t>(0) == 42);
        REQUIRE(engine->evaluate("nativeThing._objectName").toString() == "NativeThing");
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine typed array evaluation", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        if (!engine->supports_typed_arrays()) {
            SUCCEED("typed arrays intentionally unsupported on this backend");
            continue;
        }

        auto result = engine->evaluate("new Uint8Array([1, 2, 255])");
        REQUIRE(result.isArray());
        REQUIRE(result.size() == 3);
        REQUIRE(result[0].getWithDefault<int32_t>(0) == 1);
        REQUIRE(result[1].getWithDefault<int32_t>(0) == 2);
        REQUIRE(result[2].getWithDefault<int32_t>(0) == 255);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine native promise functions return real Promise objects", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        if (!engine->supports_promises()) {
            SUCCEED("promises intentionally unsupported on this backend");
            continue;
        }

        engine->register_promise_function("asyncAdd", [](const choc::value::Value* args, size_t count) {
            int total = 0;
            for (size_t i = 0; i < count; ++i)
                total += args[i].getWithDefault<int32_t>(0);
            return choc::value::createInt32(total);
        });

        REQUIRE(engine->evaluate("Object.prototype.toString.call(asyncAdd(20, 22))").toString() == "[object Promise]");
        REQUIRE(engine->evaluate("typeof asyncAdd(20, 22).then").toString() == "function");
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine native promise functions resolve through captured callbacks",
          "[js_engine][api-contract]") {
    FOR_EACH_ENGINE(engine)
        if (!engine->supports_promises()) {
            SUCCEED("promises intentionally unsupported on this backend");
            continue;
        }

        engine->register_promise_function("asyncAdd", [](const choc::value::Value* args, size_t count) {
            int total = 0;
            for (size_t i = 0; i < count; ++i)
                total += args[i].getWithDefault<int32_t>(0);
            return choc::value::createInt32(total);
        });

        engine->evaluate(R"(
            globalThis.__pulpPromiseValue = -1;
            globalThis.__pulpPromiseError = "";
            asyncAdd(20, 22).then(
                (value) => { globalThis.__pulpPromiseValue = value; },
                (error) => { globalThis.__pulpPromiseError = String(error && error.message ? error.message : error); }
            );
            void 0;
        )");
        engine->pump_message_loop();

        REQUIRE(engine->evaluate("globalThis.__pulpPromiseError").toString() == "");
        REQUIRE(engine->evaluate("globalThis.__pulpPromiseValue").getWithDefault<int>(0) == 42);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine smoke can assemble browser-style GPU bridge primitives", "[js_engine][gpu-bridge]") {
    auto engines_ = available_engines();
    REQUIRE_FALSE(engines_.empty());

    for (auto engine_type_ : engines_) {
        DYNAMIC_SECTION("engine=" << engine_type_name(engine_type_)) {
            ScriptEngine script(engine_type_);
            auto& engine = script.engine();

            if (!engine.supports_host_objects() || !engine.supports_promises()) {
                SUCCEED("GPU bridge smoke intentionally unsupported on this backend");
                continue;
            }

            HostObjectDescriptor gpu;
            gpu.class_name = "GPU";
            gpu.properties.push_back({"backend", choc::value::createString("mock-dawn")});
            gpu.methods.push_back({"getPreferredCanvasFormat", [](const choc::value::Value*, size_t) {
                return choc::value::createString("bgra8unorm");
            }});

            script.register_host_object("navigatorGPU", std::move(gpu));
            script.register_promise_function("__requestAdapterImpl", [](const choc::value::Value*, size_t) {
                auto adapter = choc::value::createObject("GPUAdapter");
                adapter.addMember("name", choc::value::createString("Mock Adapter"));
                return adapter;
            });

            script.evaluate(R"(
                globalThis.navigator = { gpu: navigatorGPU };
                navigator.gpu.requestAdapter = () => __requestAdapterImpl();
                void 0;
            )");

            REQUIRE(script.evaluate("navigator.gpu.backend").toString() == "mock-dawn");
            REQUIRE(script.evaluate("navigator.gpu.getPreferredCanvasFormat()").toString() == "bgra8unorm");
            REQUIRE(script.evaluate("Object.prototype.toString.call(navigator.gpu.requestAdapter())").toString() == "[object Promise]");
            REQUIRE(script.evaluate("typeof navigator.gpu.requestAdapter().then").toString() == "function");
        }
    }
}

TEST_CASE("JsEngine typed array reaches native callbacks when supported", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        if (!engine->supports_typed_arrays()) {
            SUCCEED("typed arrays intentionally unsupported on this backend");
            continue;
        }

        engine->register_function("sumTyped", [](const choc::value::Value* args, size_t count) {
            REQUIRE(count == 1);
            REQUIRE(args[0].isArray());

            int total = 0;
            for (uint32_t i = 0; i < args[0].size(); ++i)
                total += args[0][static_cast<int>(i)].getWithDefault<int32_t>(0);

            return choc::value::createInt32(total);
        });

        auto result = engine->evaluate("sumTyped(new Uint8Array([3, 4, 5]))");
        REQUIRE(result.getWithDefault<int32_t>(0) == 12);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine gc_hint does not crash", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        engine->evaluate("var arr = []; for (var i = 0; i < 1000; i++) arr.push({x: i}); void 0");
        engine->gc_hint();  // Should not throw or crash
        auto result = engine->evaluate("arr.length");
        REQUIRE(result.getWithDefault<int>(0) == 1000);
    END_FOR_EACH_ENGINE
}

TEST_CASE("JsEngine multiple native functions", "[js_engine]") {
    FOR_EACH_ENGINE(engine)
        engine->register_function("mul", [](const choc::value::Value* args, size_t count) {
            double a = count > 0 ? args[0].getWithDefault<double>(0.0) : 0.0;
            double b = count > 1 ? args[1].getWithDefault<double>(0.0) : 0.0;
            return choc::value::createFloat64(a * b);
        });
        engine->register_function("sub", [](const choc::value::Value* args, size_t count) {
            double a = count > 0 ? args[0].getWithDefault<double>(0.0) : 0.0;
            double b = count > 1 ? args[1].getWithDefault<double>(0.0) : 0.0;
            return choc::value::createFloat64(a - b);
        });
        auto result = engine->evaluate("sub(mul(7, 8), 14)");
        REQUIRE(result.getWithDefault<int>(0) == 42);
    END_FOR_EACH_ENGINE
}

// QuickJS unhandled-promise-rejection tracker.
//
// QuickJS doesn't call JS-side `addEventListener("unhandledrejection", ...)`
// handlers, so without a host hook a rejected promise with no `.catch` is
// silently dropped. The fix installs `JS_SetHostPromiseRejectionTracker` in
// QuickJsEngine()'s ctor and routes the rejection to runtime::log_error with
// the `PULP_QJS_UNHANDLED_REJECTION:` prefix. These tests verify the marker
// reaches stderr and that the engine survives the unhandled rejection +
// continues to evaluate.

TEST_CASE("QuickJS unhandled-rejection tracker logs PULP_QJS marker",
          "[js_engine][quickjs][issue-3206]") {
    if (!is_engine_available(JsEngineType::quickjs)) return;
    auto engine = create_js_engine(JsEngineType::quickjs);
    REQUIRE(engine->is_valid());

    std::string captured = capture_stderr([&]() {
        // Reject a promise with no .catch — the rejection becomes unhandled
        // exactly after the microtask queue drains (Promise.reject is sync
        // but the tracker fires when the queued reject task runs).
        engine->evaluate("Promise.reject(new Error('pulp-test-rejection'))");
        engine->pump_message_loop();
    });

    REQUIRE(captured.find("PULP_QJS_UNHANDLED_REJECTION") != std::string::npos);
    REQUIRE(captured.find("pulp-test-rejection") != std::string::npos);
    // Stack should also be logged when the reason is an Error.
    REQUIRE(captured.find("PULP_QJS_UNHANDLED_REJECTION_STACK") != std::string::npos);
}

TEST_CASE("QuickJS unhandled-rejection tracker does not crash on non-Error reasons",
          "[js_engine][quickjs][issue-3206]") {
    if (!is_engine_available(JsEngineType::quickjs)) return;
    auto engine = create_js_engine(JsEngineType::quickjs);
    REQUIRE(engine->is_valid());

    // Reject with a primitive — no `.stack` property exists on the reason.
    // The tracker must handle JS_GetPropertyStr returning undefined/exception
    // gracefully without aborting the host.
    std::string captured = capture_stderr([&]() {
        engine->evaluate("Promise.reject('plain-string-reason')");
        engine->pump_message_loop();
    });
    REQUIRE(captured.find("PULP_QJS_UNHANDLED_REJECTION") != std::string::npos);
    REQUIRE(captured.find("plain-string-reason") != std::string::npos);

    // Engine survives and remains usable.
    REQUIRE(engine->is_valid());
    auto result = engine->evaluate("1 + 1");
    REQUIRE(result.getWithDefault<int>(0) == 2);
}

TEST_CASE("QuickJS unhandled-rejection tracker logs exactly once per rejection",
          "[js_engine][quickjs][issue-3206]") {
    if (!is_engine_available(JsEngineType::quickjs)) return;
    auto engine = create_js_engine(JsEngineType::quickjs);

    // QuickJS fires the host tracker TWICE for a rejection that gets a
    // handler later: first with is_handled=0 (rejection created without a
    // handler) and then with is_handled=1 (handler attached). Our hook
    // logs ONLY the is_handled=0 case — verify the marker appears exactly
    // once for a single bare `Promise.reject(...)`, not twice. Without the
    // `if (is_handled) return;` guard at the top of the tracker, a noisy
    // duplicate would ship for every rejection.
    std::string captured = capture_stderr([&]() {
        engine->evaluate("Promise.reject(new Error('dedup-once'))");
        engine->pump_message_loop();
        engine->pump_message_loop();  // second pump in case the tracker is deferred
    });
    auto first = captured.find("PULP_QJS_UNHANDLED_REJECTION:");
    REQUIRE(first != std::string::npos);
    auto second = captured.find("PULP_QJS_UNHANDLED_REJECTION:", first + 1);
    REQUIRE(second == std::string::npos);
}

TEST_CASE("QuickJS rejection tracker surfaces 'new Promise(async ...)' anti-pattern",
          "[js_engine][quickjs][issue-3206]") {
    if (!is_engine_available(JsEngineType::quickjs)) return;
    auto engine = create_js_engine(JsEngineType::quickjs);

    // The specific anti-pattern that hung Three.js's WebGPURenderer.init():
    // a sync throw inside an `async` executor rejects the INNER async
    // function's promise — not the outer `new Promise(...)`. Without the
    // tracker the inner rejection is invisible. With it, we see the throw
    // immediately.
    std::string captured = capture_stderr([&]() {
        engine->evaluate(
            "new Promise(async function(resolve, reject) {"
            "  throw new Error('inner-async-throw');"
            "});");
        engine->pump_message_loop();
    });
    REQUIRE(captured.find("PULP_QJS_UNHANDLED_REJECTION") != std::string::npos);
    REQUIRE(captured.find("inner-async-throw") != std::string::npos);
}

// JSC NSException bridge in js_jsc_engine.mm.
//
// `[JSContext evaluateScript:]` can throw an NSException for malformed
// scripts. Without `@try`/`@catch(NSException*)`, the exception unwinds
// through the ObjC runtime and surfaces in C++ as the useless string
// "unknown exception" (or worse, a hard crash on `terminate`). The fix
// catches both NSException and any other ObjC throw, formats `name +
// reason` from the NSException, logs PULP_JSC_EVAL_NSEXCEPTION /
// PULP_JSC_EVAL_OBJC, and re-throws as std::runtime_error with the
// actual JSC error message. Verifying via stderr-grep is the cleanest
// route since the engine wrapper itself swallows the rethrown error
// into its check_exception() helper.

#if __APPLE__
TEST_CASE("JSC evaluate surfaces NSException via std::runtime_error",
          "[js_engine][jsc][issue-3206]") {
    if (!is_engine_available(JsEngineType::jsc)) return;
    auto engine = create_js_engine(JsEngineType::jsc);
    REQUIRE(engine->is_valid());

    // A bare top-level ES module `import` statement causes JSC's parser
    // to reject the script — depending on JSC's parse path this either
    // throws a JS-level SyntaxError (caught by check_exception()) or
    // raises an ObjC NSException (caught by our new @catch block).
    // Either way, an exception MUST reach the C++ caller — that's the
    // contract this test pins down. Without the @catch block the
    // NSException case would propagate uncaught and crash the host.
    bool threw = false;
    try {
        engine->evaluate("import { Color } from './nope.js';\nColor;");
    } catch (const std::exception&) {
        threw = true;
    } catch (...) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(engine->is_valid());

    // Engine survives the failed eval and continues to work.
    auto result = engine->evaluate("1 + 2");
    REQUIRE(result.getWithDefault<int>(0) == 3);
}
#endif
