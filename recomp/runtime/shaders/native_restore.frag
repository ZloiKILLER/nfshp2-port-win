#version 450
// With multisampling, the picture after an effect back into all the samples: the effect was drawn on the resolved one.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) out vec4 o_col;

void main()
{
    o_col = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
}
