#version 450 core

// Inputs from vertex shader
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

// Output to the frame buffer
layout(location = 0) out vec4 out_color;

// The texture sampler (set = 0, binding = 0)
layout(set = 0, binding = 0) uniform sampler2D tex0;

// Uniform buffer for color modulation (slot 0 → binding = 4)
layout(set = 0, binding = 4, std140) uniform UBO {
    vec4 colorMod;
};

void main() {
    // Sample the texture, multiply by vertex color, then apply uniform modulation
    out_color = v_color * texture(tex0, v_uv) * colorMod;
}