#version 450
// Ambient Occlusion (native.cpp, ao_end; native_vk.cpp, draw_fx): how much of the sky a place on the picture has lost to what stands
// by it, from the picture's own depth. This pass makes that for every second pixel into an image of its own (native_ao_apply.frag
// lays it over the picture). The first one (till 2026-11-14) went over the finished picture, shadows and all, with a pattern of
// noise nothing smoothed, and spoilt the shadows' edges. This one is made before the shadows and the smoke are drawn, takes the
// surface's own lie into account (a level road shades nothing of itself), and its pattern is exactly what the second pass's four by
// four taps smooth away.
layout(push_constant) uniform Push {
    mat4 to;            // the picture's (x, y in -1..1 of the view, depth, 1) to the world, with the camera at 0
    vec4 rect;          // x the reach in metres, y the strength (the second pass's), z how many of the picture's pixels a pixel here is
    vec4 colour;
    vec4 view;          // the view in the picture's pixels: x, y, width, height
    float mode, texel, low, high;
} u;
layout(set = 0, binding = 0) uniform sampler2D depth;
layout(location = 0) out vec4 o_col;

// what the picture has at a pixel: its place in the world about the camera; nothing there: far off
vec3 place(ivec2 c)
{
    c = clamp(c, ivec2(0), textureSize(depth, 0) - 1);
    float d = texelFetch(depth, c, 0).r;
    vec4 h = u.to * vec4((vec2(c) + 0.5 - u.view.xy) / u.view.zw * 2.0 - 1.0, d, 1.0);
    return d > 0.0 && h.w != 0.0 ? h.xyz / h.w : vec3(1.0e9);
}

void main()
{
    o_col = vec4(0.0);
    ivec2 c = ivec2(gl_FragCoord.xy * u.rect.z);
    vec3 here = place(c);
    if (here.x > 1.0e8) return;
    // the surface: by the neighbour that is nearer on each side (at an edge the other one is something else's)
    vec3 x1 = place(c + ivec2(1, 0)) - here, x0 = here - place(c - ivec2(1, 0));
    vec3 y1 = place(c + ivec2(0, 1)) - here, y0 = here - place(c - ivec2(0, 1));
    vec3 dx = dot(x1, x1) < dot(x0, x0) ? x1 : x0, dy = dot(y1, y1) < dot(y0, y0) ? y1 : y0;
    vec3 n = cross(dx, dy);
    float nl = length(n);
    if (!(nl > 1.0e-12 && nl < 1.0e12)) return;
    n /= nl;
    if (dot(n, here) > 0.0) n = -n;         // towards the camera
    float far = length(here);
    float fade = 1.0 - smoothstep(50.0, 90.0, far);
    if (fade <= 0.0) return;
    // the reach on the picture: so many pixels, by what a pixel is across here
    float across = max(sqrt(min(dot(x1, x1), dot(x0, x0))), 1.0e-5);
    float reach = clamp(u.rect.x / across, 3.0, 72.0);
    // the taps turn by the pixel's place in a four by four block: sixteen turns, which the second pass's sixteen taps average
    float turn = float((((int(gl_FragCoord.x) & 3) + (int(gl_FragCoord.y) & 3) * 4) * 7) & 15) * (6.2831853 / 16.0);
    float lost = 0.0;
    const int taps = 10;
    for (int i = 0; i < taps; i++) {
        float angle = turn + float(i) * 2.3999632, out_ = (float(i) + 0.5) / float(taps);
        vec3 v = place(c + ivec2(round(vec2(cos(angle), sin(angle)) * (reach * out_)))) - here;
        float d2 = dot(v, v);
        if (!(d2 > 1.0e-8 && d2 < 1.0e10)) continue;
        // what stands above the surface there, the nearer the more; a tenth above level counts for nothing (the road's own grain)
        lost += max(dot(n, v) * inversesqrt(d2) - 0.08, 0.0) * max(1.0 - d2 / (u.rect.x * u.rect.x), 0.0);
    }
    o_col = vec4(clamp(lost / float(taps) * 3.6, 0.0, 1.0) * fade);
}
