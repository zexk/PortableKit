#version 450

layout(location = 0) in vec2 frag_texcoord;
layout(location = 1) in vec4 frag_color;
layout(location = 2) in vec3 frag_specular;
layout(location = 3) in float frag_fog;
layout(location = 4) flat in vec4 frag_uv_rect;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D guest_texture;
layout(set = 2, binding = 0) uniform sampler2D framebuffer_palette;

// False for pipelines of draws without an alpha test (texture_params.w 0):
// the discard below is then compiled out, so tiled GPUs keep their early
// depth test and hidden surface removal for them.
layout(constant_id = 0) const bool kAlphaTest = true;

layout(push_constant) uniform Push {
    mat4 transform;
    vec4 viewport;
    // x: texture flags (framebuffer_texture_flags on the host), y: texture
    // function (+256 palette filtering), z: alpha reference, w: alpha function.
    vec4 texture_params;
    vec4 uv_transform;
    vec4 view_z;
} push;

// Only the fog colour is read here; the block is described in ge.vert.
layout(set = 1, binding = 0) uniform Environment {
    vec4 ambient;
    vec4 fog;
    vec4 fog_color;
} lighting;

vec4 palette_texel(ivec2 position, int flags) {
    ivec2 size = textureSize(guest_texture, 0);
    vec4 color = texelFetch(guest_texture, clamp(position, ivec2(0), size - 1), 0);
    uint format = (uint(flags) >> 4) & 3u;
    uvec4 bits = uvec4(round(color * 255.0));
    uint pixel;
    if (format == 0u) {
        pixel = (bits.r >> 3) | ((bits.g >> 2) << 5) | ((bits.b >> 3) << 11);
    } else if (format == 1u) {
        pixel = (bits.r >> 3) | ((bits.g >> 3) << 5) | ((bits.b >> 3) << 10) | ((bits.a >> 7) << 15);
    } else if (format == 2u) {
        pixel = (bits.r >> 4) | ((bits.g >> 4) << 4) | ((bits.b >> 4) << 8) | ((bits.a >> 4) << 12);
    } else {
        pixel = bits.r | (bits.g << 8) | (bits.b << 16) | (bits.a << 24);
    }
    uint index = ((pixel >> ((uint(flags) >> 6) & 31u)) & ((uint(flags) >> 11) & 255u)) |
                 (((uint(flags) >> 19) & 31u) << 4);
    return texelFetch(framebuffer_palette, ivec2(int(index), 0), 0);
}

void main() {
    vec4 color = frag_color;
    if (push.texture_params.x > 0.5) {
        vec2 uv = clamp(frag_texcoord, frag_uv_rect.xy, frag_uv_rect.zw);
        // The GE's clamp wrap mode, per axis (texture_params.x bits 1 and 2):
        // the sampler repeats, so the coordinate stays between the centres of
        // the edge texels, which is what clamping to the edge samples.
        int flags = int(push.texture_params.x);
        if ((flags & 6) != 0) {
            vec2 half_texel = 0.5 / vec2(textureSize(guest_texture, 0));
            if ((flags & 2) != 0) uv.x = clamp(uv.x, half_texel.x, 1.0 - half_texel.x);
            if ((flags & 4) != 0) uv.y = clamp(uv.y, half_texel.y, 1.0 - half_texel.y);
        }
        vec4 texel;
        int function = int(push.texture_params.y + 0.5);
        if ((flags & 8) != 0) {
            vec2 position = uv * vec2(textureSize(guest_texture, 0)) - 0.5;
            ivec2 at = ivec2(floor(position));
            if ((function & 256) != 0) {
                vec2 f = fract(position);
                texel = mix(mix(palette_texel(at, flags), palette_texel(at + ivec2(1, 0), flags), f.x),
                            mix(palette_texel(at + ivec2(0, 1), flags), palette_texel(at + ivec2(1, 1), flags), f.x), f.y);
            } else {
                texel = palette_texel(ivec2(floor(position + 0.5)), flags);
            }
        } else {
            texel = texture(guest_texture, uv);
        }
        function &= 255;
        if (function == 0) {          // modulate
            color *= texel;
        } else if (function == 1) {   // decal
            color = vec4(mix(color.rgb, texel.rgb, texel.a), color.a);
        } else if (function == 2) {   // blend
            color = vec4(mix(color.rgb, texel.rgb, texel.rgb), color.a * texel.a);
        } else {                      // replace and everything else
            color = texel;
        }
    }

    // A separate specular term is added after texturing, then fog blends
    // towards its colour; neither touches alpha.
    color.rgb = min(color.rgb + frag_specular, vec3(1.0));
    if ((int(push.viewport.w + 0.5) & 1) != 0) color.rgb = mix(lighting.fog_color.rgb, color.rgb, clamp(frag_fog, 0.0, 1.0));

    // PSP alpha test, evaluated per fragment.
    if (!kAlphaTest) {
        out_color = color;
        return;
    }
    int alpha_function = int(push.texture_params.w + 0.5);
    float reference = push.texture_params.z / 255.0;
    float alpha = color.a;
    bool passed = true;
    if (alpha_function == 1) passed = false;                     // never
    else if (alpha_function == 2) passed = abs(alpha - reference) < 0.002;
    else if (alpha_function == 3) passed = abs(alpha - reference) >= 0.002;
    else if (alpha_function == 4) passed = alpha < reference;
    else if (alpha_function == 5) passed = alpha <= reference;
    else if (alpha_function == 6) passed = alpha > reference;
    else if (alpha_function == 7) passed = alpha >= reference;
    if (!passed) discard;

    out_color = color;
}
