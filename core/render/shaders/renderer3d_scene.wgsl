// Renderer3D scene shader. The Uniforms block mirrors SceneUniforms in
// core/render/src/renderer3d.cpp; edit the two together.

struct Uniforms {
    angle: f32,
    aspect: f32,
    pad0: f32,
    pad1: f32,
    base_color_factor: vec4f,
    unlit: f32,
    alpha_mask: f32,
    alpha_cutoff: f32,
    normal_shading: f32,
    emissive_factor: vec4f,
    light_color: vec4f,
    light_direction: vec4f,
    point_light_color: vec4f,
    point_light_position: vec4f,
    point_light_params: vec4f,
    spot_light_color: vec4f,
    spot_light_position: vec4f,
    spot_light_direction: vec4f,
    spot_light_params: vec4f,
    camera_params: vec4f,
    camera_offset: vec4f,
    camera_right: vec4f,
    camera_up: vec4f,
    camera_depth: vec4f,
    camera_depth_params: vec4f,
    material_params: vec4f,
    emissive_texture_params: vec4f,
    occlusion_texture_params: vec4f,
    normal_texture_params: vec4f,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var scene_texture: texture_2d<f32>;
@group(0) @binding(2) var scene_sampler: sampler;
@group(0) @binding(3) var emissive_texture: texture_2d<f32>;
@group(0) @binding(4) var emissive_sampler: sampler;
@group(0) @binding(5) var metallic_roughness_texture: texture_2d<f32>;
@group(0) @binding(6) var metallic_roughness_sampler: sampler;
@group(0) @binding(7) var occlusion_texture: texture_2d<f32>;
@group(0) @binding(8) var occlusion_sampler: sampler;
@group(0) @binding(9) var normal_texture: texture_2d<f32>;
@group(0) @binding(10) var normal_sampler: sampler;

struct VertexIn {
    @location(0) position: vec3f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
    @location(3) normal: vec3f,
    @location(4) tangent: vec4f,
    @location(5) normal_uv: vec2f,
    @location(6) metallic_roughness_uv: vec2f,
    @location(7) occlusion_uv: vec2f,
    @location(8) emissive_uv: vec2f,
};

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) shade: f32,
    @location(2) color: vec4f,
    @location(3) normal: vec3f,
    @location(4) local_position: vec3f,
    @location(5) tangent: vec4f,
    @location(6) normal_uv: vec2f,
    @location(7) metallic_roughness_uv: vec2f,
    @location(8) occlusion_uv: vec2f,
    @location(9) emissive_uv: vec2f,
};

@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    let cy = cos(uniforms.angle);
    let sy = sin(uniforms.angle);
    let cx = cos(0.35);
    let sx = sin(0.35);

    var p = input.position;
    let x0 = p.x * cy + p.z * sy;
    let z0 = -p.x * sy + p.z * cy;
    let y0 = p.y * cx - z0 * sx;
    let z1 = p.y * sx + z0 * cx;

    var out: VertexOut;
    let default_position = vec4f(x0 / uniforms.aspect,
                                 y0,
                                 0.5 - z1 * 0.30,
                                 1.0);
    let camera_relative = vec3f(x0, y0, z1) - uniforms.camera_offset.xyz;
    let camera_x = dot(camera_relative, uniforms.camera_right.xyz);
    let camera_y = dot(camera_relative, uniforms.camera_up.xyz);
    let camera_z = dot(camera_relative, uniforms.camera_depth.xyz);
    let camera_aspect = max(uniforms.camera_params.w, 0.0001);
    let view_z = max(1.2 - camera_z * 0.20, 0.2);
    let default_depth = 0.5 - camera_z * 0.30;
    let camera_near = max(uniforms.camera_depth_params.y, 0.0001);
    let camera_far = max(uniforms.camera_depth_params.z, camera_near + 0.0001);
    let depth_distance = max(view_z, camera_near);
    let ranged_depth = clamp((depth_distance - camera_near) /
                                 (camera_far - camera_near),
                             0.0,
                             1.0);
    let camera_depth_value = mix(default_depth,
                                 ranged_depth,
                                 uniforms.camera_depth_params.x);
    let perspective_position = vec4f(
        camera_x * uniforms.camera_params.y / (camera_aspect * view_z),
        camera_y * uniforms.camera_params.y / view_z,
        camera_depth_value,
        1.0);
    let orthographic_position = vec4f(
        camera_x * uniforms.camera_params.y / camera_aspect,
        camera_y * uniforms.camera_params.y,
        camera_depth_value,
        1.0);
    let camera_position = mix(perspective_position,
                              orthographic_position,
                              uniforms.camera_params.z);
    out.position = mix(default_position,
                       camera_position,
                       uniforms.camera_params.x);
    out.uv = input.uv;
    out.shade = clamp(0.82 - z1 * 0.22, 0.45, 1.0);
    out.color = input.color;
    out.normal = normalize(input.normal);
    out.local_position = input.position;
    out.tangent = input.tangent;
    out.normal_uv = input.normal_uv;
    out.metallic_roughness_uv = input.metallic_roughness_uv;
    out.occlusion_uv = input.occlusion_uv;
    out.emissive_uv = input.emissive_uv;
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4f {
    let texel = textureSample(scene_texture, scene_sampler, input.uv);
    let light_dir = normalize(uniforms.light_direction.xyz);
    let base_normal = normalize(input.normal);
    let tangent = normalize(input.tangent.xyz);
    let bitangent = normalize(cross(base_normal, tangent)) * input.tangent.w;
    let normal_texel = textureSample(normal_texture,
                                     normal_sampler,
                                     input.normal_uv);
    let tangent_normal_xy =
        (normal_texel.xy * 2.0 - vec2f(1.0)) * uniforms.normal_texture_params.y;
    let tangent_normal = normalize(vec3f(tangent_normal_xy,
                                         normal_texel.z * 2.0 - 1.0));
    let sampled_normal = normalize(tangent * tangent_normal.x +
                                   bitangent * tangent_normal.y +
                                   base_normal * tangent_normal.z);
    let shading_normal = normalize(mix(base_normal,
                                       sampled_normal,
                                       uniforms.normal_texture_params.x));
    let normal_light = clamp(dot(shading_normal, light_dir) * 0.55 + 0.45,
                             0.18,
                             1.0);
    let point_light_dir = normalize(uniforms.point_light_position.xyz -
                                    input.local_position);
    let point_distance = length(uniforms.point_light_position.xyz -
                                input.local_position);
    let point_range_light = mix(
        1.0,
        clamp(1.0 - point_distance / max(uniforms.point_light_params.y, 0.0001),
              0.0,
              1.0),
        uniforms.point_light_params.z);
    let point_normal_light = clamp(dot(shading_normal, point_light_dir) *
                                       0.55 + 0.45,
                                   0.18,
                                   1.0);
    let point_light_rgb = uniforms.point_light_color.rgb *
        point_normal_light * point_range_light * uniforms.point_light_params.x;
    let spot_to_fragment = normalize(input.local_position -
                                     uniforms.spot_light_position.xyz);
    let spot_light_dir = normalize(uniforms.spot_light_position.xyz -
                                   input.local_position);
    let spot_distance = length(uniforms.spot_light_position.xyz -
                               input.local_position);
    let spot_range_light = mix(
        1.0,
        clamp(1.0 - spot_distance / max(uniforms.spot_light_params.w, 0.0001),
              0.0,
              1.0),
        uniforms.spot_light_direction.w);
    let spot_cone = dot(spot_to_fragment,
                        normalize(uniforms.spot_light_direction.xyz));
    let spot_denominator = max(uniforms.spot_light_params.y -
                                   uniforms.spot_light_params.z,
                               0.0001);
    let spot_cone_light = clamp((spot_cone - uniforms.spot_light_params.z) /
                                    spot_denominator,
                                0.0,
                                1.0);
    let spot_normal_light = clamp(dot(shading_normal, spot_light_dir) *
                                      0.55 + 0.45,
                                  0.18,
                                  1.0);
    let spot_light_rgb = uniforms.spot_light_color.rgb *
        spot_normal_light * spot_cone_light * spot_range_light *
        uniforms.spot_light_params.x;
    let lit_shade = mix(input.shade, input.shade * normal_light, uniforms.normal_shading);
    let shade = mix(lit_shade, 1.0, uniforms.unlit);
    let light_rgb = mix(uniforms.light_color.rgb, vec3f(1.0), uniforms.unlit);
    let metallic_roughness_texel = textureSample(
        metallic_roughness_texture,
        metallic_roughness_sampler,
        input.metallic_roughness_uv);
    let metallic = clamp(
        uniforms.material_params.x *
            mix(1.0, metallic_roughness_texel.b, uniforms.material_params.z),
        0.0,
        1.0);
    let roughness = clamp(
        uniforms.material_params.y *
            mix(1.0, metallic_roughness_texel.g, uniforms.material_params.z),
        0.0,
        1.0);
    let pbr_energy = clamp(1.0 +
                               (1.0 - metallic) * 0.25 +
                               (1.0 - roughness) * 0.20,
                           1.0,
                           1.45);
    let material_energy = mix(pbr_energy, 1.0, uniforms.unlit);
    let alpha = texel.a * uniforms.base_color_factor.a * input.color.a;
    if uniforms.alpha_mask > 0.5 && alpha < uniforms.alpha_cutoff {
        discard;
    }
    let combined_light_rgb = min(light_rgb + point_light_rgb + spot_light_rgb,
                                 vec3f(4.0));
    let occlusion_texel = textureSample(occlusion_texture,
                                        occlusion_sampler,
                                        input.occlusion_uv);
    let occlusion = mix(
        1.0,
        mix(1.0, occlusion_texel.r, uniforms.occlusion_texture_params.y),
        uniforms.occlusion_texture_params.x);
    let lit = texel.rgb * uniforms.base_color_factor.rgb * input.color.rgb *
        shade * combined_light_rgb * material_energy * occlusion;
    let emissive_texel = textureSample(emissive_texture,
                                       emissive_sampler,
                                       input.emissive_uv);
    let emissive_rgb = mix(
        uniforms.emissive_factor.rgb,
        uniforms.emissive_factor.rgb * emissive_texel.rgb,
        uniforms.emissive_texture_params.x);
    let rgb = min(lit + emissive_rgb, vec3f(1.0));
    return vec4f(rgb,
                 alpha);
}
