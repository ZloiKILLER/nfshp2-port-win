#version 450
// What eDisplayFrame lays over the frame by each pixel's depth or its own colour. The GS got the depth by copying Z into
// the frame's alpha, Z >> 8 or 255 past 0xffff (weather-fog.md); here the depth buffer is read as it is. The palette is linear.
layout(push_constant) uniform Push {
    int mode;           // 0 fog, 1 black and white, 2 depth of field
    int row;            // of the palette image
    vec2 size;          // the picture
    float lod;          // the level of the picture that is one GS pixel a texel
    vec2 scale;         // the picture's pixels to a GS pixel
    vec2 origin;        // where the 512 x 448 of the GS start in the picture
} u;
layout(set = 0, binding = 0) uniform sampler2D depth;
layout(set = 0, binding = 1) uniform sampler2D palette;
layout(set = 0, binding = 2) uniform sampler2D scene;
layout(location = 0) out vec4 o_col;

// A texel of the GS's half buffer, in GS pixels: the two jitter passes, (0, 0) and (+2, +2), each at FIX 64 over a clear buffer. A pass reads
// the frame at twice the half buffer's pixel, bilinear.
vec3 half_texel(ivec2 h)
{
    vec2 size = vec2(textureSize(scene, 0));
    vec3 a = floor(textureLod(scene, (u.origin + vec2(h * 2) * u.scale) / size, u.lod).rgb * 255.0 + 0.5);
    vec3 b = floor(textureLod(scene, (u.origin + vec2(h * 2 + 2) * u.scale) / size, u.lod).rgb * 255.0 + 0.5);
    return floor(a / 2.0) + floor(b / 2.0);
}

// The last pass stretches it over the frame from half a pixel back: at GS pixel x it reads x / 2 + 0.25, bilinear.
vec3 half_buffer()
{
    vec2 at = floor((gl_FragCoord.xy - u.origin) / u.scale) * 0.5 + 0.25 - 0.5;
    vec2 f = floor(at), w = floor((at - f) * 256.0) / 256.0;
    ivec2 i = ivec2(f);
    vec3 top = mix(half_texel(i), half_texel(i + ivec2(1, 0)), w.x);
    vec3 bottom = mix(half_texel(i + ivec2(0, 1)), half_texel(i + ivec2(1, 1)), w.x);
    return floor(mix(floor(top), floor(bottom), w.y)) / 255.0;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / u.size;
    if (u.mode == 1) {
        float g = textureLod(scene, uv, 0.0).g;     // the grey is the green
        o_col = vec4(g, g, g, 1.0);
        return;
    }
    float z = texelFetch(depth, ivec2(gl_FragCoord.xy), 0).r * 16777216.0;
    int i = z > 65535.0 ? 255 : int(z) >> 8;
    vec4 p = texelFetch(palette, ivec2(i, u.row), 0);
    float a = min(p.a * 255.0 / 128.0, 1.0);
    // ... >> 7 + Cd: the GS rounds down, the blender to the nearest
    vec3 cs = u.mode == 0 ? p.rgb : half_buffer();
    o_col = vec4(max(cs * a - 0.49 / 255.0, vec3(0.0)), a);
}
