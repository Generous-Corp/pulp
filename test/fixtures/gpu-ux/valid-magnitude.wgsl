@group(0) @binding(0) var<storage, read> input : array<f32>;
@group(0) @binding(1) var<storage, read_write> output : array<f32>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid : vec3u) {
    let idx = gid.x;
    let num_bins = arrayLength(&output);
    if (idx >= num_bins) {
        return;
    }
    let re = input[idx * 2u];
    let im = input[idx * 2u + 1u];
    output[idx] = sqrt(re * re + im * im);
}
