#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex0;

layout(std140, set = 0, binding = 1) uniform UBO
{
    vec4 colorMod;
};

void main()
{
    out_color = texture(tex0, v_uv) * colorMod;
}