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
// Dithering on (native_vk.cpp, g_color_wide): the picture, the mirror and the reflections' views have 16 bits a channel, and the colour
// of a pixel keeps what lies between two of the GS's bytes: the texels are filtered and the vertex's colour is multiplied in as
// fractions, nothing is rounded down to a byte (or, in a reflection's view, to 5 bits) on the way. With every draw rounded as the
// GS rounds there was nothing between two steps for the dithering to show, whatever the picture could hold. A colour that is a whole
// byte stays that byte. false: the GS's own integers, as always.
layout(constant_id = 1) const bool WIDE = false;
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

vec4 texel_wide(int level, ivec2 at, ivec2 size)
{
    if ((u.tex_mode & 1) != 0) at.x &= size.x - 1; else at.x = clamp(at.x, 0, size.x - 1);
    if ((u.tex_mode & 2) != 0) at.y &= size.y - 1; else at.y = clamp(at.y, 0, size.y - 1);
    return texelFetch(tex, at, level) * 255.0;
}

vec4 sample_level_wide(int level, vec2 uv, bool linear)
{
    ivec2 size = textureSize(tex, level);
    uv /= float(1 << level);
    if (!linear) return texel_wide(level, ivec2(floor(uv)), size);
    uv -= 0.5;
    vec2 f = floor(uv), w = uv - f;
    ivec2 i = ivec2(f);
    vec4 top = mix(texel_wide(level, i, size), texel_wide(level, i + ivec2(1, 0), size), w.x);
    vec4 bottom = mix(texel_wide(level, i + ivec2(0, 1), size), texel_wide(level, i + ivec2(1, 1), size), w.x);
    return mix(top, bottom, w.y);
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

// The rear-view mirror's picture on a car's own mirror glass (tex_mode 128; native.cpp, mirror_glass_now; the user, 2026-11-24).
// v_st.x is the place across the glass, -1 to 1 toward the car's left, v_st.y the picture's own t. The vertices' colour is no
// colour (the draw is DECAL) but the glass's numbers, the same at all its vertices: r the side (0 the right mirror, 255 the
// left, 128 the one inside), g the width of the picture's part over the glass, b its middle, both in 255ths of the picture.
//  - A side mirror is aspheric: flat over its inner two thirds, and over the outer third the picture runs on faster, by the
//    square of the way into that third (GLASS_CURVE; native.cpp's kGlassCurve is the same number, for the part's reach).
//  - The picture is taken at a quarter of its resolution: what a copy a quarter the size, each texel the mean of 4 x 4, would
//    give under a linear filter. A tap of the linear sampler at the corner four texels share is their mean; four such taps
//    are a quarter-size texel, and four of those are mixed as the copy's texels would be.
const float GLASS_CURVE = 1.0;
vec4 sample_glass(vec4 c)
{
    float side = c.r > 191.0 ? 1.0 : c.r < 64.0 ? -1.0 : 0.0;
    float across = v_st.x, outer = across * side - 1.0 / 3.0;
    if (outer > 0.0) across += side * GLASS_CURVE * outer * outer;
    vec2 uv = vec2(c.b / 255.0 + across * (c.g / 255.0) * 0.5, v_st.y);
    vec2 size = vec2(textureSize(tex, 0));
    vec2 at = uv * size * 0.25 - 0.5;
    vec2 f = floor(at), w = at - f;
    vec4 sum = vec4(0.0);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec2 base = (f + vec2(float(i), float(j))) * 4.0;
            vec4 t = textureLod(tex, (base + vec2(1.0, 1.0)) / size, 0.0) + textureLod(tex, (base + vec2(3.0, 1.0)) / size, 0.0) +
                     textureLod(tex, (base + vec2(1.0, 3.0)) / size, 0.0) + textureLod(tex, (base + vec2(3.0, 3.0)) / size, 0.0);
            sum += t * 0.25 * (i == 0 ? 1.0 - w.x : w.x) * (j == 0 ? 1.0 - w.y : w.y);
        }
    return sum * 255.0;
}

vec4 sample_tex(float lod)
{
    vec2 uv = v_st * vec2(textureSize(tex, 0));
    int mxl = u.tex_mode >> 8;
    bool min_linear = (u.tex_mode & 8) != 0;
    if (WIDE) {
        if (lod <= 0.0) return sample_level_wide(0, uv, (u.tex_mode & 4) != 0);
        if (mxl == 0) return sample_level_wide(0, uv, min_linear);
        if ((u.tex_mode & 16) == 0) return sample_level_wide(min(int(lod + 0.5), mxl), uv, min_linear);
        int l0 = min(int(lod), mxl);
        vec4 t = sample_level_wide(l0, uv, min_linear);
        if (l0 != mxl) t = mix(t, sample_level_wide(l0 + 1, uv, min_linear), lod - float(l0));
        return t;
    }
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

// what the GS rounds down to a byte
vec3 gs(vec3 x) { return WIDE ? x : floor(x); }
float gs(float x) { return WIDE ? x : floor(x); }

void main()
{
#ifndef CLIP_FAR
    if (v_far < 0.0) discard;               // past the far plane: the depth clamp would have kept it
#endif
    vec4 c = WIDE ? max(v_col * gl_FragCoord.w * 255.0, vec4(0.0)) : floor(v_col * gl_FragCoord.w * 255.0 + 0.01);
    vec3 rgb;
    float a8;
    if (u.textured > 0.5) {
        // the GS's level goes by Q alone: (log2(1 / Q) << L) + K, and Q is 1 / w
        float lod = u.pass == 2 ? 0.0 : log2(1.0 / gl_FragCoord.w) * pow(2.0, u.lod_l) + (u.pass == 0 ? v_k : 0.0);
        // the setting's anisotropic filter: the level goes by how the texture lies on the screen, not by Q
        vec4 t = (u.tex_mode & 128) != 0 ? sample_glass(c) : (u.tex_mode & 32) != 0 ? texture(tex, v_st) * 255.0 : sample_tex(lod);
        // the sphere map: its alpha is 1. The showroom's own map (bit 64) is 32 bits, and its alpha goes into As as any texture's
        if (u.pass == 2 && (u.tex_mode & 64) == 0) t.a = 128.0;
        if (u.tfx == 1) {                   // DECAL
            rgb = t.rgb / 255.0;
            a8 = t.a;
        } else if (u.tfx >= 2) {            // HIGHLIGHT, HIGHLIGHT2
            rgb = min(gs(c.rgb * t.rgb / 128.0) + c.a, vec3(255.0)) / 255.0;
            a8 = u.tfx == 2 ? min(t.a + c.a, 255.0) : t.a;
        } else {
            rgb = min(gs(c.rgb * t.rgb / 128.0), vec3(255.0)) / 255.0;
            a8 = min(gs(c.a * t.a / 128.0), 255.0);
        }
    } else {
        rgb = c.rgb / 255.0;
        a8 = c.a;
    }
    // the test is of the byte: with fractions kept, 48.9 is the byte 48
    if (ALPHA_TEST && u.alpha_test > 0.5 && (WIDE ? a8 < 49.0 : a8 <= 48.0)) discard;
    if (u.bits16 != 0) {                    // what such a target keeps
        if (!WIDE) rgb = floor(rgb * 255.0 / 8.0 + 1e-3) * 8.0 / 255.0;
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
        // (with fractions kept there is no byte to round to, and a whole colour laid whole stays whole)
        o_col = vec4(max(rgb * f - (WIDE ? 0.0 : 0.49 / 255.0), vec3(0.0)), min(f, 1.0));
    }
}
