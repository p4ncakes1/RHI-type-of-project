#version 450

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 p = vec2(
        (gl_VertexIndex & 1) * 4.0 - 1.0,
        (gl_VertexIndex & 2) * 2.0 - 1.0
    );

    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}