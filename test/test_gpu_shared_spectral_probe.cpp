#include <pulp/gpu_audio/gpu_spectral_mask.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>
#include <chrono>
#include <thread>
#include <iostream>
#include <cmath>
#include <limits>
#include <vector>
using Session = pulp::gpu_audio::GpuSpectralMaskSession;

bool update_error(float actual, float expected, double& error) {
    if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
    const double residual = std::abs(double(actual) - double(expected));
    if (!std::isfinite(residual)) return false;
    error = std::max(error, residual);
    return true;
}

std::optional<Session::Result> receive(Session& gpu, std::span<float> output,
                                       std::uint64_t now = 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        gpu.service(now);
        if (auto result = gpu.receive(output)) return result;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return std::nullopt;
}

int run_case(unsigned n, unsigned h, unsigned c, bool impulse) {
    std::vector<float> gains(n / 2 + 1);
    for (unsigned i = 0; i < gains.size(); ++i)
        gains[i] = impulse ? 1.f : i < 40 ? 0.8f : 0.2f;
    auto created = Session::create({n, h, c, 48000, 3, gains});
    if (!created) { std::cerr << "create failed " << int(created.error) << '\n'; return 1; }
    auto& gpu = *created.session;
    if (gpu.latency_samples() != n + h) return 2;
    pulp::signal::SpectralFrameEngine cpu;
    cpu.prepare({.fft_size=int(n), .analysis_hop=int(h), .channels=int(c), .max_block=int(h)});
    std::vector<float> input(c*h), expected(c*h), actual(c*h);
    std::vector<const float*> in(c);
    std::vector<float*> out(c);
    for (unsigned ch=0; ch<c; ++ch) { in[ch]=input.data()+ch*h; out[ch]=expected.data()+ch*h; }
    unsigned rng=4321, peak_index=0;
    float peak=0;
    double error=0;
    for (unsigned q=0; q<48; ++q) {
        for (unsigned i=0; i<input.size(); ++i) {
            rng=rng*1664525u+1013904223u;
            input[i]=impulse ? ((q*h+i)==n ? 1.f : 0.f)
                              : q<36 ? float(rng>>8)/16777216.f-0.5f : 0.f;
        }
        cpu.process(in.data(), out.data(), h, [&](auto frames, int bins) {
            for (unsigned ch=0; ch<c; ++ch)
                for (int k=0; k<bins; ++k) frames[ch][k]*=gains[k];
        });
        if (gpu.submit_hop(std::span<const float>(input).first(input.size()-1), q)) return 3;
        if (!gpu.submit_hop(input, q, q==10 ? 1 : 0)) return 4;
        if (gpu.submit_hop(input, q+2)) return 5;
        auto result=receive(gpu, actual, q==10 ? 2 : 0);
        if (!result || !result->delivered || result->sequence!=q || result->epoch!=gpu.epoch()) return 6;
        if (q==10 && !result->late) return 7; // Physical completion is not deadline acceptance.
        for (unsigned i=0; i<c*h; ++i) {
            if (!update_error(actual[i], expected[i], error)) return 8;
            if (impulse && std::abs(actual[i])>peak) {peak=std::abs(actual[i]); peak_index=q*h+i;}
        }
    }
    if (impulse && (peak_index!=2*n+h || peak<0.99f)) return 9;
    const auto report=gpu.diagnostics();
#if defined(PULP_SPECTRAL_REQUIRE_CONFIGURED_PROVIDER)
    // A successful physical dispatch cannot substitute for the SDK's pin proof.
    if (!report.configured_revision_verified) return 13;
#endif
    if (!report.authenticated_shared_metal || report.dawn_revision.empty() || report.adapter_name.empty() ||
        report.runtime_write_buffer_calls || report.runtime_copy_buffer_calls || report.runtime_map_async_calls ||
        report.imported_allocations!=6 || report.retired_success!=48 || report.retired_failure ||
        report.cpu_input_bytes!=48*c*h*sizeof(float) || report.cpu_output_bytes!=48*c*h*sizeof(float)) return 10;
    if (!gpu.release() || !gpu.diagnostics().physical_release_confirmed || gpu.prepared()) return 11;
    std::cout << "fft=" << n << " hop=" << h << " channels=" << c
              << " impulse=" << impulse << " max_error=" << error
              << " provider=" << report.dawn_revision << " adapter=" << report.adapter_name
              << " configured_revision_verified=" << report.configured_revision_verified
              << " imported=" << report.imported_allocations << " retired=" << report.retired_success
              << " runtime_webgpu_calls=0 cpu_input_bytes=" << report.cpu_input_bytes << '\n';
    return error<1e-4 ? 0 : 12;
}

int capacity_case() {
    std::vector<float> gains(129, 1), input(64, 0), output(64);
    auto created=Session::create({256,64,1,48000,3,gains});
    if (!created) return 20;
    auto& gpu=*created.session;
    auto peer=Session::create({256,64,1,48000,3,gains});
    if (!peer || peer.session->epoch()==gpu.epoch() || !peer.session->release()) return 27;
    for (unsigned q=0; q<3; ++q) if (!gpu.submit_hop(input,q)) return 21;
    if (gpu.submit_hop(input,3)) return 22; // Slots remain owned until CPU release.
    bool seen[3]={};
    for (unsigned i=0; i<3; ++i) {
        auto r=receive(gpu,output);
        if (!r || !r->delivered || r->sequence>=3 || seen[r->sequence]) return 23;
        seen[r->sequence]=true;
    }
    if (!gpu.submit_hop(input,3)) return 24; // Refusal did not advance GPU history.
    auto r=receive(gpu,output);
    if (!r || !r->delivered || r->sequence!=3) return 25;
    if (!gpu.submit_hop(input,4) || !gpu.release()) return 26; // Drain in-flight work.
    const auto final_report = gpu.diagnostics();
    if (!final_report.physical_release_confirmed || final_report.retired_success != 5 ||
        final_report.retired_failure != 0) return 28;
    if (!gpu.release() || gpu.diagnostics().retired_success != 5) return 29;
    return 0;
}

int main() {
    double error=0;
    const float nan=std::numeric_limits<float>::quiet_NaN();
    if (update_error(nan,0,error) || update_error(0,nan,error) ||
        update_error(std::numeric_limits<float>::infinity(),0,error)) return 30;
    auto invalid=Session::create({});
    if (invalid || invalid.error!=Session::Error::InvalidConfig) return 31;
    std::vector<float> bad(129,1); bad[3]=nan;
    if (Session::create({256,64,1,48000,3,bad}).error!=Session::Error::InvalidConfig) return 32;
    for (const auto n : {256u,1024u,8192u}) {
        const int rc=run_case(n,n/4,n==256?1:2,n==256);
        if (rc) {std::cerr<<"case "<<n<<" failed "<<rc<<'\n';return rc;}
    }
    return capacity_case();
}
