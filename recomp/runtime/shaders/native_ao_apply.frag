#version 450
// Ambient Occlusion, laid over the picture (native_ao.frag made it at every second pixel): sixteen taps of it about the pixel, each
// counted by how near its own depth is to this pixel's, so the shade of a wall does not run onto the car in front of it. Darkens.
layout(push_constant) uniform Push {
    mat4 to;
    vec4 rect;          // y the strength, z how many of the picture's pixels a pixel of the image is
    vec4 colour;
    vec4 view;
    float mode, texel, low, high;
} u;
layout(set = 0, binding = 0) uniform sampler2D depth;
layout(set = 1, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 o_col;

void main()
{
    ivec2 last = textureSize(depth, 0) - 1, most = textureSize(tex, 0) - 1;
    float d0 = texelFetch(depth, clamp(ivec2(gl_FragCoord.xy), ivec2(0), last), 0).r;
    if (d0 <= 0.0) discard;                 // nothing was drawn there
    ivec2 base = ivec2(floor(gl_FragCoord.xy / u.rect.z - 0.5)) - 1;
    float sum = 0.0, weights = 0.0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            ivec2 q = clamp(base + ivec2(i, j), ivec2(0), most);
            float d = texelFetch(depth, clamp(ivec2((vec2(q) + 0.5) * u.rect.z), ivec2(0), last), 0).r;
            float apart = abs(d - d0) / max(d0, 1.0e-7);        // the depth goes by one over the distance: half a percent apart is another surface
            float w = 1.0 / (1.0 + apart * apart * 4.0e4);
            sum += w * texelFetch(tex, q, 0).r;
            weights += w;
        }
    float a = weights > 0.0 ? sum / weights * u.rect.y : 0.0;
    if (a <= 0.004) discard;
    o_col = vec4(0.0, 0.0, 0.0, a);
}
