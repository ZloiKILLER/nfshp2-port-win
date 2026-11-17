#version 450
// Shadows: High (native.cpp, "Shadows: High and Maximum"). A car's shadow laid over the picture by the picture's own depth: a pixel's
// place on the screen and its depth are a place in the world, and the matrix takes that straight to the shadow's own space.
//   mode 0, the patch under the car: (u, v) in the patch's texture and the height above the game's polygon. What lies between
//   `low` and `high` of it takes the texture as the polygon would have had it (TFX MODULATE, 0x80 is 1).
//   mode 1, the car's outline from the sun: (u, v) in the car's cell of the map and how far along the sun's ray, 0..1. The map has
//   the furthest of the car along each ray (0: no car there). What is further than that is in the shadow; its edge is soft, and
//   the softer the further the car is above the place.
//   mode 0's third is the height above the plane through the game's polygon (native.cpp, shadow_poly), not above a level.
layout(push_constant) uniform Push {
    mat4 to;            // the picture's (x, y in -1..1 of the view, depth, 1) to the shadow's space
    vec4 rect;          // mode 1: the car's cell in the map: x, y, width, height of the whole; mode 0: x, y the patch's size in metres
    vec4 colour;        // mode 0: the polygon's colour over 128; mode 1: a is the shadow's strength, r how much wider the soft edge
                        // gets for each 1 of the ray the car is above the place (in the cell's own 0..1)
    vec4 view;          // the view in the picture's pixels: x, y, width, height
    float mode;
    float texel;        // mode 1: a texel of the map in the cell's own 0..1
    float low, high;    // mode 0; mode 1: the soft edge's reach, in the cell's own 0..1: low at its widest, high at its narrowest
} u;
// MSAA: the picture is in samples, and the shadow goes over the samples themselves, each by its own depth (the shader runs once a
// sample: gl_SampleID). Over the resolved picture, as it was, a pixel on a car's edge was the car and the road mixed, with one depth
// for both: a rim of the wrong shade round every car that stood over a shadow.
#ifdef MSAA
layout(set = 0, binding = 0) uniform sampler2DMS depth;
#else
layout(set = 0, binding = 0) uniform sampler2D depth;
#endif
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 o_col;

// what the picture has at a pixel, in the shadow's space (the pixel's middle; with MSAA this sample's depth there)
vec3 place(ivec2 c)
{
#ifdef MSAA
    c = clamp(c, ivec2(0), textureSize(depth) - 1);
    float d = texelFetch(depth, c, gl_SampleID).r;
#else
    c = clamp(c, ivec2(0), textureSize(depth, 0) - 1);
    float d = texelFetch(depth, c, 0).r;
#endif
    vec4 h = u.to * vec4((vec2(c) + 0.5 - u.view.xy) / u.view.zw * 2.0 - 1.0, d, 1.0);
    return d > 0.0 && h.w != 0.0 ? h.xyz / h.w : vec3(1.0e9);
}

// Mode 0: how flat the surface at this pixel lies on the patch's plane, 1 the ground, 0 a wall. The patch is the ground's: laid on
// all that is no higher than a hand above the plane it also went over the lower part of the car's own wheels, tyres and rims, up
// to a level line (the user's picture, 2026-11-17). The surface's lie is taken from the pixels beside this one, on each side the
// neighbour that is nearer in height: at an edge the other one belongs to something else.
float flatness(ivec2 c)
{
    vec3 here = place(c);
    vec3 x1 = place(c + ivec2(1, 0)) - here, x0 = here - place(c - ivec2(1, 0));
    vec3 y1 = place(c + ivec2(0, 1)) - here, y0 = here - place(c - ivec2(0, 1));
    vec3 dx = abs(x1.z) < abs(x0.z) ? x1 : x0, dy = abs(y1.z) < abs(y0.z) ? y1 : y0;
    vec3 n = cross(vec3(dx.xy * u.rect.xy, dx.z), vec3(dy.xy * u.rect.xy, dy.z));
    float l = length(n);
    return l > 1.0e-20 && l < 1.0e15 ? abs(n.z) / l : 1.0;
}

void main()
{
#ifdef MSAA
    float d = texelFetch(depth, ivec2(gl_FragCoord.xy), gl_SampleID).r;
#else
    float d = texelFetch(depth, ivec2(gl_FragCoord.xy), 0).r;
#endif
    if (d <= 0.0) discard;                  // nothing was drawn there
    vec2 at = (gl_FragCoord.xy - u.view.xy) / u.view.zw * 2.0 - 1.0;
    vec4 h = u.to * vec4(at, d, 1.0);
    if (h.w == 0.0) discard;
    vec3 p = h.xyz / h.w;
    if (u.mode < 0.5) {
        if (!(p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0 && p.z >= u.low && p.z <= u.high)) discard;
        // the texel between its four neighbours, whatever the texture's own sampler is
        ivec2 size = textureSize(tex, 0);
        vec2 pos = p.xy * vec2(size) - 0.5;
        ivec2 i = ivec2(floor(pos)), last = size - 1;
        vec2 f = pos - floor(pos);
        vec4 t = mix(mix(texelFetch(tex, clamp(i, ivec2(0), last), 0), texelFetch(tex, clamp(i + ivec2(1, 0), ivec2(0), last), 0), f.x),
                     mix(texelFetch(tex, clamp(i + ivec2(0, 1), ivec2(0), last), 0), texelFetch(tex, clamp(i + ivec2(1, 1), ivec2(0), last), 0), f.x), f.y);
        float a = min(t.a * (255.0 / 128.0) * u.colour.a, 1.0);
        a *= smoothstep(u.low, u.low + 0.3, p.z);       // and fades out below it
        a *= 1.0 - smoothstep(u.high - 0.05, u.high, p.z);      // and above it: no level line on a kerb either
        if (a <= 0.0) discard;
        if (u.rect.x > 0.0) a *= smoothstep(0.45, 0.75, flatness(ivec2(gl_FragCoord.xy)));
        if (a <= 0.0) discard;
        o_col = vec4(min(t.rgb * u.colour.rgb, 1.0) * a, a);
    } else {
        if (!(p.x > 0.0 && p.x < 1.0 && p.y > 0.0 && p.y < 1.0 && p.z > 0.0 && p.z < 1.75)) discard;
        // How much of the car is over this pixel: the shadow's soft edge.
        // The glass (2026-11-17): the map's lower half has, cell for cell, the car's glass. A place with none of the body and
        // some glass that this pixel is behind is the glass's faint shadow: its tint.
        // The edge (2026-11-17, the user: "still pixelated"; his choice of the two). It was twenty-five taps on a square grid of
        // 2.5 cm, each "a car there or not" at the nearest texel: an edge along the grid crossed the taps five at a time, five
        // steps of a fifth each, and every step had the texels' own stairs. Now:
        //   - a tap is the share of car between the four texels about it (a texel's stairs are gone, and a tap's step is a
        //     slope a texel wide),
        //   - the taps lie on a disc, a spiral of thirty-two with no two in a line: an edge crosses them one at a time,
        //   - the disc is as wide as the car is far above the place (the mean of the car's surfaces found about it, sixteen
        //     taps over the widest the edge gets): hard by the wheels and the sills, wide under the roof's far edge.
        ivec2 map = textureSize(tex, 0);
        float across = 1.0 / u.texel;
        ivec2 corner = ivec2(u.rect.xy * vec2(map) + 0.5), last = ivec2(int(across + 0.5) - 1);
        ivec2 glass_at = ivec2(0, map.y / 2);
        const vec2 kSeek[16] = vec2[16](
            vec2(0.1768, 0.0000), vec2(-0.2258, 0.2068), vec2(0.0346, -0.3938), vec2(0.2846, 0.3712),
            vec2(-0.5222, -0.0924), vec2(0.4947, -0.3147), vec2(-0.1655, 0.6155), vec2(-0.3156, -0.6076),
            vec2(0.6846, 0.2500), vec2(-0.7123, 0.2940), vec2(0.3434, -0.7337), vec2(0.2537, 0.8089),
            vec2(-0.7647, -0.4432), vec2(0.8971, -0.1972), vec2(-0.5475, 0.7788), vec2(-0.1265, -0.9761));
        const vec2 kDisc[32] = vec2[32](
            vec2(0.1250, 0.0000), vec2(-0.1596, 0.1462), vec2(0.0244, -0.2784), vec2(0.2012, 0.2625),
            vec2(-0.3693, -0.0653), vec2(0.3498, -0.2225), vec2(-0.1170, 0.4352), vec2(-0.2231, -0.4296),
            vec2(0.4841, 0.1768), vec2(-0.5036, 0.2079), vec2(0.2428, -0.5188), vec2(0.1794, 0.5720),
            vec2(-0.5408, -0.3134), vec2(0.6344, -0.1395), vec2(-0.3871, 0.5507), vec2(-0.0894, -0.6902),
            vec2(0.5491, 0.4628), vec2(-0.7389, 0.0306), vec2(0.5390, -0.5363), vec2(-0.0361, 0.7798),
            vec2(-0.5128, -0.6145), vec2(0.8124, 0.1093), vec2(-0.6883, 0.4789), vec2(0.1881, -0.8361),
            vec2(0.4350, 0.7592), vec2(-0.8504, -0.2713), vec2(0.8261, -0.3817), vec2(-0.3579, 0.8552),
            vec2(-0.3194, -0.8880), vec2(0.8499, 0.4467), vec2(-0.9440, 0.2488), vec2(0.5366, -0.8345));
        float most = 0.0, above = 0.0, found = 0.0;
        for (int i = 0; i < 16; i++) {
            ivec2 t = clamp(ivec2(floor((p.xy + kSeek[i] * u.low) * across)), ivec2(0), last);
            float far = texelFetch(tex, corner + t, 0).r;
            most = max(most, far);
            if (far <= 0.0) {
                far = texelFetch(tex, corner + t + glass_at, 0).r;
                if (!(far > 0.0 && p.z > far + 0.006)) continue;
            }
            above += far, found += 1.0;
        }
        if (found <= 0.0) discard;              // no car anywhere near
        float wide = clamp((p.z - above / found) * u.colour.r, u.high, u.low);
        float shade = 0.0, tinted = 0.0;
        for (int i = 0; i < 32; i++) {
            // the four texels about the tap, taken at the corner they share (so which four is never in doubt)
            vec2 q = clamp(p.xy + kDisc[i] * wide, u.texel, 1.0 - u.texel) * across - 0.5;
            ivec2 at0 = clamp(ivec2(floor(q)), ivec2(0), last - 1);
            vec2 f = clamp(q - vec2(at0), 0.0, 1.0);
            vec4 share = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));     // textureGather's order
            vec4 far = textureGather(tex, (vec2(corner + at0) + 1.0) / vec2(map), 0);
            vec4 body = vec4(greaterThan(far, vec4(0.0)));
            shade += dot(body, share);
            most = max(most, max(max(far.x, far.y), max(far.z, far.w)));
            if (body.x + body.y + body.z + body.w < 3.5) {
                vec4 glass = textureGather(tex, (vec2(corner + at0 + glass_at) + 1.0) / vec2(map), 0);
                vec4 lit = vec4(greaterThan(glass, vec4(0.0))) * vec4(greaterThan(vec4(p.z), glass + 0.006)) * (1.0 - body);
                tinted += dot(lit, share);
            }
        }
        // What the pixel has to be behind: the car's furthest surface on the pixel's own ray, and exactly there, not at a texel's
        // middle. It was the furthest of the nine taps with a margin of 0.006 (5 cm along the ray), and the ground under a wheel,
        // which is less than that behind the tyre, was left in the light: a bright strip under every wheel (the user's picture,
        // 2026-11-17). The margin and the nine were there to keep the car from shading its own far side, where that surface runs
        // nearly along the ray and one texel's middle is far from the next. So: the four texels about the pixel. Where they lie
        // in a plane, however steep (their sums across the two diagonals agree), the surface there is the plane, and the value
        // between them is the car's own to a hair: a margin of a centimetre does. Where they do not (an edge of the car, a step
        // from one part to another, a texel with no car), the furthest of them, as before but of four and not nine.
        vec2 at4 = p.xy * across - 0.5;
        ivec2 i4 = ivec2(floor(at4));
        vec2 w4 = at4 - floor(at4);
        float f00 = texelFetch(tex, corner + clamp(i4, ivec2(0), last), 0).r;
        float f10 = texelFetch(tex, corner + clamp(i4 + ivec2(1, 0), ivec2(0), last), 0).r;
        float f01 = texelFetch(tex, corner + clamp(i4 + ivec2(0, 1), ivec2(0), last), 0).r;
        float f11 = texelFetch(tex, corner + clamp(i4 + ivec2(1, 1), ivec2(0), last), 0).r;
        float furthest = max(max(f00, f10), max(f01, f11));
        float behind;
        if (min(min(f00, f10), min(f01, f11)) > 0.0 && abs(f00 + f11 - f01 - f10) < 0.0006)
            behind = mix(mix(f00, f10, w4.x), mix(f01, f11, w4.x), w4.y);
        else
            behind = furthest > 0.0 ? furthest : most;
        if (!(p.z > behind + 0.0012)) shade = 0.0;
        float a = u.colour.a * (shade + 0.6 * tinted) / 32.0 * (1.0 - smoothstep(1.0, 1.75, p.z));
        if (a <= 0.0) discard;
        o_col = vec4(0.0, 0.0, 0.0, a);
    }
}
