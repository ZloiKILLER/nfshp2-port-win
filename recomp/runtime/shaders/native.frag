#version 450
// TFX MODULATE on the GS's bytes: (Cv * Ct) >> 7, so 0x80 is 1.0 and a vertex of 248 nearly doubles the texel.
// With the texture off the colour is the vertex's.
layout(push_constant) uniform Push {
    mat4 clip;
    vec2 scroll;
    float alpha_test;
    float blend_alpha;
    float textured;
    int light;
    int flags;
    int pass;
    int tfx;
    float lod_l;
    int bits16;         // the target is PSMCT16 (the envmap views)
    int tex_mode;       // bits: 1 repeat s, 2 repeat t, 4 mag linear, 8 min linear, 16 mip linear, 32 the GPU's filter; from bit 8 the last level
} u;
layout(set = 0, binding = 0) uniform sampler2D tex;
// false for the pipelines of draws without the alpha test: a shader with no discard in it keeps the GPU's early depth test
layout(constant_id = 0) const bool ALPHA_TEST = true;
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_st;
layout(location = 2) in float v_k;
layout(location = 3) in float v_far;
layout(location = 0) out vec4 o_col;
#ifdef SOFT
// Particles: High, the soft sprite (2026-11-17). A sprite is a flat sheet at the depth of its middle, and the road cut a large
// puff of smoke along a hard line. Here the particle is a ball of its own radius about that middle: a pixel shows as much of it
// as the share of the ball's thickness there that lies in front of what the picture already has behind it (a copy of the depth
// made before the frame's first such sprite: native_vk.cpp, copy_depth_soft). So a puff sinks into the ground and goes round a
// car, and thins out as the camera comes into it, where the game's goes out at once. Small ones (sparks, stones) stay as they are.
layout(set = 2, binding = 0) uniform sampler2D scene_depth;
layout(location = 4) in vec4 v_soft;        // native.vert: where in the sprite, how far its middle is, its radius
layout(location = 5) flat in vec4 v_cam;
layout(location = 6) flat in vec2 v_cam2;

float soft_share()
{
    if (!(v_soft.w > 0.0)) return 1.0;
    float zw = 2.0 + texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r * v_cam2.x;
    float under = zw * v_cam.z - v_cam.x;
    float behind = abs(under) > 1.0e-20 ? (v_cam.y - zw * v_cam.w) / under : 1.0e9;
    if (!(behind > 0.0)) behind = 1.0e9;    // nothing there
    float out2 = dot(v_soft.xy, v_soft.xy);
    float half_thick = v_soft.w * sqrt(max(1.0 - out2, 0.0625));
    float share = clamp((behind - (v_soft.z - half_thick)) / (2.0 * half_thick), 0.0, 1.0);
    share *= clamp((v_soft.z - v_cam2.y) / v_soft.w, 0.0, 1.0);
    // A ball has no corners: nothing of it past the circle within the sprite. A large puff near the camera showed the straight
    // edge of its sprite (the user's picture, 2026-11-17: a dark cloud ending at an upright line): its cell of the sheet, 32
    // texels stretched over hundreds of pixels, is not clear at its border, and the next cell's texels come into the filter there.
    share *= 1.0 - smoothstep(0.7, 1.0, sqrt(out2));
    return mix(1.0, share, smoothstep(0.15, 0.6, v_soft.w));
}
#endif

// The GS's sampling, integers and all: the same bytes as the software GS's sample_level and sample_tex.
ivec4 texel(int level, ivec2 at, ivec2 size)
{
    if ((u.tex_mode & 1) != 0) at.x &= size.x - 1; else at.x = clamp(at.x, 0, size.x - 1);
    if ((u.tex_mode & 2) != 0) at.y &= size.y - 1; else at.y = clamp(at.y, 0, size.y - 1);
    ivec4 t = ivec4(texelFetch(tex, at, level) * 255.0 + 0.5);
    if (u.pass == 2 && (u.tex_mode & 64) == 0) t.rgb = t.rgb / 8 * 8;     // the sphere map keeps 5 bits; CARSELECTENVMAP its own
    return t;
}

ivec4 sample_level(int level, vec2 uv, bool linear)
{
    ivec2 size = textureSize(tex, level);
    uv /= float(1 << level);
    if (!linear) return texel(level, ivec2(floor(uv)), size);
    uv -= 0.5;
    vec2 f = floor(uv);
    ivec2 i = ivec2(f), w = ivec2((uv - f) * 256.0);
    ivec4 t00 = texel(level, i, size), t10 = texel(level, i + ivec2(1, 0), size);
    ivec4 t01 = texel(level, i + ivec2(0, 1), size), t11 = texel(level, i + ivec2(1, 1), size);
    ivec4 top = t00 + ((t10 - t00) * w.x >> 8), bottom = t01 + ((t11 - t01) * w.x >> 8);
    return top + ((bottom - top) * w.y >> 8);
}

vec4 sample_tex(float lod)
{
    vec2 uv = v_st * vec2(textureSize(tex, 0));
    int mxl = u.tex_mode >> 8;
    bool min_linear = (u.tex_mode & 8) != 0;
    ivec4 t;
    if (lod <= 0.0) {
        t = sample_level(0, uv, (u.tex_mode & 4) != 0);
    } else if (mxl == 0) {
        t = sample_level(0, uv, min_linear);
    } else if ((u.tex_mode & 16) == 0) {
        t = sample_level(min(int(lod + 0.5), mxl), uv, min_linear);
    } else {
        int l0 = min(int(lod), mxl);
        t = sample_level(l0, uv, min_linear);
        if (l0 != mxl) {
            ivec4 t1 = sample_level(l0 + 1, uv, min_linear);
            t += (t1 - t) * int((lod - float(l0)) * 256.0) >> 8;
        }
    }
    return vec4(t);
}

void main()
{
#ifndef CLIP_FAR
    if (v_far < 0.0) discard;               // past the far plane: the depth clamp would have kept it
#endif
    vec4 c = floor(v_col * gl_FragCoord.w * 255.0 + 0.01);
    vec3 rgb;
    float a8;
    if (u.textured > 0.5) {
        // the GS's level goes by Q alone: (log2(1 / Q) << L) + K, and Q is 1 / w
        float lod = u.pass == 2 ? 0.0 : log2(1.0 / gl_FragCoord.w) * pow(2.0, u.lod_l) + (u.pass == 0 ? v_k : 0.0);
        // the setting's anisotropic filter: the level goes by how the texture lies on the screen, not by Q
        vec4 t = (u.tex_mode & 32) != 0 ? texture(tex, v_st) * 255.0 : sample_tex(lod);
        // the sphere map: its alpha is 1. The showroom's own map (bit 64) is 32 bits, and its alpha goes into As as any texture's
        if (u.pass == 2 && (u.tex_mode & 64) == 0) t.a = 128.0;
        if (u.tfx == 1) {                   // DECAL
            rgb = t.rgb / 255.0;
            a8 = t.a;
        } else if (u.tfx >= 2) {            // HIGHLIGHT, HIGHLIGHT2
            rgb = min(floor(c.rgb * t.rgb / 128.0) + c.a, vec3(255.0)) / 255.0;
            a8 = u.tfx == 2 ? min(t.a + c.a, 255.0) : t.a;
        } else {
            rgb = min(floor(c.rgb * t.rgb / 128.0), vec3(255.0)) / 255.0;
            a8 = min(floor(c.a * t.a / 128.0), 255.0);
        }
    } else {
        rgb = c.rgb / 255.0;
        a8 = c.a;
    }
    if (ALPHA_TEST && u.alpha_test > 0.5 && a8 <= 48.0) discard;
    if (u.bits16 != 0) {                    // what such a target keeps
        rgb = floor(rgb * 255.0 / 8.0 + 1e-3) * 8.0 / 255.0;
        // the env map's views have FBA 1 in their register block: whatever is drawn there sets the alpha bit, the clear's grey has it clear
        if (u.blend_alpha < -1.5) a8 = 128.0;
    }
    // The frame's alpha holds As as it is, a byte. Cs Ad, with 0x80 as 1.0, comes to twice Cs Ad / 256: drawn twice, -3.
    if (u.blend_alpha < -3.5) {
        o_col = vec4(rgb, min(a8 / 128.0, 1.0));    // what (Cs - Cd) Ad + Cd reads: a factor here, it does not pass 0x80
    } else if (u.blend_alpha < -2.5) {
        o_col = vec4(rgb * (255.0 / 256.0), 0.0);
    } else if (u.blend_alpha < -1.5) {
        o_col = vec4(rgb, a8 / 255.0);
    } else {
        // the factor can go past 1 on the GS: the colour takes it whole, the blend's alpha what fits
        // ... >> 7 + Cd: the GS rounds down, the blender to the nearest
        float f = u.blend_alpha < 0.0 ? a8 / 128.0 : u.blend_alpha;
#ifdef SOFT
        f *= soft_share();
#endif
        o_col = vec4(max(rgb * f - 0.49 / 255.0, vec3(0.0)), min(f, 1.0));
    }
}
