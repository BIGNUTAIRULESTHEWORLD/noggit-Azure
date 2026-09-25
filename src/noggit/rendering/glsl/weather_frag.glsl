#version 330 core

in vec2 f_uv;
in vec3 f_color;
in float f_alpha;
flat in int f_weather_type;

uniform sampler2DArray weather_texture;
uniform int weather_texture_index;
uniform int use_weather_texture;

out vec4 out_color;

void main()
{
  vec3 particle_color = f_color;
  float shape;

  if (use_weather_texture != 0)
  {
    vec4 sprite = texture(weather_texture, vec3(f_uv, weather_texture_index));
    particle_color *= sprite.rgb;
    shape = sprite.a;
  }
  else
  {
    vec2 centered = f_uv * 2.0 - 1.0;
    if (f_weather_type == 1)
    {
      float core = 1.0 - smoothstep(0.10, 0.7, abs(centered.x));
      float ends = smoothstep(0.0, 0.15, f_uv.y)
                 * smoothstep(0.0, 0.15, 1.0 - f_uv.y);
      shape = core * ends;
    }
    else if (f_weather_type == 2)
    {
      float radius = length(centered);
      float spoke = abs(cos(3.0 * atan(centered.y, centered.x)));
      float outline = mix(0.47, 0.78, pow(spoke, 7.0));
      shape = 1.0 - smoothstep(outline - 0.12, outline, radius);
    }
    else
    {
      float radius = length(centered);
      shape = 1.0 - smoothstep(0.1, 0.9, radius);
    }
  }

  float family_alpha = f_weather_type == 3 ? 0.28
                     : (f_weather_type == 2 ? 0.72 : 0.67);
  float alpha = shape * f_alpha * family_alpha;
  if (alpha < 0.006)
    discard;
  out_color = vec4(particle_color, alpha);
}
