#version 330 core

uniform vec4 color;
uniform vec3 origin;
uniform float radius;
uniform float inner_cursor_ratio;
uniform int liquid_brush_falloff;
uniform bool water_grid_preview;

in vec3 f_pos;
in vec3 world_pos_;

out vec4 out_color;

void main()
{
    if (water_grid_preview)
    {
      const float UNITSIZE = 533.33333 / 128.0;
      float brush_distance = length(world_pos_.xz - origin.xz);
      if (brush_distance > radius)
        discard;

      vec2 vertex_offset = abs(world_pos_.xz
        - round(world_pos_.xz / UNITSIZE) * UNITSIZE);
      float pixel_world = clamp(length(fwidth(world_pos_.xz)), 0.001,
                                UNITSIZE * 0.15);
      float grid_alpha = 1.0 - smoothstep(pixel_world * 0.65,
                                          pixel_world * 1.5,
                                          min(vertex_offset.x, vertex_offset.y));
      float vertex_alpha = 1.0 - smoothstep(pixel_world * 2.75,
                                            pixel_world * 4.0,
                                            length(vertex_offset));

      float influence = 0.0;
      float inner_radius = inner_cursor_ratio * radius;
      if (brush_distance <= inner_radius || liquid_brush_falloff == 0)
        influence = 1.0;
      else
      {
        influence = clamp(1.0 - (brush_distance - inner_radius)
                          / max(radius - inner_radius, 0.0001), 0.0, 1.0);
        if (liquid_brush_falloff == 2)
          influence = influence * influence * (3.0 - 2.0 * influence);
      }

      vec3 grid_color = mix(vec3(0.05, 0.75, 1.0),
                            vec3(1.0, 0.82, 0.08), influence);
      if (influence >= 0.999)
        grid_color = vec3(1.0, 0.35, 0.04);

      float ring_alpha = 1.0 - smoothstep(0.0, pixel_world * 1.5,
                                           abs(brush_distance - radius));
      float alpha = max(max(grid_alpha * 0.55, vertex_alpha), ring_alpha);
      if (alpha < 0.01)
        discard;
      out_color = vec4(mix(grid_color, color.rgb, ring_alpha), alpha);
      return;
    }

    bool discard_x = mod((f_pos.x + 1.) * 5., 2.) < 1.;
    bool discard_y = mod((f_pos.z + 1.) * 5., 2.) < 1.;
    // discard in a checker board pattern
    if(discard_x != discard_y)
    {
      discard;
    }

    out_color = color;
}
