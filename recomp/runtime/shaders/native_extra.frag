#version 450
// The port's own dithering against the steps of a smooth gradient (Options > Graphics > Advanced), on the finished picture: one
// pass that reads the picture's copy. None of this is the game's: the PS2 had none of it.
// (An ambient occlusion and a bloom were here too, passes of their own, and were taken out on the user's word, 2026-11-14:
// work/parked/ssao and work/parked/bloom.)
layout(push_constant) uniform Push {
    vec4 rect;          // the picture's part to work on, in pixels: left, top, right, bottom
    vec2 size;          // the picture
    float frame;        // moves the noise from frame to frame
    float lines;        // the picture's height over the GS's 448: sizes in pixels go by it
} u;
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) out vec4 o_col;

float hash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

void main()
{
    vec2 fc = gl_FragCoord.xy;
    ivec2 ip = ivec2(fc);
    vec4 here = texelFetch(scene, ip, 0);
    vec3 c = here.rgb;
    float n1 = hash(fc + u.frame * 7.31), n2 = hash(fc * 1.37 + 91.7 + u.frame * 3.17);
    // Four places round the pixel, at a distance and an angle of chance. Where all are within a few steps of the pixel's colour
    // the pixel is on a gradient (the sky, the fog, a shaded wall) and takes their average: the steps of 1 / 255 go. An edge
    // or a texture's detail differs by more and is left alone.
    float a = n1 * 6.2831853, d = (3.0 + n2 * 13.0) * u.lines;
    vec2 o = vec2(cos(a), sin(a)) * d;
    vec2 lo = u.rect.xy + 0.5, hi = u.rect.zw - 0.5;
    vec3 s0 = textureLod(scene, clamp(fc + o, lo, hi) / u.size, 0.0).rgb, s1 = textureLod(scene, clamp(fc - o, lo, hi) / u.size, 0.0).rgb;
    vec3 s2 = textureLod(scene, clamp(fc + vec2(-o.y, o.x), lo, hi) / u.size, 0.0).rgb, s3 = textureLod(scene, clamp(fc - vec2(-o.y, o.x), lo, hi) / u.size, 0.0).rgb;
    vec3 far = max(max(abs(s0 - c), abs(s1 - c)), max(abs(s2 - c), abs(s3 - c)));
    if (max(far.r, max(far.g, far.b)) < 3.5 / 255.0) c = (s0 + s1 + s2 + s3) * 0.25;
    c += (n1 + n2 - 1.0) / 255.0;           // a step's worth of noise: the average above lands between two steps
    o_col = vec4(c, here.a);
}
