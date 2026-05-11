#version 330 core
layout (location = 0) in vec3 a_pos;
layout (location = 1) in vec2 a_texcoord;

out vec2 TexCoord;

void main()
{
    gl_Position = vec4(a_pos, 1.0);
    TexCoord = vec2(a_texcoord.x, 1.0 - a_texcoord.y);
}

