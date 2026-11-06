#version 450
// The native renderer: what VU1's strip programs did to a vertex (src/vu/vu1MicroCode.dsm). A strip is drawn up
// to three times: u.pass 0 the strip, 1 the road's specular, 2 the sphere map.
// Colour is linear on the screen on the GS: times w here, times 1/w in the fragment shader. ST is perspective correct.
layout(push_constant) uniform Push {
    mat4 clip;
    vec2 scroll;
    float alpha_test;   // 1: the GS's alpha GREATER 0x30, AFAIL KEEP
    float blend_alpha;  // >= 0: the blend's fixed factor; -1 As; -2 no blending; -3 the frame's alpha; -4 the mask for -3's lerp
    float textured;
    int light;          // into L, -1: none
    int flags;          // vi07: 0x80 light, 0x40 road, 0x100 sphere map, 4 gloss map in alpha
    int pass;
    int tfx;
    float lod_l;
    int bits16;
    int tex_mode;
    vec2 snap;          // half the target in GS pixels, if the vertices are to land on the GS's sixteenths of a pixel
} u;
// VU1 memory from 860 (eLightContext) and from 848 (the material), by quads, for each light of the frame
layout(std430, set = 1, binding = 0) readonly buffer Lights { vec4 L[]; };
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec2 a_st;
layout(location = 3) in vec4 a_nrm;
layout(location = 4) in float a_w;
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_st;
layout(location = 2) out float v_k;
layout(location = 3) out float v_far;
invariant gl_Position;

const int COLOUR = 0, DIR = 4, EYE = 12, ENV = 14, MAT = 18, QUADS = 22;
// VU1's fast inverse root, from its memory at 668
const vec2 root = vec2(-0.5, intBitsToFloat(0x4ebe6189));

// VU1's fast power: m x^p as the float whose bits are p bits(x) + b; a logarithm, straight between powers of two
float fast_pow(float x, float p, float b)
{
    return intBitsToFloat(int(float(floatBitsToInt(x)) * p + b));
}

void main()
{
    gl_Position = u.clip * vec4(a_pos, 1.0) * a_w;
    // VU1 adds X / W to 2^19 as a float, which leaves four bits of fraction (the old path's floats round to the nearest)
    if (u.snap.x > 0.0 && gl_Position.w > 0.0) {
        vec2 px = (gl_Position.xy / gl_Position.w + 1.0) * u.snap;
        px = floor(px * 16.0 + 0.5) / 16.0;
        gl_Position.xy = (px / u.snap - 1.0) * gl_Position.w;
    }
    v_k = a_nrm.w * 127.0 / 16.0;
    v_far = gl_Position.z;
#ifdef CLIP_FAR
    gl_ClipDistance[0] = gl_Position.z;     // past the far plane, as the fragment shader's discard did: clipped before the rasterizer
#endif
    vec4 col = a_col * 255.0;
    vec2 st = a_st + u.scroll;
    if (u.light >= 0 && (u.flags & 0x1c0) != 0) {
        int b = u.light * QUADS;
        vec4 m0 = L[b + MAT], diffuse = L[b + MAT + 1], specular = L[b + MAT + 2], gloss = L[b + MAT + 3];
        // vuiCalculateReflectionVectorsAndDecompressNormals: the length is VU1's fast inverse root, not an exact one
        vec3 n = a_nrm.xyz, v = a_pos - L[b + EYE].xyz;
        vec3 r = v - n * (2.0 * dot(v, n));
        r *= fast_pow(dot(r, r), root.x, root.y);
        vec3 ld = L[b + DIR].xyz * n.x + L[b + DIR + 1].xyz * n.y + L[b + DIR + 2].xyz * n.z;
        vec3 ls = L[b + DIR].xyz * r.x + L[b + DIR + 1].xyz * r.y + L[b + DIR + 2].xyz * r.z;
        bool has_gloss = (u.flags & 4) != 0;
        vec2 power = has_gloss ? gloss.xy : m0.xy;
        vec3 s = max(ls, vec3(m0.z));
        s = vec3(fast_pow(s.x, power.x, power.y), fast_pow(s.y, power.x, power.y), fast_pow(s.z, power.x, power.y));
        vec3 lit = vec3(col.rgb);
        if ((u.flags & 0x80) != 0) {
            // vuiReferenceLighter; ftoi0 leaves whole numbers
            vec3 d = max(ld, vec3(0.0));
            vec3 c = L[b + COLOUR + 3].xyz;
            float a = L[b + COLOUR + 3].w;
            for (int i = 0; i < 3; i++) {
                c += L[b + COLOUR + i].xyz * diffuse.xyz * d[i] + L[b + COLOUR + i].xyz * specular.xyz * s[i];
                a += L[b + COLOUR + i].w * d[i];
            }
            vec3 mix_ = 1.0 + (col.rgb - 255.0) * m0.w;
            lit = floor(clamp(c * mix_, 0.0, 255.0));
            col = vec4(lit, floor(clamp(has_gloss ? gloss.z + gloss.w * s.x : a, 0.0, 255.0)));
        }
        // vuiReferenceRoadSpecularBasePass: the frame's alpha for the second pass
        if ((u.flags & 0x40) != 0) {
            vec3 sr = max(ls, vec3(m0.z));
            col.a = floor(clamp(gloss.z + gloss.w * fast_pow(sr.x, gloss.x, gloss.y), 0.0, 255.0));
        }
        if (u.pass == 2) {
            // vuiSphericalEnvMapper: x and z of the matrix negated. RGBAQ takes the low byte of the alpha, whatever it came to
            vec3 e0 = L[b + ENV].xyz * vec3(-1, 1, -1), e1 = L[b + ENV + 1].xyz * vec3(-1, 1, -1), e2 = L[b + ENV + 2].xyz * vec3(-1, 1, -1);
            vec3 q = e0 * r.x + e1 * r.y + e2 * r.z;
            vec3 h = q + vec3(0.0, 0.0, 1.0);
            st = q.xy * fast_pow(dot(h, h), root.x, root.y) * 0.5 + 0.5;
            col = vec4(has_gloss ? lit : vec3(128.0), float(int(diffuse.w + specular.w * q.z) & 255));
        }
    }
    v_col = col / 255.0 * gl_Position.w;
    v_st = st;
}
