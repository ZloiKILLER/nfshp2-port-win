#version 450
// The native renderer: a view's rectangle, for what eDisplayFrame lays over the frame (native_post.frag).
void main()
{
    vec2 at = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1 & 1) * 4.0 - 1.0;
    gl_Position = vec4(at, 0.0, 1.0);
}
