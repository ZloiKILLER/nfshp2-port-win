// SMAA (shaders/smaa/SMAA.hlsl, its GLSL path) for the native renderer's finished picture. Vulkan's texture coordinates start at the top
// left as Direct3D's do, so the area and search textures go in as they are. The preset is the push constants', not the build's.
layout(push_constant) uniform Push {
    vec4 rt;            // 1 / width, 1 / height, width, height
    float threshold;
    int steps, diag, corner;
} u;
#define SMAA_RT_METRICS u.rt
#define SMAA_GLSL_4 1
#define SMAA_THRESHOLD u.threshold
#define SMAA_MAX_SEARCH_STEPS u.steps
#define SMAA_MAX_SEARCH_STEPS_DIAG u.diag
#define SMAA_CORNER_ROUNDING u.corner
