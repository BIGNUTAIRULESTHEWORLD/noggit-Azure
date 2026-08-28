// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec3 position;
in vec3 normal;
in vec4 color;
in vec2 uv;

out vec2 f_uv;
out vec4 f_color;
out vec3 f_normal;
out float camera_dist;

layout (std140) uniform matrices
{
  mat4 model_view;
  mat4 projection;
};

void main()
{
  vec4 view_pos = model_view * vec4(position, 1.0);
  f_uv = uv;
  f_color = color;
  f_normal = normal;
  camera_dist = length(view_pos.xyz);
  gl_Position = projection * view_pos;
}
