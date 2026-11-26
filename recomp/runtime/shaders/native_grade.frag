#version 450
// The settings' brightness, contrast and gamma on the finished picture. Its alpha is left as it is: a later frame may read it.
layout(push_constant) uniform Push {
    float brightness;   // added, -0.5..0.5
    float contrast;     // around the middle grey, 1 changes nothing
    float gamma;        // above 1 lighter, 1 changes nothing
    float opaque;       // 1: this is the picture's last pass, and its alpha goes to 1 for the window (what native_opaque.frag did)
} u;
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) out vec4 o_col;

void main()
{
    vec4 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    vec3 rgb = clamp((c.rgb - 0.5) * u.contrast + 0.5 + u.brightness, 0.0, 1.0);
    o_col = vec4(pow(rgb, vec3(1.0 / u.gamma)), u.opaque > 0.5 ? 1.0 : c.a);
}
