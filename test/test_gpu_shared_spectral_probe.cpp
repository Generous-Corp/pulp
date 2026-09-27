#include <pulp/gpu_audio/gpu_spectral_mask.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>
#include <chrono>
#include <thread>
#include <iostream>
#include <cmath>
#include <vector>

int main() {
    constexpr unsigned n=1024,h=256,c=2;
    std::vector<float> gains(n/2+1);
    for(unsigned i=0;i<gains.size();++i) gains[i]=i<40?0.8f:0.2f;
    auto created=pulp::gpu_audio::GpuSpectralMaskSession::create({n,h,c,48000,3,gains});
    if(!created){std::cerr<<"create failed "<<int(created.error)<<'\n';return 1;}
    auto& gpu=*created.session;
    pulp::signal::SpectralFrameEngine cpu;
    cpu.prepare({.fft_size=n,.analysis_hop=h,.channels=c,.max_block=h});
    std::vector<float> input(c*h),expected(c*h),actual(c*h);
    const float* in[]={input.data(),input.data()+h};
    float* out[]={expected.data(),expected.data()+h};
    unsigned rng=4321; double error=0;
    for(unsigned q=0;q<48;++q){
        for(auto& v:input){rng=rng*1664525u+1013904223u;v=q<36?float(rng>>8)/16777216.f-0.5f:0.f;}
        cpu.process(in,out,h,[&](auto frames,int bins){for(unsigned ch=0;ch<c;++ch)for(int k=0;k<bins;++k)frames[ch][k]*=gains[k];});
        if(!gpu.submit_hop(input,q))return 2;
        if(gpu.submit_hop(input,q+2))return 3; // A gap may not advance causal state.
        std::optional<pulp::gpu_audio::GpuSpectralMaskSession::Result> r;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!(r=gpu.receive(actual))){
            gpu.service(0);
            if(std::chrono::steady_clock::now()>deadline)return 4;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if(!r->delivered||r->sequence!=q||r->epoch!=gpu.epoch())return 5;
        for(unsigned i=0;i<c*h;++i){
            if(!std::isfinite(actual[i]))return 6;
            error=std::max(error,double(std::abs(actual[i]-expected[i])));
        }
    }
    std::cout<<"shared spectral CPU WOLA max error="<<error<<'\n';
    const auto report=gpu.diagnostics();
    if(!report.authenticated_shared_metal || report.dawn_revision.empty() || report.adapter_name.empty() ||
       report.runtime_write_buffer_calls || report.runtime_copy_buffer_calls || report.runtime_map_async_calls ||
       report.imported_allocations!=6 || report.retired_success!=48 || report.retired_failure ||
       report.cpu_input_bytes!=48*c*h*sizeof(float) || report.cpu_output_bytes!=48*c*h*sizeof(float)) return 9;
    if(!gpu.release() || !gpu.diagnostics().physical_release_confirmed)return 7;
    std::cout<<"provider="<<report.dawn_revision<<" adapter="<<report.adapter_name
             <<" imported="<<report.imported_allocations<<" retired="<<report.retired_success
             <<" webgpu_payload_calls=0 cpu_input_bytes="<<report.cpu_input_bytes<<'\n';
    return error<1e-4?0:8;
}
