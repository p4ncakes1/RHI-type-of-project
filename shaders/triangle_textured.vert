#version 450 core

// Vertex attributes matching renderer_pipeline_desc in main.c
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

// Outputs to fragment shader
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = vec4(in_pos, 1.0);
    v_color = in_color;
    v_uv = in_uv;
}