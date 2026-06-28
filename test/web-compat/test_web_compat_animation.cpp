// Web-compat animation tests for the embedded Element.animate() shim.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;

TEST_CASE("WebCompat: Element.animate is available in the embedded prelude",
          "[webcompat][browser][animation]") {
    TestEnvironment env;
    auto surface = env.engine.evaluate(
        "typeof Element.prototype.animate === 'function' && "
        "typeof document.createElement('div').animate === 'function' && "
        "typeof Animation === 'function' && "
        "typeof window.Animation === 'function' && "
        "window.Animation === Animation");
    REQUIRE(surface.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: Element.animate completes, fills, and fires callbacks",
          "[webcompat][browser][animation]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __animEl = document.createElement('div');
        document.body.appendChild(__animEl);
        var __animEnded = 0;
        var __animFinished = 0;
        __animEl.addEventListener('animationend', function() { __animEnded++; });
        var __anim = __animEl.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 0, fill: 'forwards' }
        );
        __anim.finished.then(function() { __animFinished++; });
    )JS");

    REQUIRE(std::string(env.engine.evaluate("__anim.playState").getWithDefault<std::string_view>("")) == "finished");
    REQUIRE(env.engine.evaluate("__animEnded").getWithDefault<int32_t>(0) == 1);
    REQUIRE(env.engine.evaluate("__animFinished").getWithDefault<int32_t>(0) == 1);
    REQUIRE(std::string(env.engine.evaluate("__animEl.style.opacity").getWithDefault<std::string_view>("")) == "1");

    auto nativeId = std::string(env.engine.evaluate("__animEl._id").getWithDefault<std::string_view>(""));
    auto* view = env.widget(nativeId);
    REQUIRE(view != nullptr);
    REQUIRE_THAT(view->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.001f));
}

TEST_CASE("WebCompat: Element.animate interpolates numeric values before completion",
          "[webcompat][browser][animation]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __midEl = document.createElement('div');
        document.body.appendChild(__midEl);
        var __animNow = 1000;
        performance.now = function() { return __animNow; };
        var __mid = __midEl.animate(
            [{ opacity: '0', width: '10px' }, { opacity: '1', width: '30px' }],
            { duration: 1000, fill: 'forwards' }
        );

        var __easeEl = document.createElement('div');
        document.body.appendChild(__easeEl);
        var __ease = __easeEl.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 1000, easing: 'ease-out', fill: 'forwards' }
        );
        __animNow = 1500;
    )JS");
    env.bridge->poll_async_results();

    REQUIRE(std::string(env.engine.evaluate("__midEl.style.opacity").getWithDefault<std::string_view>("")) == "0.5");
    REQUIRE(std::string(env.engine.evaluate("__midEl.style.width").getWithDefault<std::string_view>("")) == "20px");
    REQUIRE(env.engine.evaluate("__midEl.style.opacity === '0.5px'").getWithDefault<bool>(true) == false);

    auto eased = env.engine.evaluate("Number(__easeEl.style.opacity)").getWithDefault<double>(0.0);
    REQUIRE(eased > 0.5);
    REQUIRE(eased < 1.0);
    env.eval("__mid.cancel(); __ease.cancel();");
}

TEST_CASE("WebCompat: Element.animate fill none and cancel restore captured values",
          "[webcompat][browser][animation]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __fillEl = document.createElement('div');
        document.body.appendChild(__fillEl);
        __fillEl.style.opacity = '0.25';
        var __fill = __fillEl.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 1000, fill: 'none' }
        );
        __fill.finish();

        var __cancelEl2 = document.createElement('div');
        document.body.appendChild(__cancelEl2);
        __cancelEl2.style.opacity = '0.2';
        var __cancelRejected = 0;
        var __cancelResolved = 0;
        var __cancelOnCancel = 0;
        var __cancel2 = __cancelEl2.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 1000, fill: 'forwards' }
        );
        __cancel2.finished.then(
            function() { __cancelResolved++; },
            function() { __cancelRejected++; }
        );
        __cancel2.oncancel = function() { __cancelOnCancel++; };
        __cancel2._applyAt(0.5);
        __cancel2.cancel();
    )JS");

    REQUIRE(std::string(env.engine.evaluate("__fillEl.style.opacity").getWithDefault<std::string_view>("")) == "0.25");
    REQUIRE(std::string(env.engine.evaluate("__cancelEl2.style.opacity").getWithDefault<std::string_view>("")) == "0.2");
    REQUIRE(env.engine.evaluate("__cancelResolved").getWithDefault<int32_t>(-1) == 0);
    REQUIRE(env.engine.evaluate("__cancelRejected").getWithDefault<int32_t>(0) == 1);
    REQUIRE(env.engine.evaluate("__cancelOnCancel").getWithDefault<int32_t>(0) == 1);
}

TEST_CASE("WebCompat: Element.animate pause and cancel stop queued frames",
          "[webcompat][browser][animation]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __pausedEl = document.createElement('div');
        document.body.appendChild(__pausedEl);
        var __pausedEnded = 0;
        __pausedEl.addEventListener('animationend', function() { __pausedEnded++; });
        var __paused = __pausedEl.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 1000, fill: 'forwards' }
        );
        __paused.pause();

        var __cancelEl = document.createElement('div');
        document.body.appendChild(__cancelEl);
        var __cancelEnded = 0;
        __cancelEl.addEventListener('animationend', function() { __cancelEnded++; });
        var __cancelled = __cancelEl.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 1000, fill: 'forwards' }
        );
        __cancelled.cancel();

        var __playEl = document.createElement('div');
        document.body.appendChild(__playEl);
        var __originalRaf = requestAnimationFrame;
        var __playRafCount = 0;
        requestAnimationFrame = function(fn) {
            __playRafCount++;
            return __originalRaf(fn);
        };
        var __playAgain = __playEl.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 1000, fill: 'forwards' }
        );
        __playAgain.play();
        requestAnimationFrame = __originalRaf;

        var __resumeEl = document.createElement('div');
        document.body.appendChild(__resumeEl);
        var __resumeNow = 1000;
        performance.now = function() { return __resumeNow; };
        var __resume = __resumeEl.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 1000, fill: 'forwards' }
        );
    )JS");
    env.engine.evaluate("__resumeNow = 1500");
    env.bridge->poll_async_results();
    env.eval("__resume.pause(); __resumeNow = 1700; __resume.play(); __resumeValueAfterPlay = __resumeEl.style.opacity;");

    REQUIRE(std::string(env.engine.evaluate("__paused.playState").getWithDefault<std::string_view>("")) == "paused");
    REQUIRE(env.engine.evaluate("__pausedEnded").getWithDefault<int32_t>(-1) == 0);
    REQUIRE(std::string(env.engine.evaluate("__pausedEl.style.opacity").getWithDefault<std::string_view>("")) == "0");

    REQUIRE(std::string(env.engine.evaluate("__cancelled.playState").getWithDefault<std::string_view>("")) == "idle");
    REQUIRE(env.engine.evaluate("__cancelEnded").getWithDefault<int32_t>(-1) == 0);
    REQUIRE(std::string(env.engine.evaluate("__cancelEl.style.opacity").getWithDefault<std::string_view>("")) == "0");
    REQUIRE(env.engine.evaluate("__playRafCount").getWithDefault<int32_t>(0) == 1);
    REQUIRE(std::string(env.engine.evaluate("__resumeValueAfterPlay").getWithDefault<std::string_view>("")) == "0.5");
    env.eval("__playAgain.cancel(); __resume.cancel();");
}

TEST_CASE("WebCompat: Element.animate finish applies final keyframe immediately",
          "[webcompat][browser][animation]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __finishEl = document.createElement('div');
        document.body.appendChild(__finishEl);
        var __finishEnded = 0;
        var __finishFinished = 0;
        var __finishOnFinish = 0;
        __finishEl.addEventListener('animationend', function() { __finishEnded++; });
        var __finish = __finishEl.animate(
            [{ opacity: '0' }, { opacity: '1' }],
            { duration: 1000, fill: 'forwards' }
        );
        __finish.finished.then(function() { __finishFinished++; });
        __finish.onfinish = function() { __finishOnFinish++; };
        __finish.finish();
    )JS");

    REQUIRE(std::string(env.engine.evaluate("__finish.playState").getWithDefault<std::string_view>("")) == "finished");
    REQUIRE(env.engine.evaluate("__finishEnded").getWithDefault<int32_t>(0) == 1);
    REQUIRE(env.engine.evaluate("__finishFinished").getWithDefault<int32_t>(0) == 1);
    REQUIRE(env.engine.evaluate("__finishOnFinish").getWithDefault<int32_t>(0) == 1);
    REQUIRE(std::string(env.engine.evaluate("__finishEl.style.opacity").getWithDefault<std::string_view>("")) == "1");

    auto nativeId = std::string(env.engine.evaluate("__finishEl._id").getWithDefault<std::string_view>(""));
    auto* view = env.widget(nativeId);
    REQUIRE(view != nullptr);
    REQUIRE_THAT(view->opacity(), Catch::Matchers::WithinAbs(1.0f, 0.001f));
}
