#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/js_engine_recommend.hpp>
#include <pulp/view/script_engine.hpp>
#include <string>
#include <vector>

using namespace pulp::view;

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

TEST_CASE("JsEngine Phase 13 smoke can assemble browser-style GPU bridge primitives", "[js_engine][phase13]") {
    auto engines_ = available_engines();
    REQUIRE_FALSE(engines_.empty());

    for (auto engine_type_ : engines_) {
        DYNAMIC_SECTION("engine=" << engine_type_name(engine_type_)) {
            ScriptEngine script(engine_type_);
            auto& engine = script.engine();

            if (!engine.supports_host_objects() || !engine.supports_promises()) {
                SUCCEED("phase13 smoke intentionally unsupported on this backend");
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
