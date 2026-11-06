#version 450
// The finished picture's alpha to 1 before it goes to the window (the pipeline writes alpha only). The game's alpha is its own business:
// depth for the fog, masks for the blends. A window's picture has to be opaque, or whatever shows it by its alpha (a display plane, a
// capture, an overlay) darkens it where the game left little.
layout(location = 0) out vec4 o_col;

void main()
{
    o_col = vec4(0.0, 0.0, 0.0, 1.0);
}
