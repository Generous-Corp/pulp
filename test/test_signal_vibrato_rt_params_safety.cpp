#include "test_signal_vibrato_support.hpp"

TEST_CASE("Vibrato parameter setters clamp to their declared ranges", "[vibrato][params]") {
    DelayVibrato64 delay;
    delay.prepare(kFs);
    delay.set_rate_hz(1e6);
    CHECK(delay.rate_hz() == Approx(DelayVibrato64::kMaxRateHz));
    delay.set_rate_hz(-1.0);
    CHECK(delay.rate_hz() == Approx(DelayVibrato64::kMinRateHz));
    delay.set_depth_cents(1e6);
    CHECK(delay.depth_cents() == Approx(DelayVibrato64::kMaxDepthCents));

    // The delay line is sized at prepare() for the worst legal case, so the
    // slowest rate at the deepest setting must still read inside it.
    delay.set_rate_hz(DelayVibrato64::kMinRateHz);
    delay.set_depth_cents(DelayVibrato64::kMaxDepthCents);
    delay.reset();
    bool finite = true;
    for (int i = 0; i < 96000; ++i) {
        const double y = delay.process(std::sin(kTwoPi * 110.0 * i / kFs));
        finite = finite && std::isfinite(y) && std::abs(y) <= 2.0;
    }
    CHECK(finite);

    PhaseVibrato64 phase;
    phase.prepare(kFs);
    phase.set_stage_count(99);
    CHECK(phase.stage_count() == PhaseVibrato64::kMaxStages);
    phase.set_stage_count(-3);
    CHECK(phase.stage_count() == 1);
    phase.set_center_hz(10.0);
    CHECK(phase.center_hz() == Approx(PhaseVibrato64::kMinCenterHz));

    UniVibe64 vibe;
    vibe.prepare(kFs);
    vibe.set_rate_hz(0.0);
    CHECK(vibe.rate_hz() == Approx(UniVibe64::kMinRateHz));
    vibe.set_depth(5.0);
    CHECK(vibe.depth() == Approx(1.0));
}

TEST_CASE("all vibrato engines reject non-finite controls and audio",
          "[signal][vibrato][nonfinite]") {
    for (double bad : {NAN, INFINITY, -INFINITY}) {
        DelayVibrato64 da, db; for(auto* v:{&da,&db}){v->prepare(kFs);v->set_rate_hz(4.1);v->set_depth_cents(37);v->set_delay_ms(11);v->set_fade_in_ms(23);v->reset();}
        da.set_rate_hz(bad); da.set_depth_cents(bad); da.set_delay_ms(bad); da.set_fade_in_ms(bad);
        REQUIRE(da.process(bad)==0); db.reset(); for(int i=0;i<64;++i) REQUIRE(da.process(.2)==db.process(.2));
        PhaseVibrato64 pa,pb; for(auto* v:{&pa,&pb}){v->prepare(kFs);v->set_rate_hz(3.2);v->set_depth(.6);v->set_center_hz(777);v->set_mix(.7);v->reset();}
        pa.set_rate_hz(bad);pa.set_depth(bad);pa.set_center_hz(bad);pa.set_mix(bad);REQUIRE(pa.process(bad)==0);pb.reset();for(int i=0;i<64;++i)REQUIRE(pa.process(.2)==pb.process(.2));
        UniVibe64 ua,ub;for(auto* v:{&ua,&ub}){v->prepare(kFs);v->set_rate_hz(2.4);v->set_depth(.8);v->reset();}ua.set_rate_hz(bad);ua.set_depth(bad);double l=1,r=1;ua.process(bad,l,r);REQUIRE(l==0);REQUIRE(r==0);ub.reset();for(int i=0;i<64;++i){double bl=0,br=0;ua.process(.2,l,r);ub.process(.2,bl,br);REQUIRE(l==bl);REQUIRE(r==br);}
    }
}
