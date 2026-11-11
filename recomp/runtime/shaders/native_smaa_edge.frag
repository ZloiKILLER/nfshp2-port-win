#version 450
#extension GL_GOOGLE_include_directive : require
// SMAA's first pass: where the picture's edges are, by luma.
#include "smaa/common.glsl"
#define SMAA_INCLUDE_VS 0
#include "smaa/SMAA.hlsl"
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_off[3];
layout(location = 0) out vec4 o_col;
void main() { o_col = vec4(SMAALumaEdgeDetectionPS(v_uv, v_off, scene), 0.0, 0.0); }
