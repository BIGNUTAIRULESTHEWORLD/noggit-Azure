// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/scoped_blp_texture_reference.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// A captured reference can span up to 96 world units laterally. Half-unit
// samples retain the native terrain alphamap detail while still allowing a
// clean zero-alpha tail to be stored outside broad dirt shoulders.
inline constexpr std::size_t ROAD_PROFILE_SAMPLE_COUNT = 193;
inline constexpr std::size_t ROAD_EXEMPLAR_SAMPLE_COUNT = 64;

struct sampled_texture_layer
{
  scoped_blp_texture_reference texture;
  float weight = 0.0f;
  std::uint32_t flags = 0;
  std::uint32_t effect_id = 0xFFFFFFFF;
  bool base_layer = false;
};

struct road_material_profile
{
  scoped_blp_texture_reference texture;
  std::array<float, ROAD_PROFILE_SAMPLE_COUNT> weights{};
  std::uint32_t flags = 0;
  std::uint32_t effect_id = 0xFFFFFFFF;
  std::array<std::array<float, ROAD_PROFILE_SAMPLE_COUNT>, ROAD_EXEMPLAR_SAMPLE_COUNT>
    exemplar_weights{};
  // Required materials must be admitted consistently by every destination
  // chunk. Current road-reference capture marks each selected road/detail
  // material required so the style cannot silently change at a chunk border.
  bool required = false;
  // Structural materials identify an already-painted road at crossings. This
  // must remain separate from palette admission: preserving every required
  // shoulder/detail layer can amplify a destination background texture into a
  // solid ribbon.
  bool structural = false;
};

struct sampled_road_style
{
  std::vector<road_material_profile> materials;
  glm::vec3 start_position{};
  glm::vec2 outward_direction{1.0f, 0.0f};
  // half_width is the conservative raster/preflight bound. UI and cursor
  // sizing use representative_half_width so a single wide captured section
  // cannot make the whole road appear wider than it will actually paint.
  float half_width = 15.0f;
  float representative_half_width = 15.0f;
  float confidence = 0.0f;
  // Left is the positive local normal (-tangent.y, tangent.x); right is the
  // negative local normal. Preview, cursor, and rasterization share this convention.
  std::array<float, ROAD_EXEMPLAR_SAMPLE_COUNT> left_half_widths{};
  std::array<float, ROAD_EXEMPLAR_SAMPLE_COUNT> right_half_widths{};
  // Fraction of the destination terrain replaced by the captured road at each
  // exemplar sample. Material weights describe the mixture inside that
  // coverage; keeping coverage explicit lets a low-alpha dirt tail reveal the
  // destination grass instead of becoming an opaque outer ribbon.
  std::array<std::array<float, ROAD_PROFILE_SAMPLE_COUNT>, ROAD_EXEMPLAR_SAMPLE_COUNT>
    exemplar_coverage{};
  float exemplar_step = 1.0f;
  // Exemplar profiles use fixed source-space lateral coordinates across this
  // extent. Keeping every row in the same metre-based coordinate system
  // preserves shoulder gaps, protrusions, and asymmetry instead of stretching
  // each row to a normalized road envelope.
  float exemplar_lateral_extent = 0.0f;
  std::size_t exemplar_sample_count = 0;
  bool has_longitudinal_exemplar = false;
};

struct sampled_road_exemplar_position
{
  std::size_t lower = 0;
  std::size_t upper = 0;
  float fraction = 0.0f;
};

inline sampled_road_exemplar_position sampled_road_exemplar_at(
  sampled_road_style const& style, float path_distance, float width_scale)
{
  sampled_road_exemplar_position sample;
  if (!style.has_longitudinal_exemplar || style.exemplar_sample_count < 2
      || style.exemplar_step <= 0.0f)
  {
    return sample;
  }

  // Width scaling represents a geometric scale of the captured road, not just
  // a squeeze of its cross-section. Evaluate the longitudinal exemplar in
  // source-road coordinates so its opacity and width details retain the same
  // proportions at every destination width.
  float const source_distance = path_distance / std::max(width_scale, 0.001f);
  float const exemplar_length = static_cast<float>(style.exemplar_sample_count - 1)
    * style.exemplar_step;
  float const repeat_length = exemplar_length * 2.0f;
  float phase = repeat_length > 0.0f ? std::fmod(source_distance, repeat_length) : 0.0f;
  if (phase < 0.0f)
  {
    phase += repeat_length;
  }
  if (phase > exemplar_length)
  {
    phase = repeat_length - phase;
  }
  float const position = phase / style.exemplar_step;
  sample.lower = std::min(static_cast<std::size_t>(std::floor(position)),
    style.exemplar_sample_count - 1);
  sample.upper = std::min(sample.lower + 1, style.exemplar_sample_count - 1);
  sample.fraction = position - static_cast<float>(sample.lower);
  return sample;
}

inline std::pair<float, float> sampled_road_widths_at(sampled_road_style const& style,
                                                      float path_distance,
                                                      float width_scale)
{
  float left = style.representative_half_width * width_scale;
  float right = left;
  if (!style.has_longitudinal_exemplar || style.exemplar_sample_count < 2
      || style.exemplar_step <= 0.0f)
  {
    return {left, right};
  }

  sampled_road_exemplar_position const sample = sampled_road_exemplar_at(
    style, path_distance, width_scale);
  left = (style.left_half_widths[sample.lower] * (1.0f - sample.fraction)
    + style.left_half_widths[sample.upper] * sample.fraction) * width_scale;
  right = (style.right_half_widths[sample.lower] * (1.0f - sample.fraction)
    + style.right_half_widths[sample.upper] * sample.fraction) * width_scale;
  if (left <= 0.0f || right <= 0.0f)
  {
    left = style.representative_half_width * width_scale;
    right = left;
  }
  return {left, right};
}

struct road_paint_result
{
  bool changed = false;
  bool blocked_by_texture_limit = false;
  std::size_t replaced_texture_layers = 0;
};
