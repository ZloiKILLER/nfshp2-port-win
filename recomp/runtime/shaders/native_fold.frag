#version 450
// RGBA (96, 96, 96, 64), modulated, ALPHA 0x44 over a PSMCT16 map that nothing clears: a running blend, and the 5 bits it keeps wear it
// down, so it is worked out here as the GS does it, from the map as it was before this view (a copy).
layout(push_constant) uniform Push { int linear; } u;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 1, binding = 0) uniform sampler2D prev;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

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
