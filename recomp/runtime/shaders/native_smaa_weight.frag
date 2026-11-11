#version 450
#extension GL_GOOGLE_include_directive : require
// SMAA's second pass: the blending weights, from the edges and the two tables.
#include "smaa/common.glsl"
#define SMAA_INCLUDE_VS 0
#include "smaa/SMAA.hlsl"
layout(set = 0, binding = 0) uniform sampler2D edges;
layout(set = 1, binding = 0) uniform sampler2D area;
layout(set = 2, binding = 0) uniform sampler2D search;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pix;
layout(location = 2) in vec4 v_off[3];
layout(location = 0) out vec4 o_col;
void main() { o_col = SMAABlendingWeightCalculationPS(v_uv, v_pix, v_off, edges, area, search, vec4(0.0)); }
