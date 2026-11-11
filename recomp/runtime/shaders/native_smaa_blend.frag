#version 450
#extension GL_GOOGLE_include_directive : require
// SMAA's third pass: the picture blended with its neighbours by the weights. The picture's alpha stays: a later frame may read it.
#include "smaa/common.glsl"
#define SMAA_INCLUDE_VS 0
#include "smaa/SMAA.hlsl"
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 1, binding = 0) uniform sampler2D weights;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_off;
layout(location = 0) out vec4 o_col;
void main()
{
    o_col = vec4(SMAANeighborhoodBlendingPS(v_uv, v_off, scene, weights).rgb, texelFetch(scene, ivec2(gl_FragCoord.xy), 0).a);
}
