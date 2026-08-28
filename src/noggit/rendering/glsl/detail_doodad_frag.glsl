// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#version 330 core

in vec2 f_uv;
in vec4 f_color;
in vec3 f_normal;
in float camera_dist;

out vec4 out_color;

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

uniform sampler2DArray tex;
uniform int tex_index;
uniform float fade_dist;

void main()
{
  vec4 t = texture(tex, vec3(f_uv, tex_index));

  // the client is fully opaque up to 0.85 * groundEffectDist, then ramps to zero
  float fade = clamp((fade_dist - camera_dist) / (0.15 * fade_dist), 0.0, 1.0);
  if (t.a * fade < 128.0 / 255.0) // the client's detailDoodadAlpha alpha-key
  {
    discard;
  }

  // the vertex colour carries MCCV with the MCSH shadow term baked in; the
  // normal is the terrain facet normal, so diffuse shading follows the ground
  vec4 color = vec4(t.rgb * f_color.rgb, 1.0);

  float nDotL = clamp(dot(normalize(f_normal), -normalize(vec3(-LightDir_FogRate.x, LightDir_FogRate.z, -LightDir_FogRate.y))), 0.0, 1.0);
  vec3 ambientColor = AmbientColor_FogEnd.xyz;
  vec3 skyColor = (ambientColor * 1.10000002);
  vec3 groundColor = (ambientColor * 0.699999988);
  vec3 currColor = mix(groundColor, skyColor, 0.5 + (0.5 * nDotL));
  vec3 lDiffuse = DiffuseColor_FogStart.xyz * nDotL;

  color.rgb = clamp(color.rgb * (currColor + lDiffuse), 0.0, 1.0);

  if (FogColor_FogOn.w != 0)
  {
    float start = AmbientColor_FogEnd.w * DiffuseColor_FogStart.w;

    vec3 fogParams;
    fogParams.x = -(1.0 / (AmbientColor_FogEnd.w - start));
    fogParams.y = (1.0 / (AmbientColor_FogEnd.w - start)) * AmbientColor_FogEnd.w;
    fogParams.z = LightDir_FogRate.w;

    float f1 = (camera_dist * fogParams.x) + fogParams.y;
    float f2 = max(f1, 0.0);
    float f3 = pow(f2, fogParams.z);
    float f4 = min(f3, 1.0);

    float fogFactor = 1.0 - f4;

    color.rgb = mix(color.rgb, FogColor_FogOn.rgb, fogFactor);
  }

  out_color = color;
}
