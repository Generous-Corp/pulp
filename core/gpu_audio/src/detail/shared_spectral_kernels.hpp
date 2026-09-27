#pragma once
#include <array>
#include <string>

namespace pulp::gpu_audio::detail {
// All mutable spectral history is queue-ordered device storage. Metadata lives
// in the imported slot and remains immutable until its physical retirement.
inline std::array<std::string, 4> shared_spectral_kernels(unsigned n, unsigned h, unsigned c) {
    const auto common = "const N:u32=" + std::to_string(n) + "u; const H:u32=" +
        std::to_string(h) + "u; const C:u32=" + std::to_string(c) + R"WGSL(u;
const META:u32=2u*N*C;
const OLA:u32=N*C;
const NORM:u32=3u*N*C;
const WINDOW:u32=NORM+2u*N;
@group(0) @binding(0) var<storage,read> input:array<f32>;
@group(0) @binding(1) var<storage,read_write> state:array<f32>;
@group(0) @binding(2) var<storage,read_write> data:array<f32>;
fn slot_word(i:u32)->u32 { return bitcast<u32>(input[META+i]); }
)WGSL";
    return {common + R"WGSL(
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id:vec3<u32>) {
 let k=id.x; if(k>=H*C){return;} let ch=k/H; let i=k%H;
 state[ch*N+(slot_word(0u)+i)%N]=input[k];
 // Touch binding 2 so all kernels share a stable three-buffer layout.
 if(k==0u){data[0u]=0.0;}
})WGSL", common + R"WGSL(
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id:vec3<u32>) {
 let k=id.x; if(k>=N*C){return;} let ch=k/N; let i=k%N;
 var value=0.0;
 if(slot_word(1u)!=0u){value=state[ch*N+(slot_word(0u)+H+i)%N]*state[WINDOW+i];}
 data[2u*k]=value; data[2u*k+1u]=0.0;
})WGSL", common + R"WGSL(
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id:vec3<u32>) {
 let k=id.x; if(k>=N*C || slot_word(1u)==0u){return;}
 let ch=k/N; let i=k%N; let index=(slot_word(2u)+i)%(2u*N); let w=state[WINDOW+i];
 state[OLA+ch*2u*N+index]+=data[2u*k]*w;
 if(ch==0u){state[NORM+index]+=w*w;}
})WGSL", common + R"WGSL(
@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id:vec3<u32>) {
 let i=id.x; if(i>=H){return;}
 if(slot_word(3u)==0u){for(var ch=0u; ch<C; ch++){data[ch*H+i]=0.0;} return;}
 let index=(slot_word(4u)+i)%(2u*N); var norm=state[NORM+index];
 if(slot_word(5u)!=0u){norm=max(norm,state[WINDOW+N]);}
 for(var ch=0u; ch<C; ch++){
   let at=OLA+ch*2u*N+index; var value=0.0;
   if(norm>1e-9){value=state[at]/norm;}
   data[ch*H+i]=value; state[at]=0.0;
 }
 state[NORM+index]=0.0;
})WGSL"};
}
// Read the immutable gain snapshot from this dispatch's imported input slot.
// The first 2*N*C floats reserve FFT payload, followed by six metadata words.
inline std::string shared_spectral_gain_kernel(unsigned n, unsigned c) {
    return "const N:u32=" + std::to_string(n) + "u; const C:u32=" +
        std::to_string(c) + R"WGSL(u;
@group(0) @binding(0) var<storage,read> spectrum:array<vec2<f32>>;
@group(0) @binding(1) var<storage,read> input:array<f32>;
@group(0) @binding(2) var<storage,read_write> product:array<vec2<f32>>;
@compute @workgroup_size(256) fn main(@builtin(global_invocation_id) id:vec3<u32>) {
 let k=id.x; if(k>=N*C){return;} let bin=k%N;
 let positive=select(N-bin,bin,bin<=N/2u);
 let gain=input[2u*N*C+6u+positive]/f32(N);
 product[k]=spectrum[k]*gain;
})WGSL";
}

}
