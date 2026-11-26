#version 450
// RGBA (96, 96, 96, 64), modulated, ALPHA 0x44 over a PSMCT16 map that nothing clears: a running blend, and the 5 bits it keeps wear it
// down, so it is worked out here as the GS does it, from the map as it was before this view (a copy).
layout(push_constant) uniform Push { int linear; } u;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 1, binding = 0) uniform sampler2D prev;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
// Dithering on (native_vk.cpp, g_color_wide): the views and the map have 16 bits a channel and the same running blend is done in
// fractions, with no 5 bits to round to: the reflections on the cars lose their steps of 8. The 5 bits also wore the map down (a
// view's texel some 3.5 of 255 on average, the blend's result some 8 where a view is laid at a half), and that much is taken off
// here, so that the reflections stay as dark as they were.
layout(constant_id = 0) const bool WIDE = false;

vec4 texel_wide(ivec2 at)
{
    at = clamp(at, ivec2(0), textureSize(tex, 0) - 1);
    vec4 t = texelFetch(tex, at, 0);
    return vec4(t.rgb, t.a > 0.25 ? 1.0 : 0.0);
}

// A view is PSMCT16 with TEXA 0, 0x80: its alpha is the bit, 0x80 where something was drawn (FBA 1) and 0 where the clear's grey is left,
// and the fold's As is 64 times that: what a view did not draw stays in the map as it was.
ivec4 texel(ivec2 at)
{
    at = clamp(at, ivec2(0), textureSize(tex, 0) - 1);
    vec4 t = texelFetch(tex, at, 0);
    return ivec4(ivec3(t.rgb * 255.0 + 0.5) / 8 * 8, t.a > 0.25 ? 128 : 0);
}

void main()
{
    vec2 uv = v_uv * vec2(textureSize(tex, 0));
    if (WIDE) {
        vec4 t;
        if (u.linear != 0) {
            vec2 at = uv - 0.5, f = floor(at), w = at - f;
            ivec2 i = ivec2(f);
            t = mix(mix(texel_wide(i), texel_wide(i + ivec2(1, 0)), w.x), mix(texel_wide(i + ivec2(0, 1)), texel_wide(i + ivec2(1, 1)), w.x), w.y);
        } else {
            t = texel_wide(ivec2(floor(uv)));
        }
        vec3 to = max(max(t.rgb - 3.5 / 255.0, vec3(0.0)) * 0.75 - 8.0 / 255.0, vec3(0.0));
        vec3 was = texelFetch(prev, ivec2(gl_FragCoord.xy), 0).rgb;
        o_col = vec4(was + (to - was) * (0.5 * t.a), 1.0);
        return;
    }
    ivec4 ct;
    if (u.linear != 0) {
        uv -= 0.5;
        vec2 f = floor(uv);
        ivec2 i = ivec2(f), w = ivec2((uv - f) * 256.0);
        ivec4 t00 = texel(i), t10 = texel(i + ivec2(1, 0)), t01 = texel(i + ivec2(0, 1)), t11 = texel(i + ivec2(1, 1));
        ivec4 top = t00 + ((t10 - t00) * w.x >> 8), bottom = t01 + ((t11 - t01) * w.x >> 8);
        ct = top + ((bottom - top) * w.y >> 8);
    } else {
        ct = texel(ivec2(floor(uv)));
    }
    ivec3 cs = ct.rgb * 96 >> 7;
    int as = 64 * ct.a >> 7;
    ivec3 cd = ivec3(texelFetch(prev, ivec2(gl_FragCoord.xy), 0).rgb * 255.0 + 0.5);
    ivec3 o = ((cs - cd) * as >> 7) + cd;
    o_col = vec4(vec3(o / 8 * 8) / 255.0, 1.0);
}
