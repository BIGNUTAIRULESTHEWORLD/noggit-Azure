// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 410 core

const float TILESIZE = 533.33333f;
const float CHUNKSIZE = TILESIZE / 16.0f;
const float UNITSIZE = CHUNKSIZE / 8.0f;

layout (std140) uniform lighting
{
  vec4 DiffuseColor_FogStart;
  vec4 AmbientColor_FogEnd;
  vec4 FogColor_FogOn;
  vec4 LightDir_FogRate;
  vec4 OceanColorLight;
  vec4 OceanColorDark;
  vec4 RiverColorLight;
  vec4 RiverColorDark;
};

uniform float animtime;
uniform sampler2DArray texture_samplers[14] ;
uniform int draw_cursor_circle;
uniform vec3 cursor_position;
uniform float outer_cursor_radius;
uniform float inner_cursor_ratio;
uniform vec4 cursor_color;
uniform bool show_liquid_vertex_grid;
uniform int selected_surface_token_low;
uniform int selected_surface_token_high;
uniform int liquid_brush_falloff;
uniform int liquid_attribute_overlay;

in float depth_;
in vec2 tex_coord_;
in float dist_from_camera_;
in vec3 world_pos_;
in vec2 liquid_grid_pos_;
flat in uvec2 surface_token_;
flat in uvec2 fishable_mask_;
flat in uvec2 fatigue_mask_;
flat in uint tex_array;
flat in uint type;
flat in vec2 anim_uv;
flat in int tex_frame;

out vec4 out_color;

vec4 get_tex_color(vec2 tex_coord, uint tex_sampler, int array_index)
{
  if (tex_sampler == 0)
  {
    return texture(texture_samplers[0], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 1)
  {
    return texture(texture_samplers[1], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 2)
  {
    return texture(texture_samplers[2], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 3)
  {
    return texture(texture_samplers[3], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 4)
  {
    return texture(texture_samplers[4], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 5)
  {
    return texture(texture_samplers[5], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 6)
  {
    return texture(texture_samplers[6], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 7)
  {
    return texture(texture_samplers[7], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 8)
  {
    return texture(texture_samplers[8], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 9)
  {
    return texture(texture_samplers[9], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 10)
  {
    return texture(texture_samplers[10], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 11)
  {
    return texture(texture_samplers[11], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 12)
  {
    return texture(texture_samplers[12], vec3(tex_coord, array_index)).rgba;
  }
  else if (tex_sampler == 13)
  {
    return texture(texture_samplers[13], vec3(tex_coord, array_index)).rgba;
  }

  return vec4(0);
}

vec2 rot2(vec2 p, float degree)
{
  float a = radians(degree);
  return mat2(cos(a), -sin(a), sin(a), cos(a))*p;
}

bool maskBit(uvec2 mask, uint bit_index)
{
  uint word = bit_index < 32u ? mask.x : mask.y;
  return ((word >> (bit_index % 32u)) & 1u) != 0u;
}

void main()
{
  // lava || slime
  if(type == 2 || type == 3)
  {
    out_color = get_tex_color(tex_coord_ + vec2(anim_uv.x*animtime / 2880.0, anim_uv.y*animtime / 2880.0), tex_array, tex_frame);
  }
  else
  {
    vec2 uv = rot2(tex_coord_ * anim_uv.x, anim_uv.y);

    vec4 texel = get_tex_color(uv, tex_array, tex_frame);
    vec4 lerp = (type == 1)
              ? mix (OceanColorLight, OceanColorDark, depth_)
              : mix (RiverColorLight, RiverColorDark, depth_)
              ;

    //clamp shouldn't be needed
    out_color = vec4 (clamp(texel + lerp, 0.0, 1.0).rgb, lerp.a);
  }

  if (FogColor_FogOn.w != 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start));
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (dist_from_camera_ * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    out_color.rgb = mix(out_color.rgb, FogColor_FogOn.rgb, fogFactor);
  }

  if (draw_cursor_circle == 1)
  {
    float distance_from_center = length(world_pos_.xz - cursor_position.xz);
    float distance_from_line = abs(distance_from_center - outer_cursor_radius);
    if (inner_cursor_ratio > 0.0)
    {
      distance_from_line = min(distance_from_line,
                               abs(distance_from_center
                                   - outer_cursor_radius * inner_cursor_ratio));
    }

    float line_width = max(length(fwidth(world_pos_.xz)), 0.02);
    float line_alpha = 1.0 - smoothstep(0.0, line_width, distance_from_line);
    line_alpha *= cursor_color.a;
    out_color.rgb = mix(out_color.rgb, cursor_color.rgb, line_alpha);
    out_color.a = max(out_color.a, line_alpha);
  }

  bool token_selection = selected_surface_token_low != 0
                      || selected_surface_token_high != 0;
  bool surface_matches = !token_selection
                      || all(equal(surface_token_,
                                   uvec2(uint(selected_surface_token_low),
                                         uint(selected_surface_token_high))));
  if ((show_liquid_vertex_grid || liquid_attribute_overlay != 0) && surface_matches)
  {
    float brush_distance = length(world_pos_.xz - cursor_position.xz);
    // Vertex-edit context follows the brush. Attribute editing is a map-style
    // overlay, so keep the 8x8 cells visible across every rendered surface.
    float context_alpha = liquid_attribute_overlay != 0
                        ? 1.0
                        : 1.0 - smoothstep(outer_cursor_radius + UNITSIZE,
                                           outer_cursor_radius + 2.0 * UNITSIZE,
                                           brush_distance);

    vec2 nearest_vertex = round(liquid_grid_pos_);
    vec2 offset_from_vertex = abs(liquid_grid_pos_ - nearest_vertex) * UNITSIZE;
    float distance_from_grid = min(offset_from_vertex.x, offset_from_vertex.y);
    float distance_from_vertex = length(offset_from_vertex);

    float pixel_world = clamp(length(fwidth(world_pos_.xz)), 0.001, UNITSIZE * 0.15);
    float grid_alpha = 1.0 - smoothstep(pixel_world * 0.65,
                                        pixel_world * 1.5,
                                        distance_from_grid);
    float vertex_alpha = 1.0 - smoothstep(pixel_world * 2.75,
                                          pixel_world * 4.0,
                                          distance_from_vertex);

    float influence = 0.0;
    if (brush_distance < outer_cursor_radius)
    {
      float inner_radius = inner_cursor_ratio * outer_cursor_radius;
      if (brush_distance <= inner_radius || liquid_brush_falloff == 0)
      {
        influence = 1.0;
      }
      else
      {
        float span = max(outer_cursor_radius - inner_radius, 0.0001);
        influence = clamp(1.0 - (brush_distance - inner_radius) / span, 0.0, 1.0);
        if (liquid_brush_falloff == 2)
          influence = influence * influence * (3.0 - 2.0 * influence);
      }
    }

    if (show_liquid_vertex_grid)
    {
      vec3 grid_color;
      float overlay_alpha;
      if (liquid_attribute_overlay != 0)
      {
        // Attribute overlays use a quiet, neutral grid. The grid describes
        // cell boundaries only; flag state is rendered independently below.
        grid_color = vec3(0.82);
        overlay_alpha = max(grid_alpha * 0.18, vertex_alpha * 0.32);
      }
      else
      {
        grid_color = mix(vec3(0.05, 0.75, 1.0),
                         vec3(1.0, 0.82, 0.08), influence);
        if (influence >= 0.999)
          grid_color = vec3(1.0, 0.35, 0.04);
        overlay_alpha = max(grid_alpha * 0.55, vertex_alpha) * context_alpha;
      }

      out_color.rgb = mix(out_color.rgb, grid_color, overlay_alpha);
      out_color.a = max(out_color.a, overlay_alpha);
    }

    if (liquid_attribute_overlay != 0)
    {
      uvec2 cell = uvec2(clamp(floor(liquid_grid_pos_), vec2(0.0), vec2(7.0)));
      uint bit_index = cell.y * 8u + cell.x;
      bool flagged = liquid_attribute_overlay == 1
                   ? maskBit(fishable_mask_, bit_index)
                   : maskBit(fatigue_mask_, bit_index);
      if (flagged)
      {
        vec3 flag_color = liquid_attribute_overlay == 1
                        ? vec3(0.05, 1.0, 0.55)
                        : vec3(1.0, 0.18, 0.04);
        float flag_alpha = 0.42 * context_alpha;
        out_color.rgb = mix(out_color.rgb, flag_color, flag_alpha);
        out_color.a = max(out_color.a, flag_alpha);
      }
    }
  }
}
