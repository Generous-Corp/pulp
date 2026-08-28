@group(0) @binding(0) var<storage, read> input : array<f32>;

@compute @workgroup_size(1)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    input[gid.x] = 1.0;
}
