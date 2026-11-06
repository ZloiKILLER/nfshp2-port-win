#version 450
// The native renderer: a view folded into the sphere map (CreateEnvMap, light.md's "The sphere map").
layout(location = 0) in vec2 a_st;
layout(location = 1) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;

void main()
{
    gl_Position = vec4(a_st * 2.0 - 1.0, 0.0, 1.0);
    v_uv = a_uv;
}
