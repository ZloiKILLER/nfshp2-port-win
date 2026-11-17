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
    int merged;         // not 0: the draw is one of a run sent in one call; its matrix and its light are D[gl_InstanceIndex]'s
    float between;      // where this picture is between the last step of the game's and this one: 1 is this one
} u;
// a merged draw's own: what differs between the draws of a run (native_vk.cpp, draw_target)
struct DrawData {
    mat4 clip;
    int light;
    int pad0, pad1, pad2;
};
layout(std430, set = 1, binding = 1) readonly buffer Draws { DrawData D[]; };
// VU1 memory from 860 (eLightContext) and from 848 (the material), by quads, for each light of the frame
layout(std430, set = 1, binding = 0) readonly buffer Lights { vec4 L[]; };
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec2 a_st;
layout(location = 3) in vec4 a_nrm;
layout(location = 4) in float a_w;
layout(location = 5) in vec3 a_pos_mid;     // where the vertex is in the picture halfway to this frame: itself, but for the draws with a second copy
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_st;
layout(location = 2) out float v_k;
layout(location = 3) out float v_far;
// A particle of the port's as a ball of smoke, for native.frag's SOFT (the other fragment shaders do not read these):
//   v_soft  xy where in the sprite, -1..1 each way; z how far its middle is from the camera, w its radius, both in metres (w 0: no ball)
//   v_cam   the view's depth: the GS's Z / W of a point d metres ahead is (x d + y) / (z d + w)
//   v_cam2  x: Z / W is 2 + the depth buffer's value times this; y: how far ahead the nearest the view shows is, in metres
layout(location = 4) out vec4 v_soft;
layout(location = 5) flat out vec4 v_cam;
layout(location = 6) flat out vec2 v_cam2;
invariant gl_Position;

const int COLOUR = 0, DIR = 4, EYE = 12, ENV = 14, MAT = 18, QUADS = 22;
// VU1's fast inverse root, from its memory at 668
const vec2 root = vec2(-0.5, intBitsToFloat(0x4ebe6189));

// VU1's fast power: m x^p as the float whose bits are p bits(x) + b; a logarithm, straight between powers of two
float fast_pow(float x, float p, float b)
{
    return intBitsToFloat(int(float(floatBitsToInt(x)) * p + b));
}

// ---- Particles: High (native.cpp, "the port's own particles") --------------------------------------------------------
// The vertex is a particle as it was born, and one corner of its sprite:
//   a_pos   where it was born                       a_st.x  the clock then (its low 16 bits), in the game's 1/1024 s
//   a_nrm   xyz its direction (127 is 1), w corner  a_st.y  its life in those ticks
//   a_w     its speed (128 is 1 m/s)                a_col   its angle (low byte, high byte), its share of the car's way, its frame
// The block at L[b] (native.cpp, gpu_block) has the view and the emitter. Everything else is done here, for the time of this
// picture: the game's own steps from its birth (AcidEffectSystem::DoTimeStep, 0x128718: the drag by the square of the speed along
// each axis, gravity, then the speed cut to a word and the direction to three bytes, which is what slows the game's smoke down),
// each as long as the game's was (their lengths in seconds are in L too, the latest first: native.cpp, gpu_table), then the sprite
// as AcidEffectSystem::RenderEffects (0x128da0) makes it, with nothing cut to the GS's grid.
const int PARTICLE = 0x10000;        // above the 16 bits of vi07
const int SOFT_FRONT = 0x20000;      // the renderer's: this draw is made with native.frag's SOFT (native_vk.cpp, draw_target)
const int SOFT_2D = 0x40000;         // a draw of the port's sprites of the game's own particles (native.cpp, kSoft2dFlag): a_nrm.xy is
                                     // the sprite's corner, -1 or 1 each way, a_w its radius in metres, and L at its light has the view's

// Drawn as a ball, a sprite is tested against the depth at the ball's front, not at its middle: at its middle the road would still
// cut it, at half strength. The fragment shader fades it out before that. The depth for the vertex, as the game cuts a sprite's.
float soft_front(vec4 cam, float nearest, float middle, float radius, int shift, float z)
{
    float front = max(middle - radius * smoothstep(0.15, 0.6, radius), nearest);
    float under = cam.z * front + cam.w;
    if (!(abs(under) > 1.0e-20)) return z;
    float zw = clamp((cam.x * front + cam.y) / under, 2.0, 3.9999995);
    return max(float(((floatBitsToInt(zw) & 0x0fffffff) >> shift) & 0xffffff) / 16777216.0, z);
}
void particle(mat4 clip, int b)
{
    gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);       // not shown: a sprite's six vertices all come here, and it has no area
    v_col = vec4(0.0);
    v_st = vec2(0.0);
    v_k = 0.0;
    v_far = 0.0;
    v_soft = vec4(0.0);
    v_cam = vec4(0.0);
    v_cam2 = vec2(0.0);
#ifdef CLIP_FAR
    gl_ClipDistance[0] = 0.0;
#endif
    float f = u.between;
    vec4 clock = L[b + 28];         // the clock a frame before (16 bits of it), the ticks since, the sheet's ticks a frame, its cells less one
    vec4 hist = L[b + 32];          // the steps: where their lengths are in L, how many are there, how many of them this frame added; 1/127
    float life = a_st.y;
    float age1 = mod(clock.x + clock.y - a_st.x + 65536.0, 65536.0);       // its age as the frame itself shows it
    if (age1 > 60000.0) return;     // all but a whole turn of the clock: not born yet
    float age = age1 - (1.0 - f) * clock.y;
    if (age < 0.0 || age >= life) return;
    vec4 k0 = L[b + 27];            // the emitter's Life, Gravity, Drag; how much narrower a sprite is on the wide picture
    int table = int(hist.x), have = int(hist.y), since = int(hist.z);
    // how many steps it has lived through: back from the latest till their ticks make its age
    int n = 0;
    float ticks = 0.0, seconds = 1.0 / 60.0;
    while (ticks + 0.5 < age1 && n < 4096) {
        if (n < have) seconds = L[table + (n >> 2)][n & 3];
        ticks += max(float(int(seconds * 1024.0)), 1.0);
        n++;
    }
    // and through them, the oldest first. `was` is where it stood when the frame before was shown
    vec3 pos = a_pos, was = a_pos;
    vec3 d = round(a_nrm.xyz * 127.0);
    float sp = a_w;
    for (int j = n - 1; j >= 0; j--) {
        if (j == since - 1) was = pos;
        int at_ = min(j, have - 1);
        float dt = have > 0 ? L[table + (at_ >> 2)][at_ & 3] : 1.0 / 60.0;
        vec3 v = d * (sp * (1.0 / 128.0) * hist.w);
        vec3 acc = -sign(v) * k0.z * (v * v);
        acc.z -= k0.y;
        v += acc * dt;
        pos += v * dt;
        float l = length(v);
        sp = float(int(l * 128.0) & 0xffff);
        d = l > 0.0 ? trunc(v * (1.0 / l) * 127.0) : vec3(0.0);
    }
    if (since == 0) was = pos;
    ivec4 c8 = ivec4(a_col * 255.0 + 0.5);
    // between the two frames: on the way from the one to the other; and with its car by its share (MotionLive)
    pos = mix(was, pos, f) + mix(L[b + 30].xyz, L[b + 31].xyz, f) * (float(c8.z) * L[b + 33].x);
    vec4 p1 = vec4(pos, 1.0);
    vec4 cl = mat4(mix(L[b], L[b + 9], f), mix(L[b + 1], L[b + 10], f), mix(L[b + 2], L[b + 11], f), mix(L[b + 3], L[b + 12], f)) * p1;
    // eGetClipFlags, its depth alone: the game drops a particle whose middle is outside the view, and a wide sprite of smoke went out
    // all at once as its middle crossed the picture's edge. Here the picture's edge cuts it.
    if (!(abs(cl.z) <= abs(cl.w))) return;
    vec4 ahead = mix(L[b + 8], L[b + 17], f);
    float depth = dot(ahead, p1);
    mat4 screen = mat4(mix(L[b + 4], L[b + 13], f), mix(L[b + 5], L[b + 14], f), mix(L[b + 6], L[b + 15], f), mix(L[b + 7], L[b + 16], f));
    vec4 scr = screen * p1;
    if (scr.w == 0.0) return;
    vec3 at = scr.xyz / scr.w;
    if (!(abs(at.x) < 16384.0 && abs(at.y) < 16384.0)) return;
    float t = 1.0 - (life - age) * (1.0 / 1024.0) / k0.x;
    vec4 tt = vec4(t * t * t, t * t, t, 1.0);
    vec4 extra = mat4(L[b + 22], L[b + 23], L[b + 24], L[b + 25]) * tt;     // the size, the spin
    vec4 m0 = L[b + 26];            // H a step before and now, the most pixels, the far limit
    float pixels = extra.x > 0.0 ? extra.x * mix(m0.x, m0.y, f) / depth : 0.0;
    pixels = min(pixels, m0.z);
    if (!(pixels > 1.0) || !(depth < m0.w)) return;
    int angle = c8.x | (c8.y << 8);
    int delta = abs(extra.y) < 2.0e9 ? int(extra.y) & 0xffff : 0;
    int rot = ((angle & 1) != 0 ? angle - delta : angle + delta) & 0xffff;
    int corner = int(round(a_nrm.w * 127.0));
    vec2 e = vec2((corner & 1) != 0 ? pixels : -pixels, (corner & 2) != 0 ? pixels : -pixels);
    vec2 off;
    if (rot == 0) {
        off = e * vec2(0.5, 0.4375);
    } else {
        float th = float(rot) * L[b + 30].w, sn = sin(th), cs = sin(th + L[b + 31].w);
        off = vec2((cs * e.x - sn * e.y) * 0.5, (sn * e.x + cs * e.y) * 0.4375);
    }
    off.x *= k0.w;
    vec4 col = floor(clamp(mat4(L[b + 18], L[b + 19], L[b + 20], L[b + 21]) * tt, 0.0, 255.0));
    vec4 sheet = L[b + 29];         // the sheet's powers of two across and down, the depth's shift, TEX1's K
    int frame = c8.w, period = int(clock.z);
    if (period > 0) frame = (frame + int(life) / period - (int(life) - int(age)) / period) & int(clock.w);
    int pw = int(sheet.x), ph = int(sheet.y);
    vec2 cell = vec2(1.0 / float(1 << pw), 1.0 / float(1 << ph));
    v_st = (vec2(float(frame & ((1 << pw) - 1)), float((frame >> pw) & ((1 << ph) - 1))) + vec2(float(corner & 1), float((corner >> 1) & 1))) * cell;
    // the depth as the game cuts it: the bits of the float z, 28 of them, shifted
    float z = float(((floatBitsToInt(at.z) & 0x0fffffff) >> int(sheet.z)) & 0xffffff) / 16777216.0;
    gl_Position = clip * vec4(at.xy + off, z, 1.0);
    v_k = sheet.w / 16.0;
    v_far = gl_Position.z;
#ifdef CLIP_FAR
    gl_ClipDistance[0] = gl_Position.z;
#endif
    v_col = col / 255.0 * gl_Position.w;
    // The ball of smoke (2026-11-17). The screen's rows for Z and W go by the distance ahead alone, as any projection's do: each is
    // the row `ahead` times a number, plus a number. From them the fragment shader has the distance of what the picture already
    // shows behind this pixel, out of the depth buffer.
    vec3 size = abs(ahead.xyz);
    int axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
    if (size[axis] > 1.0e-12 && clip[2][2] > 0.0) {
        vec4 row_z = vec4(screen[0].z, screen[1].z, screen[2].z, screen[3].z), row_w = vec4(screen[0].w, screen[1].w, screen[2].w, screen[3].w);
        float kz = row_z[axis] / ahead[axis], kw = row_w[axis] / ahead[axis];
        v_cam = vec4(kz, row_z.w - kz * ahead.w, kw, row_w.w - kw * ahead.w);
        float nearest = 4.0 * v_cam.z - v_cam.x;        // Z / W is 4 at the nearest the GS's depth holds
        v_cam2 = vec2(64.0 / clip[2][2], abs(nearest) > 1.0e-20 ? max((v_cam.y - 4.0 * v_cam.w) / nearest, 0.0) : 0.0);
        v_soft = vec4((corner & 1) != 0 ? 1.0 : -1.0, (corner & 2) != 0 ? 1.0 : -1.0, depth, 0.5 * pixels * depth / mix(m0.x, m0.y, f));
        // drawn as a ball (the renderer says so: SOFT_FRONT)
        if ((u.flags & SOFT_FRONT) != 0) {
            gl_Position.z = soft_front(v_cam, v_cam2.y, depth, v_soft.w, int(sheet.z), z) * clip[2][2] + clip[3][2];
            v_far = gl_Position.z;
#ifdef CLIP_FAR
            gl_ClipDistance[0] = gl_Position.z;
#endif
        }
    }
}

void main()
{
    mat4 clip = u.clip;
    int light = u.light;
    if (u.merged != 0) {
        clip = D[gl_InstanceIndex].clip;
        light = D[gl_InstanceIndex].light;
    }
    if ((u.flags & PARTICLE) != 0) {
        particle(clip, light * QUADS);
        return;
    }
    // a picture between two frames: the vertex on the line through its place halfway and its own (exactly those at the half and at 1)
    vec3 place = u.between < 1.0 ? mix(a_pos_mid, a_pos, 2.0 * u.between - 1.0) : a_pos;
    v_soft = vec4(0.0);
    v_cam = vec4(0.0);
    v_cam2 = vec2(0.0);
    bool soft2d = (u.flags & SOFT_2D) != 0 && light >= 0;
    gl_Position = clip * vec4(place, 1.0) * (soft2d ? 1.0 : a_w);
    // The game's own particle as a ball of smoke: how far its middle is comes out of the sprite's own depth, by the view's numbers
    if (soft2d && clip[2][2] > 0.0) {
        vec4 cam = L[light * QUADS], more = L[light * QUADS + 1];      // more: the nearest distance, 2 ^ (shift + 2), the shift
        float zw = 2.0 + place.z * more.y;
        float under = zw * cam.z - cam.x;
        float middle = abs(under) > 1.0e-20 ? (cam.y - zw * cam.w) / under : 0.0;
        if (middle > 0.0 && a_w > 0.0) {
            v_cam = cam;
            v_cam2 = vec2(64.0 / clip[2][2], more.x);
            v_soft = vec4(a_nrm.xy, middle, a_w);
            if ((u.flags & SOFT_FRONT) != 0) gl_Position.z = soft_front(cam, more.x, middle, a_w, int(more.z), place.z) * clip[2][2] + clip[3][2];
        }
    }
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
    if (light >= 0 && (u.flags & 0x1c0) != 0) {
        int b = light * QUADS;
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
