#version 330 core

uniform mat4 model_view;
uniform mat4 projection;
uniform vec3 camera_position;
uniform vec3 weather_color;
uniform vec3 fog_color;
uniform float fog_start;
uniform float fog_end;
uniform float elapsed_seconds;
uniform float intensity;
uniform int weather_type;

out vec2 f_uv;
out vec3 f_color;
out float f_alpha;
flat out int f_weather_type;

float hash1(float n)
{
  return fract(sin(n * 91.3458 + 17.123) * 47453.5453);
}

void main()
{
  vec2 corners[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));
  vec2 uvs[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

  float id = float(gl_InstanceID) + 1.0;
  vec3 seed = vec3(hash1(id * 1.17), hash1(id * 2.31), hash1(id * 4.73));
  float variation = hash1(id * 8.91);
  float radius = weather_type == 3 ? 34.0 : 27.0;
  vec3 volume = vec3(radius * 2.0, weather_type == 2 ? 32.0 : 38.0,
                     radius * 2.0);
  vec3 motion = vec3(0.0);
  vec2 size;

  if (weather_type == 1) // rain
  {
    float speed = mix(17.0, 29.0, intensity) * mix(0.8, 1.2, variation);
    motion.y = -elapsed_seconds * speed;
    motion.x = elapsed_seconds * mix(0.8, 3.0, intensity);
    size = vec2(mix(0.010, 0.024, variation),
                mix(0.33, 0.78, variation) * mix(0.8, 1.2, intensity));
  }
  else if (weather_type == 2) // snow
  {
    float speed = mix(1.4, 3.6, intensity) * mix(0.75, 1.25, variation);
    motion.y = -elapsed_seconds * speed;
    motion.x = elapsed_seconds * mix(0.1, 1.4, intensity)
             + sin(elapsed_seconds * (0.6 + variation) + id) * 0.45;
    motion.z = cos(elapsed_seconds * (0.45 + seed.x) + id) * 0.35;
    size = vec2(mix(0.017, 0.051, variation));
  }
  else // sand and custom particulate weather
  {
    motion.x = elapsed_seconds * mix(4.0, 10.0, intensity)
             * mix(0.75, 1.25, variation);
    motion.y = sin(elapsed_seconds * (0.8 + variation) + id) * 0.5;
    size = vec2(mix(0.09, 0.28, variation), mix(0.05, 0.13, variation));
  }

  // Wrapping a world-anchored volume keeps particles stable while the camera
  // moves, with recycling hidden by the outer fade.
  vec3 relative = mod(seed * volume + motion - camera_position, volume)
                - volume * 0.5;
  float radial = length(relative.xz);
  vec4 view_center = model_view * vec4(camera_position + relative, 1.0);
  vec2 corner = corners[gl_VertexID] * size;

  if (weather_type == 2)
  {
    float rotation = elapsed_seconds * mix(-1.0, 1.0, variation) + id;
    float cs = cos(rotation);
    float sn = sin(rotation);
    corner = mat2(cs, -sn, sn, cs) * corner;
  }
  else if (weather_type == 1)
  {
    corner.x += corner.y * mix(0.05, 0.2, intensity);
  }

  float edge_fade = 1.0 - smoothstep(radius * 0.72, radius, radial);
  float vertical_fade = smoothstep(-volume.y * 0.5, -volume.y * 0.34, relative.y)
                      * (1.0 - smoothstep(volume.y * 0.34, volume.y * 0.5,
                                          relative.y));
  float near_fade = smoothstep(2.3, 4.2, radial);
  float fog_amount = clamp((length(relative) - fog_start)
                         / max(1.0, fog_end - fog_start), 0.0, 1.0);

  f_uv = uvs[gl_VertexID];
  f_color = mix(weather_color, fog_color, fog_amount);
  f_alpha = sqrt(intensity) * edge_fade * vertical_fade * near_fade
          * mix(0.6, 1.0, variation) * (1.0 - 0.65 * fog_amount);
  f_weather_type = weather_type;
  gl_Position = projection * (view_center + vec4(corner, 0.0, 0.0));
}
