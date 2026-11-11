#version 450
#extension GL_GOOGLE_include_directive : require
// SMAA's third pass: the picture blended along its edges.
#include "smaa/common.glsl"
#define SMAA_INCLUDE_PS 0
#include "smaa/SMAA.hlsl"
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_off;
void main()
{
    vec2 at = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1 & 1) * 2.0;
    v_uv = at;
    gl_Position = vec4(at * 2.0 - 1.0, 0.0, 1.0);
    SMAANeighborhoodBlendingVS(v_uv, v_off);
}
