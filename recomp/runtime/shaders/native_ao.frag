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
// the depth before the view's first car was drawn (FX_AO_WORLD, native_vk.cpp copy_depth_world), when u.mode is 1 (ПОРА111, the user,
// 2026-11-26: cars made a big halo on the road round them; their bodies keep their shade)
layout(set = 1, binding = 0) uniform sampler2D world;
layout(location = 0) out vec4 o_col;

// which this pixel goes by: the world's depth (a pixel of the road, a wall: no car in it, so a car shades nothing round it) or the
// whole depth (a car's own pixel: it is shaded by itself, the walls, the kerb, another car)
bool g_world = false;

// what the picture has at a pixel: its place in the world about the camera; nothing there: far off. Kept to this view's own pixels:
// with two players the other half of the picture is another camera's depth, which taken by this view's matrix made a false shade
// along the split (the picture's edges were all it kept to till 2026-11-25)
vec3 place(ivec2 c)
{
    ivec2 lo = max(ivec2(ceil(u.view.xy - 0.5)), ivec2(0));
    ivec2 hi = min(ivec2(floor(u.view.xy + u.view.zw - 0.5)), textureSize(depth, 0) - 1);
    c = clamp(c, lo, max(hi, lo));
    float d = g_world ? texelFetch(world, c, 0).r : texelFetch(depth, c, 0).r;
    vec4 h = u.to * vec4((vec2(c) + 0.5 - u.view.xy) / u.view.zw * 2.0 - 1.0, d, 1.0);
    return d > 0.0 && h.w != 0.0 ? h.xyz / h.w : vec3(1.0e9);
}

void main()
{
    o_col = vec4(0.0);
    ivec2 c = ivec2(gl_FragCoord.xy * u.rect.z);
    if (u.mode > 0.5) {
        // a car's pixel: drawn nearer than the world was there before the cars (or where the world had nothing); the depth goes by
        // one over the distance, the larger the nearer. A pixel of the world drawn after the first car counts as a car's too
        ivec2 at = clamp(c, ivec2(0), textureSize(depth, 0) - 1);
        float now = texelFetch(depth, at, 0).r, before = texelFetch(world, at, 0).r;
        g_world = !(now > before + 1.0e-4 * now);
    }
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
    float fade = 1.0 - smoothstep(100.0, 160.0, far);
    if (fade <= 0.0) return;
    // It reaches 7 m (rect.x) and stops at half the view's height on the picture, 16 taps (Off / Low / High for a day, 2026-11-25;
    // On / Off with this one, the user, 2026-11-26). The reach on the picture is so many pixels by what a pixel is across here; the
    // limit keeps the taps on the picture near the camera (the look of 2026-11-17 stopped at 72 of the picture's pixels: at twice the
    // resolution 15..30 cm of its 1.5 m within ten metres)
    float across = max(sqrt(min(dot(x1, x1), dot(x0, x0))), 1.0e-5);
    float reach = clamp(u.rect.x / across, 3.0, max(72.0, 0.5 * u.view.w));
    const int taps = 16;
    // The taps turn by the pixel's place in a four by four block, and their distance out goes by it too (a Bayer order, apart from
    // the turn's): sixteen turns and sixteen distances, which the second pass's sixteen taps average, so no rings show. They lie
    // closer in (the distance out to the power 1.5) for a sharp shade where things touch. A tap a pixel or two out, and the whole far
    // off, needs to stand higher to count (there the depth's own steps and a grazing view's normal make the grain).
    float turn = float((((int(gl_FragCoord.x) & 3) + (int(gl_FragCoord.y) & 3) * 4) * 7) & 15) * (6.2831853 / 16.0);
    const int bayer[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    float jitter = (float(bayer[(int(gl_FragCoord.x) & 3) + (int(gl_FragCoord.y) & 3) * 4]) + 0.5) / 16.0;
    float level = 0.08 + 0.10 * smoothstep(25.0, 80.0, far);
    float r = u.rect.x, lost = 0.0;
    for (int i = 0; i < taps; i++) {
        float angle = turn + float(i) * 2.3999632, out_ = pow((float(i) + jitter) / float(taps), 1.5);
        vec2 off = vec2(cos(angle), sin(angle)) * max(reach * out_, 1.0);
        ivec2 step_ = ivec2(round(off));    // at least a pixel out: one of the two is 0.7 or more
        vec3 v = place(c + step_) - here;
        float d2 = dot(v, v);
        if (!(d2 > 1.0e-8 && d2 < 1.0e10)) continue;
        // what stands above the surface there, by its angle above it, the nearer the more. The first 4.5 cm of its height count for
        // nothing: a car's body is flat triangles, and the next one bent up by a few degrees rose a centimetre or more and shaded
        // the whole of this one (a player on 0.14, a Mustang's rear bumper in dark teeth along its triangles; the user, 2026-11-25).
        // A kerb, a wall, a car's underside over the road stand far higher.
        // The halo about the cars (the user, 2026-11-26: "a big halo round the car, where no shadow should be"), two parts:
        // - the road seen past a car's outline is metres behind it, but the car was within the reach and shaded it all round the
        //   outline. The depth cannot say what lies behind the car's front, so a tap more than a metre nearer the camera than this
        //   place counts less, and from three metres not at all; what stands by the place, at its own distance, is untouched;
        // - beside the car, the nearness counts squared again (it was once till "stronger", 2026-11-25): the shade keeps to where
        //   things meet, and a car three metres off darkens the road a quarter as much, not half.
        float d = d2 * inversesqrt(d2), near_ = max(1.0 - d / r, 0.0);
        float ahead = far - length(here + v);
        float above = level + 0.15 * (1.0 - smoothstep(1.0, 3.0, length(vec2(step_))));
        lost += max((dot(n, v) - 0.045) / d - above, 0.0) * near_ * near_ * (1.0 - smoothstep(1.0, 3.0, ahead));
    }
    // a soft curve that never reaches black: at most 0.9 at the strength of 100 %
    o_col = vec4(0.9 * (1.0 - exp(-9.0 * lost / float(taps))) * fade);
}
