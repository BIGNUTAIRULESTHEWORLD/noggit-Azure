// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "MapStampAsset.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Alphamap.hpp>
#include <noggit/DetailDoodads.hpp>
#include <noggit/MapChunk.h>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/texture_set.hpp>

#include <math/ray.hpp>

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QSaveFile>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <utility>

using namespace Noggit::Ui::Tools::Stamp;

namespace
{
  constexpr quint16 format_version = 7;
  constexpr quint16 legacy_resolution = 65;
  constexpr quint32 maximum_textures = 128;
  constexpr qint64 maximum_file_size = 256ll * 1024ll * 1024ll;
  constexpr int maximum_height_resolution = 1025;
  constexpr int maximum_texture_resolution = 2049;
  constexpr double macro_fit_inner_radius = .8;
  constexpr float capture_extent = 1.f;
  constexpr float painted_capture_extent = 1.2f;
  constexpr float maximum_stored_extent = 1.5f;

  int captureResolution(float radius, float spacing, int maximum)
  {
    int result = std::max(3, static_cast<int>(std::ceil(2.f * radius / spacing)) + 1);
    if ((result & 1) == 0)
      ++result;
    return std::min(result, maximum);
  }

  void configureStream(QDataStream& stream)
  {
    stream.setVersion(QDataStream::Qt_5_12);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  }

  void writeString(QDataStream& stream, std::string const& value)
  {
    QByteArray const bytes(value.data(), static_cast<int>(value.size()));
    stream << static_cast<quint32>(bytes.size());
    if (!bytes.isEmpty())
      stream.writeRawData(bytes.constData(), bytes.size());
  }

  bool readString(QDataStream& stream, std::string& value)
  {
    quint32 size = 0;
    stream >> size;
    if (stream.status() != QDataStream::Ok || size > 1024 * 1024)
      return false;
    QByteArray bytes(static_cast<int>(size), Qt::Uninitialized);
    if (size && stream.readRawData(bytes.data(), static_cast<int>(size)) != static_cast<int>(size))
      return false;
    value.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    return true;
  }

  float shapeDistance(float nx, float nz, MapStampShape shape)
  {
    return shape == MapStampShape::Circle ? std::sqrt(nx * nx + nz * nz)
                                          : std::max(std::abs(nx), std::abs(nz));
  }

  float rotatedFootprintBoundingRadius(float radius, float rotation_degrees,
                                       MapStampShape shape)
  {
    if (shape == MapStampShape::Circle)
      return radius;
    float const radians = glm::radians(rotation_degrees);
    return radius * (std::abs(std::cos(radians)) + std::abs(std::sin(radians)));
  }

  float coverageAt(float nx, float nz, float hardness, float opacity, MapStampShape shape)
  {
    float const distance = shapeDistance(nx, nz, shape);
    if (distance >= 1.f)
      return 0.f;
    hardness = std::clamp(hardness, 0.f, .999f);
    float const falloff = distance <= hardness ? 1.f
      : 1.f - (distance - hardness) / (1.f - hardness);
    return std::clamp(falloff * opacity, 0.f, 1.f);
  }

  float stampCoverageAt(float nx, float nz, float hardness, float opacity,
                        MapStampShape shape, MapStampHeightMode height_mode)
  {
    if (height_mode == MapStampHeightMode::ConformToTerrain)
      return coverageAt(nx, nz, hardness, opacity, shape);

    // Exact-feature stamps preserve the complete selected footprint. Their transition lives
    // outside that core, in context sampled during capture, so a feature touching the selection
    // boundary is not silently softened.
    float const distance = shapeDistance(nx, nz, shape);
    float const blend = std::clamp(1.f - hardness, .05f, .5f);
    if (distance <= 1.f)
      return std::clamp(opacity, 0.f, 1.f);
    if (distance >= 1.f + blend)
      return 0.f;
    return std::clamp((1.f - (distance - 1.f) / blend) * opacity, 0.f, 1.f);
  }

  float placementExtent(float hardness, MapStampHeightMode height_mode)
  {
    return height_mode != MapStampHeightMode::ConformToTerrain
        ? 1.f + std::clamp(1.f - hardness, .05f, .5f) : 1.f;
  }

  float paintedExactSkirtBlend(float outside_distance, float radius, float hardness,
                               float opacity, float boundary_displacement)
  {
    if (!std::isfinite(outside_distance) || radius <= 0.f)
      return 0.f;

    // Preserve the requested edge blend as a minimum, but give a tall painted
    // boundary enough horizontal room to meet the destination without forming
    // a near-vertical terrain curtain. The half-radius cap keeps the generated
    // support local to the selected feature.
    float const requested_width = std::clamp(1.f - hardness, .05f, .5f);
    float const slope_limited_width = std::abs(boundary_displacement) / radius;
    float const width = std::clamp(std::max(requested_width, slope_limited_width),
                                   requested_width, .5f);
    if (outside_distance >= width)
      return 0.f;
    float const t = std::clamp(1.f - outside_distance / width, 0.f, 1.f);
    float const smooth = t * t * (3.f - 2.f * t);
    return smooth * std::clamp(opacity, 0.f, 1.f);
  }

  float heightContributionTextureBlend(float coverage, float height_contribution)
  {
    // Source texture should disappear before its generated terrain contribution
    // becomes visually flat. This retains blending at the feature foot without
    // leaving detached source-colored patches on untouched destination terrain.
    float const remaining_relief = std::abs(height_contribution) * coverage;
    float const visible_relief = std::max(1.f, UNITSIZE * .15f);
    float const t = std::clamp(remaining_relief / visible_relief, 0.f, 1.f);
    float const relief_gate = t * t * (3.f - 2.f * t);
    return coverage * relief_gate;
  }

  glm::vec2 sourceCoordinates(float world_x, float world_z, glm::vec3 const& center,
                              float radius, float rotation_degrees)
  {
    float const dx = (world_x - center.x) / radius;
    float const dz = (world_z - center.z) / radius;
    float const radians = glm::radians(-rotation_degrees);
    float const cosine = std::cos(radians);
    float const sine = std::sin(radians);
    float const nx = cosine * dx - sine * dz;
    float const nz = sine * dx + cosine * dz;
    return {(nx + 1.f) * .5f, (nz + 1.f) * .5f};
  }

  glm::vec2 clampToShapeBoundary(glm::vec2 const& uv, MapStampShape shape)
  {
    float nx = uv.x * 2.f - 1.f;
    float nz = uv.y * 2.f - 1.f;
    float const distance = shapeDistance(nx, nz, shape);
    if (distance > 1.f)
    {
      nx /= distance;
      nz /= distance;
    }
    return {(nx + 1.f) * .5f, (nz + 1.f) * .5f};
  }

  float medianValue(std::vector<float> values)
  {
    if (values.empty())
      return 0.f;
    auto const middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    float result = *middle;
    if ((values.size() & 1u) == 0)
    {
      auto const lower = std::max_element(values.begin(), middle);
      result = (*lower + result) * .5f;
    }
    return result;
  }

  std::array<float, 4> chunkWeightsAt(TextureSet* texture_set, int pixel)
  {
    std::array<float, 4> weights{};
    std::size_t const count = std::min<std::size_t>(4, texture_set->num());
    auto const& temporary = texture_set->getTempAlphamaps();
    if (temporary)
    {
      for (std::size_t layer = 0; layer < count; ++layer)
        weights[layer] = (*temporary)[layer][pixel];
      return weights;
    }

    float base = 255.f;
    auto const& alphamaps = *texture_set->getAlphamaps();
    for (std::size_t layer = 1; layer < count; ++layer)
    {
      weights[layer] = alphamaps[layer - 1]
        ? static_cast<float>(alphamaps[layer - 1]->getAlpha(pixel)) : 0.f;
      base -= weights[layer];
    }
    if (count)
      weights[0] = std::max(0.f, base);
    return weights;
  }

  std::vector<std::size_t> selectStampTextureLayers(
      std::vector<std::array<float, 64 * 64>> const& mixed,
      std::vector<double> const& protected_totals)
  {
    struct layer_score
    {
      std::size_t index = 0;
      double total = 0.0;
      double border_total = 0.0;
      float maximum = 0.f;
      float border_maximum = 0.f;
      int dominant_samples = 0;
      int strong_samples = 0;
    };

    std::vector<layer_score> scores(mixed.size());
    for (std::size_t layer = 0; layer < scores.size(); ++layer)
      scores[layer].index = layer;

    for (int z = 0; z < 64; ++z)
      for (int x = 0; x < 64; ++x)
      {
        int const pixel = z * 64 + x;
        bool const border = x == 0 || x == 63 || z == 0 || z == 63;
        std::size_t dominant = 0;
        float dominant_weight = 0.f;
        for (std::size_t layer = 0; layer < mixed.size(); ++layer)
        {
          float const weight = mixed[layer][pixel];
          layer_score& score = scores[layer];
          score.total += weight;
          score.maximum = std::max(score.maximum, weight);
          if (weight >= 1.f)
            ++score.strong_samples;
          if (border)
          {
            score.border_total += weight;
            score.border_maximum = std::max(score.border_maximum, weight);
          }
          if (weight > dominant_weight)
          {
            dominant = layer;
            dominant_weight = weight;
          }
        }
        if (dominant_weight >= 1.f)
          ++scores[dominant].dominant_samples;
      }

    std::vector<std::size_t> selected;
    selected.reserve(std::min<std::size_t>(4, scores.size()));
    for (layer_score const& score : scores)
      if (score.total >= 1.f)
        selected.push_back(score.index);

    // Preserve the existing order when the chunk fits naturally. Besides being
    // less invasive, this keeps the destination base layer stable.
    if (selected.size() <= 4)
      return selected;

    auto importance = [&](std::size_t layer)
    {
      layer_score const& score = scores[layer];
      // A chunk has room for only four texture identities. Protect layers that
      // keep untouched terrain intact and layers visible on shared chunk edges,
      // then account for coherent local features instead of relying only on the
      // integrated chunk total. The latter is what allowed broad grass to erase
      // a locally opaque rock texture.
      return protected_totals[layer] * 8.0
        + score.border_total * 32.0
        + static_cast<double>(score.border_maximum) * 4096.0
        + static_cast<double>(score.dominant_samples) * 4096.0
        + static_cast<double>(score.strong_samples) * 64.0
        + static_cast<double>(score.maximum) * 512.0
        + score.total * 0.05;
    };

    std::stable_sort(selected.begin(), selected.end(), [&](std::size_t lhs, std::size_t rhs)
    {
      return importance(lhs) > importance(rhs);
    });
    selected.resize(4);
    std::sort(selected.begin(), selected.end());
    return selected;
  }

  std::optional<float> terrainHeightAt(MapChunk* chunk, glm::vec3 const& position)
  {
    if (!chunk)
      return std::nullopt;
    selection_result hits;
    math::ray const ray({position.x, chunk->getMaxHeight() + 1.f, position.z},
                        {0.f, -1.f, 0.f});
    chunk->intersect(ray, &hits, true);
    if (hits.empty() || hits.front().second.index() != eEntry_MapChunk)
      return std::nullopt;
    return std::get<selected_chunk_type>(hits.front().second).position.y;
  }

  void normalizeHeightDatum(std::vector<float>& heights, int resolution, MapStampShape shape,
                            float sample_extent = 1.f)
  {
    std::vector<float> perimeter;
    perimeter.reserve(static_cast<std::size_t>(resolution) * 2);
    for (int y = 0; y < resolution; ++y)
      for (int x = 0; x < resolution; ++x)
      {
        float const nx = (static_cast<float>(x) / (resolution - 1) * 2.f - 1.f)
            * sample_extent;
        float const nz = (static_cast<float>(y) / (resolution - 1) * 2.f - 1.f)
            * sample_extent;
        float const distance = shapeDistance(nx, nz, shape);
        if (distance >= .8f && distance <= 1.f)
          perimeter.push_back(heights[y * resolution + x]);
      }
    if (perimeter.empty())
      return;
    auto const middle = perimeter.begin() + perimeter.size() / 2;
    std::nth_element(perimeter.begin(), middle, perimeter.end());
    float const datum = *middle;
    for (float& height : heights)
      height -= datum;
  }

  constexpr int quadratic_coefficient_count = 6;
  using QuadraticCoefficients = std::array<double, quadratic_coefficient_count>;
  using QuadraticSystem = std::array<
      std::array<double, quadratic_coefficient_count + 1>, quadratic_coefficient_count>;

  std::array<double, quadratic_coefficient_count> quadraticBasis(double nx, double nz)
  {
    return {1.0, nx, nz, nx * nx, nx * nz, nz * nz};
  }

  void addQuadraticSample(QuadraticSystem& system, double nx, double nz, double height,
                          double weight)
  {
    auto const basis = quadraticBasis(nx, nz);
    for (int row = 0; row < quadratic_coefficient_count; ++row)
    {
      for (int column = 0; column < quadratic_coefficient_count; ++column)
        system[row][column] += weight * basis[row] * basis[column];
      system[row][quadratic_coefficient_count] += weight * basis[row] * height;
    }
  }

  std::optional<QuadraticCoefficients> solveQuadratic(QuadraticSystem system)
  {
    for (int column = 0; column < quadratic_coefficient_count; ++column)
    {
      int pivot = column;
      for (int row = column + 1; row < quadratic_coefficient_count; ++row)
        if (std::abs(system[row][column]) > std::abs(system[pivot][column]))
          pivot = row;
      if (std::abs(system[pivot][column]) < 1e-9)
        return std::nullopt;
      std::swap(system[pivot], system[column]);
      double const divisor = system[column][column];
      for (int entry = column; entry <= quadratic_coefficient_count; ++entry)
        system[column][entry] /= divisor;
      for (int row = 0; row < quadratic_coefficient_count; ++row)
      {
        if (row == column)
          continue;
        double const factor = system[row][column];
        for (int entry = column; entry <= quadratic_coefficient_count; ++entry)
          system[row][entry] -= factor * system[column][entry];
      }
    }
    QuadraticCoefficients coefficients{};
    for (int i = 0; i < quadratic_coefficient_count; ++i)
      coefficients[i] = system[i][quadratic_coefficient_count];
    return coefficients;
  }

  double evaluateQuadratic(QuadraticCoefficients const& coefficients, double nx, double nz)
  {
    auto const basis = quadraticBasis(nx, nz);
    double result = 0.0;
    for (int i = 0; i < quadratic_coefficient_count; ++i)
      result += coefficients[i] * basis[i];
    return result;
  }

  void detrendHeightRelief(std::vector<float>& heights, int resolution, MapStampShape shape,
                           float sample_extent = 1.f)
  {
    QuadraticSystem system{};
    std::size_t samples = 0;
    for (int y = 0; y < resolution; ++y)
      for (int x = 0; x < resolution; ++x)
      {
        double const nx = (static_cast<double>(x) / (resolution - 1) * 2.0 - 1.0)
            * sample_extent;
        double const nz = (static_cast<double>(y) / (resolution - 1) * 2.0 - 1.0)
            * sample_extent;
        double const distance = shapeDistance(static_cast<float>(nx), static_cast<float>(nz), shape);
        // The outer band represents the ground the captured feature grows from. Fitting the
        // whole footprint would incorrectly classify a broad foothill or plateau as part of
        // the base surface and remove most of its saved height.
        if (distance >= macro_fit_inner_radius && distance <= 1.0)
        {
          addQuadraticSample(system, nx, nz, heights[y * resolution + x], 1.0);
          ++samples;
        }
      }
    std::optional<QuadraticCoefficients> const coefficients =
        samples >= quadratic_coefficient_count ? solveQuadratic(system) : std::nullopt;
    if (!coefficients)
    {
      normalizeHeightDatum(heights, resolution, shape, sample_extent);
      return;
    }
    for (int y = 0; y < resolution; ++y)
      for (int x = 0; x < resolution; ++x)
      {
        double const nx = (static_cast<double>(x) / (resolution - 1) * 2.0 - 1.0)
            * sample_extent;
        double const nz = (static_cast<double>(y) / (resolution - 1) * 2.0 - 1.0)
            * sample_extent;
        if (shapeDistance(static_cast<float>(nx), static_cast<float>(nz), shape) > 1.f)
        {
          heights[y * resolution + x] = 0.f;
          continue;
        }
        double const macro_height = evaluateQuadratic(*coefficients, nx, nz);
        heights[y * resolution + x] -= static_cast<float>(macro_height);
      }
  }

  void detrendPaintedHeightRelief(std::vector<float>& heights, int resolution,
                                  std::vector<std::uint8_t> const& footprint,
                                  std::vector<float> const& outside_distances,
                                  float sample_extent)
  {
    if (footprint.size() != heights.size() || outside_distances.size() != heights.size())
    {
      detrendHeightRelief(heights, resolution, MapStampShape::Square, sample_extent);
      return;
    }
    QuadraticSystem system{};
    std::size_t samples = 0;
    constexpr float context_width = painted_capture_extent - 1.f;
    for (int y = 0; y < resolution; ++y)
      for (int x = 0; x < resolution; ++x)
      {
        std::size_t const index = static_cast<std::size_t>(y) * resolution + x;
        // Only the automatic collar may define the source ground plane. Relief inside the
        // user's painted feature is content, not evidence about the terrain beneath it.
        if (footprint[index] >= 128 || outside_distances[index] > context_width)
          continue;
        double const nx = (static_cast<double>(x) / (resolution - 1) * 2.0 - 1.0)
            * sample_extent;
        double const nz = (static_cast<double>(y) / (resolution - 1) * 2.0 - 1.0)
            * sample_extent;
        addQuadraticSample(system, nx, nz, heights[index], 1.0);
        ++samples;
      }
    std::optional<QuadraticCoefficients> const coefficients =
        samples >= quadratic_coefficient_count ? solveQuadratic(system) : std::nullopt;
    if (!coefficients)
    {
      detrendHeightRelief(heights, resolution, MapStampShape::Square, sample_extent);
      return;
    }
    for (int y = 0; y < resolution; ++y)
      for (int x = 0; x < resolution; ++x)
      {
        double const nx = (static_cast<double>(x) / (resolution - 1) * 2.0 - 1.0)
            * sample_extent;
        double const nz = (static_cast<double>(y) / (resolution - 1) * 2.0 - 1.0)
            * sample_extent;
        heights[static_cast<std::size_t>(y) * resolution + x]
            -= static_cast<float>(evaluateQuadratic(*coefficients, nx, nz));
      }
  }

  std::optional<QuadraticCoefficients> fitTargetMacroSurface(
      World* world, glm::vec3 const& center, float radius, float rotation_degrees,
      MapStampShape shape, std::function<bool(float, float)> include_sample = {},
      bool reject_raised_features = false)
  {
    struct TargetSample
    {
      double nx;
      double nz;
      double height;
    };
    std::vector<TargetSample> samples;
    float const traversal_radius = rotatedFootprintBoundingRadius(
        radius * (shape == MapStampShape::Painted ? painted_capture_extent : 1.f),
        rotation_degrees, shape);
    world->for_all_chunks_in_rect(center, traversal_radius, [&](MapChunk* chunk)
    {
      for (glm::vec3 const& vertex : chunk->mVertices)
      {
        glm::vec2 const uv = sourceCoordinates(vertex.x, vertex.z, center, radius,
                                                rotation_degrees);
        double const nx = uv.x * 2.0 - 1.0;
        double const nz = uv.y * 2.0 - 1.0;
        double const distance = shapeDistance(static_cast<float>(nx), static_cast<float>(nz), shape);
        // Match the stamp to the terrain around its border. Interior destination bumps must
        // not pull the fitted base upward or turn a saved flat-topped feature into a peak.
        bool const included = include_sample
            ? include_sample(static_cast<float>(nx), static_cast<float>(nz))
            : distance >= macro_fit_inner_radius && distance <= 1.0;
        if (!included)
          continue;
        samples.push_back({nx, nz, vertex.y});
      }
      return false;
    });
    if (samples.size() < quadratic_coefficient_count)
      return std::nullopt;

    auto fit = [&](std::vector<std::uint8_t> const* included)
        -> std::optional<QuadraticCoefficients>
    {
      QuadraticSystem system{};
      std::size_t count = 0;
      for (std::size_t index = 0; index < samples.size(); ++index)
      {
        if (included && !(*included)[index])
          continue;
        TargetSample const& sample = samples[index];
        addQuadraticSample(system, sample.nx, sample.nz, sample.height, 1.0);
        ++count;
      }
      return count >= quadratic_coefficient_count
          ? solveQuadratic(system) : std::optional<QuadraticCoefficients>{};
    };

    std::optional<QuadraticCoefficients> coefficients = fit(nullptr);
    if (!coefficients || !reject_raised_features)
      return coefficients;

    // A previously placed mountain can occupy part of the collar used to find
    // the destination ground. Iteratively reject only strong positive residuals;
    // ordinary lower terrain and broad slopes remain valid fitting evidence.
    for (int iteration = 0; iteration < 3; ++iteration)
    {
      std::vector<float> residuals;
      residuals.reserve(samples.size());
      for (TargetSample const& sample : samples)
        residuals.push_back(static_cast<float>(sample.height
            - evaluateQuadratic(*coefficients, sample.nx, sample.nz)));
      float const residual_median = medianValue(residuals);
      std::vector<float> deviations;
      deviations.reserve(residuals.size());
      for (float residual : residuals)
        deviations.push_back(std::abs(residual - residual_median));
      float const robust_deviation = medianValue(std::move(deviations)) * 1.4826f;
      float const upper_limit = residual_median
          + std::max(UNITSIZE * .15f, robust_deviation * 2.5f);
      std::vector<std::uint8_t> included(samples.size(), 0);
      for (std::size_t index = 0; index < residuals.size(); ++index)
        included[index] = residuals[index] <= upper_limit ? 1 : 0;
      std::optional<QuadraticCoefficients> const refined = fit(&included);
      if (!refined)
        break;
      coefficients = refined;
    }
    return coefficients;
  }

  struct ProtectionGrid
  {
    int resolution = 0;
    std::vector<float> values;

    float sample(float u, float v) const
    {
      if (resolution < 2 || values.empty())
        return 0.f;
      float const fx = std::clamp(u, 0.f, 1.f) * (resolution - 1);
      float const fy = std::clamp(v, 0.f, 1.f) * (resolution - 1);
      int const x0 = static_cast<int>(std::floor(fx));
      int const y0 = static_cast<int>(std::floor(fy));
      int const x1 = std::min(x0 + 1, resolution - 1);
      int const y1 = std::min(y0 + 1, resolution - 1);
      float const tx = fx - x0;
      float const ty = fy - y0;
      float const top = std::lerp(values[y0 * resolution + x0],
                                  values[y0 * resolution + x1], tx);
      float const bottom = std::lerp(values[y1 * resolution + x0],
                                     values[y1 * resolution + x1], tx);
      return std::lerp(top, bottom, ty);
    }

  };

  float protectionRamp(float value, float start, float full)
  {
    if (value <= start)
      return 0.f;
    if (value >= full || full <= start)
      return 1.f;
    float const t = (value - start) / (full - start);
    return t * t * (3.f - 2.f * t);
  }

  ProtectionGrid buildAutomaticProtectionGrid(
      World* world, glm::vec3 const& center, float radius, float rotation_degrees,
      MapStampShape shape, QuadraticCoefficients const& target_macro,
      MapStampProtectionSettings const& settings)
  {
    ProtectionGrid grid;
    grid.resolution = captureResolution(radius, UNITSIZE, 513);
    std::size_t const sample_count = static_cast<std::size_t>(grid.resolution) * grid.resolution;
    std::vector<float> heights(sample_count, 0.f);
    std::vector<std::uint8_t> valid(sample_count, 0);
    float const radians = glm::radians(rotation_degrees);
    float const cosine = std::cos(radians);
    float const sine = std::sin(radians);
    for (int y = 0; y < grid.resolution; ++y)
      for (int x = 0; x < grid.resolution; ++x)
      {
        float const nx = static_cast<float>(x) / (grid.resolution - 1) * 2.f - 1.f;
        float const nz = static_cast<float>(y) / (grid.resolution - 1) * 2.f - 1.f;
        if (shapeDistance(nx, nz, shape) > 1.f)
          continue;
        float const dx = cosine * nx - sine * nz;
        float const dz = sine * nx + cosine * nz;
        glm::vec3 const position{center.x + dx * radius, center.y, center.z + dz * radius};
        MapChunk* chunk = nullptr;
        world->for_maybe_chunk_at(position, [&](MapChunk* found)
        {
          chunk = found;
          return true;
        });
        std::optional<float> const height = terrainHeightAt(chunk, position);
        if (!height)
          continue;
        int const index = y * grid.resolution + x;
        heights[index] = *height;
        valid[index] = 1;
      }

    grid.values.assign(sample_count, 0.f);
    float const world_spacing = 2.f * radius / (grid.resolution - 1);
    for (int y = 0; y < grid.resolution; ++y)
      for (int x = 0; x < grid.resolution; ++x)
      {
        int const index = y * grid.resolution + x;
        if (!valid[index])
          continue;
        int const left = y * grid.resolution + std::max(0, x - 1);
        int const right = y * grid.resolution + std::min(grid.resolution - 1, x + 1);
        int const above = std::max(0, y - 1) * grid.resolution + x;
        int const below = std::min(grid.resolution - 1, y + 1) * grid.resolution + x;
        float const dx_span = x > 0 && x + 1 < grid.resolution ? 2.f : 1.f;
        float const dz_span = y > 0 && y + 1 < grid.resolution ? 2.f : 1.f;
        float const gradient_x = valid[left] && valid[right]
            ? (heights[right] - heights[left]) / (world_spacing * dx_span) : 0.f;
        float const gradient_z = valid[above] && valid[below]
            ? (heights[below] - heights[above]) / (world_spacing * dz_span) : 0.f;
        float const slope = glm::degrees(std::atan(std::hypot(gradient_x, gradient_z)));
        double const nx = static_cast<double>(x) / (grid.resolution - 1) * 2.0 - 1.0;
        double const nz = static_cast<double>(y) / (grid.resolution - 1) * 2.0 - 1.0;
        float const relief = heights[index]
            - static_cast<float>(evaluateQuadratic(target_macro, nx, nz));
        float const slope_protection = protectionRamp(
            slope, settings.slope_start_degrees, settings.slope_full_degrees);
        float const relief_protection = protectionRamp(
            relief, settings.relief_start, settings.relief_full);
        grid.values[index] = std::max(slope_protection, relief_protection);
      }

    // Keep the existing detector footprint, but turn its boundary into a two-sided transition.
    // The mountain remains immutable beyond the inner edge while the regular stamp takes over
    // beyond the outer edge. This avoids joining two substantially different height surfaces at
    // a hard contour.
    std::vector<std::uint8_t> protected_core(sample_count, 0);
    for (std::size_t index = 0; index < sample_count; ++index)
      protected_core[index] = grid.values[index] > 0.f ? 1 : 0;

    // Approximate signed distance to the detected boundary with two chamfer transforms: one
    // measures unprotected samples from the mountain and the other measures protected samples
    // from ordinary terrain.
    float constexpr diagonal = 1.41421356237f;
    float const infinity = std::numeric_limits<float>::max() / 4.f;
    auto distanceTransform = [&](bool seed_protected)
    {
      std::vector<float> distances(sample_count, infinity);
      for (std::size_t index = 0; index < sample_count; ++index)
        if (static_cast<bool>(protected_core[index]) == seed_protected)
          distances[index] = 0.f;

      auto relax = [&](int x, int y, int ox, int oy, float cost)
      {
        int const sx = x + ox;
        int const sy = y + oy;
        if (sx < 0 || sy < 0 || sx >= grid.resolution || sy >= grid.resolution)
          return;
        int const index = y * grid.resolution + x;
        distances[index] = std::min(distances[index],
            distances[sy * grid.resolution + sx] + cost);
      };
      for (int y = 0; y < grid.resolution; ++y)
        for (int x = 0; x < grid.resolution; ++x)
        {
          relax(x, y, -1, 0, 1.f);
          relax(x, y, 0, -1, 1.f);
          relax(x, y, -1, -1, diagonal);
          relax(x, y, 1, -1, diagonal);
        }
      for (int y = grid.resolution - 1; y >= 0; --y)
        for (int x = grid.resolution - 1; x >= 0; --x)
        {
          relax(x, y, 1, 0, 1.f);
          relax(x, y, 0, 1, 1.f);
          relax(x, y, 1, 1, diagonal);
          relax(x, y, -1, 1, diagonal);
        }
      return distances;
    };
    std::vector<float> const distance_to_protected = distanceTransform(true);
    std::vector<float> const distance_to_unprotected = distanceTransform(false);

    float const blend_world = std::clamp(radius * .15f, 6.f * UNITSIZE, 16.f * UNITSIZE);
    float const half_blend_samples = blend_world / world_spacing * .5f;
    std::vector<float> blended_protection(sample_count, 0.f);
    for (std::size_t index = 0; index < sample_count; ++index)
    {
      // Offset by half a sample so the zero-distance boundary lies between the last protected
      // sample and the first unprotected sample rather than through either sample center.
      float const signed_distance = protected_core[index]
          ? -(distance_to_unprotected[index] - .5f)
          : distance_to_protected[index] - .5f;
      float const t = std::clamp(
          (signed_distance + half_blend_samples) / (2.f * half_blend_samples), 0.f, 1.f);
      float const stamp_weight = t * t * (3.f - 2.f * t);
      blended_protection[index] = 1.f - stamp_weight;
    }
    grid.values = std::move(blended_protection);
    return grid;
  }
}

bool MapStampAsset::capture(World* world, glm::vec3 const& center, float radius,
                            MapStampShape shape, QString* error)
{
  auto fail = [error](QString const& message)
  {
    if (error)
      *error = message;
    return false;
  };

  if (!world || !std::isfinite(radius) || radius <= 0.f
      || shape == MapStampShape::Painted)
    return fail("The capture radius is invalid.");

  MapChunk* center_chunk = nullptr;
  world->for_maybe_chunk_at(center, [&](MapChunk* chunk)
  {
    center_chunk = chunk;
    return true;
  });
  if (!center_chunk)
    return fail("The terrain beneath the cursor is not loaded.");

  std::optional<float> const center_height = terrainHeightAt(center_chunk, center);
  if (!center_height)
    return fail("Unable to sample the terrain beneath the cursor.");
  float const sample_radius = radius * capture_extent;
  int const height_resolution = captureResolution(sample_radius, UNITSIZE * .5f,
                                                   maximum_height_resolution);
  int const texture_resolution = captureResolution(sample_radius, TEXDETAILSIZE,
                                                    maximum_texture_resolution);
  std::vector<float> heights(static_cast<std::size_t>(height_resolution) * height_resolution, 0.f);
  std::vector<MapStampTexture> textures;
  std::unordered_map<std::string, std::size_t> texture_lookup;

  for (int y = 0; y < height_resolution; ++y)
    for (int x = 0; x < height_resolution; ++x)
    {
      int const sample_index = y * height_resolution + x;
      float const nx = (static_cast<float>(x) / (height_resolution - 1) * 2.f - 1.f)
          * capture_extent;
      float const nz = (static_cast<float>(y) / (height_resolution - 1) * 2.f - 1.f)
          * capture_extent;
      if (shapeDistance(nx, nz, shape) > capture_extent)
        continue;
      glm::vec3 const sample_pos{center.x + nx * radius, center.y, center.z + nz * radius};

      MapChunk* chunk = nullptr;
      world->for_maybe_chunk_at(sample_pos, [&](MapChunk* found)
      {
        chunk = found;
        return true;
      });
      if (!chunk)
        return fail("The capture crosses terrain that is not loaded. Load the surrounding ADTs and try again.");

      std::optional<float> const sample_height = terrainHeightAt(chunk, sample_pos);
      if (!sample_height)
        return fail("The sampled region contains a terrain hole or an unreadable height.");
      heights[sample_index] = *sample_height - *center_height;
    }

  for (int y = 0; y < texture_resolution; ++y)
    for (int x = 0; x < texture_resolution; ++x)
    {
      int const sample_index = y * texture_resolution + x;
      float const nx = (static_cast<float>(x) / (texture_resolution - 1) * 2.f - 1.f)
          * capture_extent;
      float const nz = (static_cast<float>(y) / (texture_resolution - 1) * 2.f - 1.f)
          * capture_extent;
      if (shapeDistance(nx, nz, shape) > capture_extent)
        continue;
      glm::vec3 const sample_pos{center.x + nx * radius, center.y, center.z + nz * radius};

      MapChunk* chunk = nullptr;
      world->for_maybe_chunk_at(sample_pos, [&](MapChunk* found)
      {
        chunk = found;
        return true;
      });
      if (!chunk)
        return fail("The capture crosses terrain that is not loaded. Load the surrounding ADTs and try again.");

      TextureSet* texture_set = chunk->getTextureSet();
      int const alpha_x = std::clamp(static_cast<int>(
          (sample_pos.x - chunk->xbase) / CHUNKSIZE * 64.f), 0, 63);
      int const alpha_z = std::clamp(static_cast<int>(
          (sample_pos.z - chunk->zbase) / CHUNKSIZE * 64.f), 0, 63);
      std::array<float, 4> const weights = chunkWeightsAt(texture_set, alpha_z * 64 + alpha_x);

      for (std::size_t layer = 0; layer < std::min<std::size_t>(4, texture_set->num()); ++layer)
      {
        std::string const& filename = texture_set->filename(layer);
        auto found = texture_lookup.find(filename);
        if (found == texture_lookup.end())
        {
          if (textures.size() >= maximum_textures)
            return fail("The sampled region uses too many distinct textures.");
          MapStampTexture texture;
          texture.filename = filename;
          texture.layer = texture_set->getMCLYEntries()[layer];
          texture.weights.resize(static_cast<std::size_t>(texture_resolution)
                                 * texture_resolution, 0);
          found = texture_lookup.emplace(filename, textures.size()).first;
          textures.emplace_back(std::move(texture));
        }
        auto& stored_weight = textures[found->second].weights[sample_index];
        stored_weight = static_cast<std::uint8_t>(std::min(255,
            static_cast<int>(stored_weight) + static_cast<int>(std::lround(weights[layer]))));
      }
    }

  _source_radius = radius;
  _height_resolution = height_resolution;
  _texture_resolution = texture_resolution;
  _shape = shape;
  _sample_extent = capture_extent;
  // Raw relative heights are the authoritative capture. Keep a derived relief copy for the
  // optional conforming mode, but never discard the exact feature in order to create it.
  _relative_heights = std::move(heights);
  _relief_heights = _relative_heights;
  detrendHeightRelief(_relief_heights, height_resolution, shape, _sample_extent);
  _height_is_relief = false;
  _painted_footprint_mask.clear();
  _painted_outside_distances.clear();
  _painted_nearest_indices.clear();
  rebuildExactFeatureMask();
  _textures = std::move(textures);
  return true;
}

bool MapStampAsset::capture(World* world, MapStampPaintedSelection const& selection,
                            QString* error)
{
  auto fail = [error](QString const& message)
  {
    if (error)
      *error = message;
    return false;
  };
  if (!world || !std::isfinite(selection.grid_step) || selection.grid_step <= 0.f
      || selection.cells.empty())
    return fail("Paint a source footprint before capturing the stamp.");

  int minimum_x = std::numeric_limits<int>::max();
  int maximum_x = std::numeric_limits<int>::lowest();
  int minimum_z = std::numeric_limits<int>::max();
  int maximum_z = std::numeric_limits<int>::lowest();
  std::size_t selected_count = 0;
  for (auto const& [cell, strength] : selection.cells)
  {
    if (strength < .25f)
      continue;
    minimum_z = std::min(minimum_z, cell.first);
    maximum_z = std::max(maximum_z, cell.first);
    minimum_x = std::min(minimum_x, cell.second);
    maximum_x = std::max(maximum_x, cell.second);
    ++selected_count;
  }
  if (!selected_count)
    return fail("The painted source footprint is empty.");

  float const minimum_world_x = minimum_x * selection.grid_step;
  float const maximum_world_x = (maximum_x + 1) * selection.grid_step;
  float const minimum_world_z = minimum_z * selection.grid_step;
  float const maximum_world_z = (maximum_z + 1) * selection.grid_step;
  glm::vec3 const center{
      (minimum_world_x + maximum_world_x) * .5f, 0.f,
      (minimum_world_z + maximum_world_z) * .5f};
  float const core_radius = std::max(
      maximum_world_x - minimum_world_x, maximum_world_z - minimum_world_z) * .5f;
  if (!std::isfinite(core_radius) || core_radius <= 0.f)
    return fail("The painted source footprint has invalid dimensions.");

  // Capture a square reference collar, then reinterpret its coordinates around the painted
  // core. The collar is evidence for grounding and detrending; the persistent mask below is
  // authoritative for what may actually be placed.
  if (!capture(world, center, core_radius * painted_capture_extent,
               MapStampShape::Square, error))
    return false;

  _source_radius = core_radius;
  _sample_extent = painted_capture_extent;
  _shape = MapStampShape::Painted;
  std::size_t const height_samples = static_cast<std::size_t>(_height_resolution)
      * _height_resolution;
  _painted_footprint_mask.assign(height_samples, 0);
  auto selectedAt = [&](float world_x, float world_z)
  {
    int const grid_x = static_cast<int>(std::floor(world_x / selection.grid_step));
    int const grid_z = static_cast<int>(std::floor(world_z / selection.grid_step));
    auto const found = selection.cells.find({grid_z, grid_x});
    return found != selection.cells.end() && found->second >= .25f;
  };
  std::size_t stored_selected = 0;
  for (int y = 0; y < _height_resolution; ++y)
    for (int x = 0; x < _height_resolution; ++x)
    {
      float const nx = (static_cast<float>(x) / (_height_resolution - 1) * 2.f - 1.f)
          * _sample_extent;
      float const nz = (static_cast<float>(y) / (_height_resolution - 1) * 2.f - 1.f)
          * _sample_extent;
      std::size_t const index = static_cast<std::size_t>(y) * _height_resolution + x;
      if (selectedAt(center.x + nx * core_radius, center.z + nz * core_radius))
      {
        _painted_footprint_mask[index] = 255;
        ++stored_selected;
      }
    }
  if (!stored_selected)
    return fail("The painted footprint is too small for the terrain sampling grid.");

  // A texture identity found only in the automatic collar is environmental context, not part
  // of the selected feature. Keep weights for identities that occur somewhere in the core;
  // placement clamps its skirt back to the nearest selected edge.
  _textures.erase(std::remove_if(_textures.begin(), _textures.end(),
      [&](MapStampTexture const& texture)
      {
        for (int y = 0; y < _texture_resolution; ++y)
          for (int x = 0; x < _texture_resolution; ++x)
          {
            float const nx = (static_cast<float>(x) / (_texture_resolution - 1) * 2.f - 1.f)
                * _sample_extent;
            float const nz = (static_cast<float>(y) / (_texture_resolution - 1) * 2.f - 1.f)
                * _sample_extent;
            if (selectedAt(center.x + nx * core_radius, center.z + nz * core_radius)
                && texture.weights[static_cast<std::size_t>(y) * _texture_resolution + x])
              return false;
          }
        return true;
      }), _textures.end());

  rebuildPaintedFootprintData();
  _relief_heights = _relative_heights;
  detrendPaintedHeightRelief(_relief_heights, _height_resolution,
                             _painted_footprint_mask, _painted_outside_distances,
                             _sample_extent);
  rebuildExactFeatureMask();
  return valid();
}

float MapStampAsset::sampleHeight(float u, float v, MapStampHeightMode height_mode) const
{
  std::vector<float> const& heights = height_mode == MapStampHeightMode::ExactFeature
      && !_height_is_relief ? _relative_heights : _relief_heights;
  if (heights.empty() || _height_resolution < 2)
    return 0.f;
  u = std::clamp(((u * 2.f - 1.f) / _sample_extent + 1.f) * .5f, 0.f, 1.f)
      * (_height_resolution - 1);
  v = std::clamp(((v * 2.f - 1.f) / _sample_extent + 1.f) * .5f, 0.f, 1.f)
      * (_height_resolution - 1);
  int const x0 = static_cast<int>(std::floor(u));
  int const y0 = static_cast<int>(std::floor(v));
  int const x1 = std::min(x0 + 1, _height_resolution - 1);
  int const y1 = std::min(y0 + 1, _height_resolution - 1);
  float const tx = u - x0;
  float const ty = v - y0;
  float const top = std::lerp(heights[y0 * _height_resolution + x0],
                              heights[y0 * _height_resolution + x1], tx);
  float const bottom = std::lerp(heights[y1 * _height_resolution + x0],
                                 heights[y1 * _height_resolution + x1], tx);
  return std::lerp(top, bottom, ty);
}

float MapStampAsset::sampleTexture(std::size_t layer, float u, float v) const
{
  if (layer >= _textures.size() || _textures[layer].weights.empty()
      || _texture_resolution < 2)
    return 0.f;
  u = std::clamp(((u * 2.f - 1.f) / _sample_extent + 1.f) * .5f, 0.f, 1.f);
  v = std::clamp(((v * 2.f - 1.f) / _sample_extent + 1.f) * .5f, 0.f, 1.f);
  float const fx = u * (_texture_resolution - 1);
  float const fy = v * (_texture_resolution - 1);
  int const x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, _texture_resolution - 1);
  int const y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, _texture_resolution - 1);
  int const x1 = std::min(x0 + 1, _texture_resolution - 1);
  int const y1 = std::min(y0 + 1, _texture_resolution - 1);
  float const tx = fx - x0;
  float const ty = fy - y0;
  auto const& weights = _textures[layer].weights;
  float const top = std::lerp(static_cast<float>(weights[y0 * _texture_resolution + x0]),
                              static_cast<float>(weights[y0 * _texture_resolution + x1]), tx);
  float const bottom = std::lerp(static_cast<float>(weights[y1 * _texture_resolution + x0]),
                                 static_cast<float>(weights[y1 * _texture_resolution + x1]), tx);
  // The renderer filters the completed destination alphamap, but it cannot undo
  // nearest-neighbour blocks baked while a source grid is rotated or rescaled.
  return std::lerp(top, bottom, ty);
}

float MapStampAsset::paintedFootprintWeight(float u, float v) const
{
  if (_shape != MapStampShape::Painted || _painted_footprint_mask.empty()
      || _height_resolution < 2)
    return 0.f;
  float const nx = u * 2.f - 1.f;
  float const nz = v * 2.f - 1.f;
  if (std::abs(nx) > _sample_extent || std::abs(nz) > _sample_extent)
    return 0.f;
  float const fx = ((nx / _sample_extent + 1.f) * .5f) * (_height_resolution - 1);
  float const fy = ((nz / _sample_extent + 1.f) * .5f) * (_height_resolution - 1);
  int const x0 = static_cast<int>(std::floor(fx));
  int const y0 = static_cast<int>(std::floor(fy));
  int const x1 = std::min(x0 + 1, _height_resolution - 1);
  int const y1 = std::min(y0 + 1, _height_resolution - 1);
  float const tx = fx - x0;
  float const ty = fy - y0;
  auto value = [&](int x, int y)
  {
    return _painted_footprint_mask[static_cast<std::size_t>(y) * _height_resolution + x]
        / 255.f;
  };
  return std::lerp(std::lerp(value(x0, y0), value(x1, y0), tx),
                   std::lerp(value(x0, y1), value(x1, y1), tx), ty);
}

float MapStampAsset::paintedOutsideDistance(float u, float v) const
{
  if (_shape != MapStampShape::Painted || _painted_outside_distances.empty()
      || _height_resolution < 2)
    return std::numeric_limits<float>::max();
  float const nx = u * 2.f - 1.f;
  float const nz = v * 2.f - 1.f;
  float const clamped_nx = std::clamp(nx, -_sample_extent, _sample_extent);
  float const clamped_nz = std::clamp(nz, -_sample_extent, _sample_extent);
  float const fx = ((clamped_nx / _sample_extent + 1.f) * .5f)
      * (_height_resolution - 1);
  float const fy = ((clamped_nz / _sample_extent + 1.f) * .5f)
      * (_height_resolution - 1);
  int const x0 = static_cast<int>(std::floor(fx));
  int const y0 = static_cast<int>(std::floor(fy));
  int const x1 = std::min(x0 + 1, _height_resolution - 1);
  int const y1 = std::min(y0 + 1, _height_resolution - 1);
  float const tx = fx - x0;
  float const ty = fy - y0;
  auto value = [&](int x, int y)
  {
    return _painted_outside_distances[static_cast<std::size_t>(y) * _height_resolution + x];
  };
  float const sampled = std::lerp(std::lerp(value(x0, y0), value(x1, y0), tx),
                                  std::lerp(value(x0, y1), value(x1, y1), tx), ty);
  return sampled + std::hypot(nx - clamped_nx, nz - clamped_nz);
}

glm::vec2 MapStampAsset::clampToPaintedBoundary(glm::vec2 const& uv) const
{
  if (_shape != MapStampShape::Painted || _painted_nearest_indices.empty()
      || _height_resolution < 2 || paintedFootprintWeight(uv.x, uv.y) >= .5f)
    return uv;
  float const nx = std::clamp(uv.x * 2.f - 1.f, -_sample_extent, _sample_extent);
  float const nz = std::clamp(uv.y * 2.f - 1.f, -_sample_extent, _sample_extent);
  float const fx = (nx / _sample_extent + 1.f) * .5f * (_height_resolution - 1);
  float const fy = (nz / _sample_extent + 1.f) * .5f * (_height_resolution - 1);
  int const x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, _height_resolution - 1);
  int const y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, _height_resolution - 1);
  int const x1 = std::min(x0 + 1, _height_resolution - 1);
  int const y1 = std::min(y0 + 1, _height_resolution - 1);
  float const tx = fx - x0;
  float const ty = fy - y0;
  auto nearestPosition = [&](int x, int y) -> std::optional<glm::vec2>
  {
    std::int32_t const nearest = _painted_nearest_indices[
        static_cast<std::size_t>(y) * _height_resolution + x];
    if (nearest < 0)
      return std::nullopt;
    int const nearest_x = nearest % _height_resolution;
    int const nearest_y = nearest / _height_resolution;
    float const nearest_nx = (static_cast<float>(nearest_x) / (_height_resolution - 1)
        * 2.f - 1.f) * _sample_extent;
    float const nearest_nz = (static_cast<float>(nearest_y) / (_height_resolution - 1)
        * 2.f - 1.f) * _sample_extent;
    return glm::vec2{(nearest_nx + 1.f) * .5f, (nearest_nz + 1.f) * .5f};
  };
  auto const top_left = nearestPosition(x0, y0);
  auto const top_right = nearestPosition(x1, y0);
  auto const bottom_left = nearestPosition(x0, y1);
  auto const bottom_right = nearestPosition(x1, y1);
  if (!top_left || !top_right || !bottom_left || !bottom_right)
    return clampToShapeBoundary(uv, MapStampShape::Square);

  // Interpolate the nearest-boundary coordinate field rather than snapping
  // every skirt sample to one mask cell. This removes the visible Voronoi fans
  // from both height and texture sampling around an irregular outline.
  glm::vec2 const top = *top_left * (1.f - tx) + *top_right * tx;
  glm::vec2 const bottom = *bottom_left * (1.f - tx) + *bottom_right * tx;
  return top * (1.f - ty) + bottom * ty;
}

float MapStampAsset::placementCoverage(float u, float v, float hardness, float opacity,
                                       MapStampHeightMode height_mode) const
{
  if (_shape != MapStampShape::Painted)
    return stampCoverageAt(u * 2.f - 1.f, v * 2.f - 1.f, hardness, opacity,
                           _shape, height_mode);
  if (paintedFootprintWeight(u, v) >= .5f)
    return std::clamp(opacity, 0.f, 1.f);
  float const blend = std::clamp(1.f - hardness, .05f, .5f);
  float const distance = paintedOutsideDistance(u, v);
  if (!std::isfinite(distance) || distance >= blend)
    return 0.f;
  float const t = std::clamp(1.f - distance / blend, 0.f, 1.f);
  return t * t * (3.f - 2.f * t) * std::clamp(opacity, 0.f, 1.f);
}

void MapStampAsset::rebuildPaintedFootprintData()
{
  _painted_outside_distances.clear();
  _painted_nearest_indices.clear();
  if (_shape != MapStampShape::Painted || _height_resolution < 2
      || _painted_footprint_mask.size()
         != static_cast<std::size_t>(_height_resolution) * _height_resolution)
    return;

  std::size_t const count = _painted_footprint_mask.size();
  float const infinity = std::numeric_limits<float>::max() / 4.f;
  std::vector<float> grid_distances(count, infinity);
  _painted_nearest_indices.assign(count, -1);
  using QueueEntry = std::pair<float, std::int32_t>;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> pending;
  for (std::int32_t index = 0; index < static_cast<std::int32_t>(count); ++index)
    if (_painted_footprint_mask[index] >= 128)
    {
      grid_distances[index] = 0.f;
      _painted_nearest_indices[index] = index;
      pending.emplace(0.f, index);
    }

  while (!pending.empty())
  {
    auto const [distance, index] = pending.top();
    pending.pop();
    if (distance != grid_distances[index])
      continue;
    int const x = index % _height_resolution;
    int const y = index / _height_resolution;
    for (int oy = -1; oy <= 1; ++oy)
      for (int ox = -1; ox <= 1; ++ox)
      {
        if ((!ox && !oy) || x + ox < 0 || y + oy < 0
            || x + ox >= _height_resolution || y + oy >= _height_resolution)
          continue;
        std::int32_t const adjacent = (y + oy) * _height_resolution + x + ox;
        float const next = distance + (ox && oy ? 1.41421356237f : 1.f);
        if (next >= grid_distances[adjacent])
          continue;
        grid_distances[adjacent] = next;
        _painted_nearest_indices[adjacent] = _painted_nearest_indices[index];
        pending.emplace(next, adjacent);
      }
  }

  float const normalized_step = 2.f * _sample_extent / (_height_resolution - 1);
  _painted_outside_distances.resize(count, infinity);
  for (std::size_t index = 0; index < count; ++index)
    _painted_outside_distances[index] = _painted_footprint_mask[index] >= 128
        ? 0.f : std::max(0.f, grid_distances[index] - .5f) * normalized_step;
}

float MapStampAsset::exactFeatureWeight(float u, float v) const
{
  if (_shape == MapStampShape::Painted)
    return 1.f;
  if (_exact_feature_mask.empty() || _height_resolution < 2)
    return 1.f;
  glm::vec2 const core_uv = clampToShapeBoundary({u, v}, _shape);
  float x = std::clamp(((core_uv.x * 2.f - 1.f) / _sample_extent + 1.f) * .5f,
                       0.f, 1.f) * (_height_resolution - 1);
  float y = std::clamp(((core_uv.y * 2.f - 1.f) / _sample_extent + 1.f) * .5f,
                       0.f, 1.f) * (_height_resolution - 1);
  int const x0 = static_cast<int>(std::floor(x));
  int const y0 = static_cast<int>(std::floor(y));
  int const x1 = std::min(x0 + 1, _height_resolution - 1);
  int const y1 = std::min(y0 + 1, _height_resolution - 1);
  float const tx = x - x0;
  float const ty = y - y0;
  float const top = std::lerp(_exact_feature_mask[y0 * _height_resolution + x0],
                              _exact_feature_mask[y0 * _height_resolution + x1], tx);
  float const bottom = std::lerp(_exact_feature_mask[y1 * _height_resolution + x0],
                                 _exact_feature_mask[y1 * _height_resolution + x1], tx);
  return std::lerp(top, bottom, ty);
}

void MapStampAsset::rebuildExactFeatureMask()
{
  _exact_source_base_height_cached = false;
  _exact_source_base_height.reset();
  _exact_feature_mask.clear();
  if (_height_is_relief || _height_resolution < 3
      || _relative_heights.size()
         != static_cast<std::size_t>(_height_resolution) * _height_resolution)
    return;

  if (_shape == MapStampShape::Painted)
  {
    _exact_feature_mask.resize(_painted_footprint_mask.size(), 0.f);
    for (std::size_t index = 0; index < _painted_footprint_mask.size(); ++index)
      _exact_feature_mask[index] = _painted_footprint_mask[index] / 255.f;
    return;
  }

  std::size_t const sample_count = _relative_heights.size();
  std::vector<std::uint8_t> core(sample_count, 0);
  float minimum = std::numeric_limits<float>::max();
  float maximum = std::numeric_limits<float>::lowest();
  std::size_t core_count = 0;
  for (int y = 0; y < _height_resolution; ++y)
    for (int x = 0; x < _height_resolution; ++x)
    {
      float const nx = (static_cast<float>(x) / (_height_resolution - 1) * 2.f - 1.f)
          * _sample_extent;
      float const nz = (static_cast<float>(y) / (_height_resolution - 1) * 2.f - 1.f)
          * _sample_extent;
      std::size_t const index = static_cast<std::size_t>(y) * _height_resolution + x;
      if (shapeDistance(nx, nz, _shape) > 1.f)
        continue;
      core[index] = 1;
      minimum = std::min(minimum, _relative_heights[index]);
      maximum = std::max(maximum, _relative_heights[index]);
      ++core_count;
    }

  _exact_feature_mask.assign(sample_count, 0.f);
  if (!core_count)
    return;
  float const height_range = maximum - minimum;
  if (height_range < 8.f * UNITSIZE)
  {
    for (std::size_t index = 0; index < sample_count; ++index)
      _exact_feature_mask[index] = core[index] ? 1.f : 0.f;
    return;
  }

  constexpr int histogram_size = 256;
  std::array<std::uint64_t, histogram_size> histogram{};
  auto histogramBin = [&](float height)
  {
    return std::clamp(static_cast<int>((height - minimum) / height_range
                                      * (histogram_size - 1)), 0, histogram_size - 1);
  };
  for (std::size_t index = 0; index < sample_count; ++index)
    if (core[index])
      ++histogram[histogramBin(_relative_heights[index])];

  double total_sum = 0.0;
  for (int bin = 0; bin < histogram_size; ++bin)
    total_sum += static_cast<double>(bin) * histogram[bin];
  std::uint64_t background_count = 0;
  double background_sum = 0.0;
  double best_score = -1.0;
  int best_bin = 0;
  for (int bin = 0; bin + 1 < histogram_size; ++bin)
  {
    background_count += histogram[bin];
    background_sum += static_cast<double>(bin) * histogram[bin];
    std::uint64_t const foreground_count = core_count - background_count;
    if (!background_count || !foreground_count)
      continue;
    double const background_mean = background_sum / background_count;
    double const foreground_mean = (total_sum - background_sum) / foreground_count;
    double const difference = background_mean - foreground_mean;
    double const score = static_cast<double>(background_count) * foreground_count
        * difference * difference;
    if (score > best_score)
    {
      best_score = score;
      best_bin = bin;
    }
  }
  float const threshold = minimum + height_range
      * (static_cast<float>(best_bin) + .5f) / histogram_size;

  std::vector<std::uint8_t> candidate(sample_count, 0);
  for (std::size_t index = 0; index < sample_count; ++index)
    candidate[index] = core[index] && _relative_heights[index] >= threshold ? 1 : 0;
  std::size_t seed = static_cast<std::size_t>(_height_resolution / 2) * _height_resolution
      + _height_resolution / 2;
  if (!candidate[seed])
  {
    seed = sample_count;
    float highest = std::numeric_limits<float>::lowest();
    for (std::size_t index = 0; index < sample_count; ++index)
      if (candidate[index] && _relative_heights[index] > highest)
      {
        highest = _relative_heights[index];
        seed = index;
      }
  }
  if (seed >= sample_count)
  {
    for (std::size_t index = 0; index < sample_count; ++index)
      _exact_feature_mask[index] = core[index] ? 1.f : 0.f;
    return;
  }

  auto visitConnected = [&](std::vector<std::uint8_t> const& allowed,
                            std::vector<std::uint8_t>& visited,
                            std::vector<std::size_t> seeds)
  {
    std::size_t cursor = 0;
    for (std::size_t const index : seeds)
      visited[index] = 1;
    while (cursor < seeds.size())
    {
      std::size_t const index = seeds[cursor++];
      int const x = static_cast<int>(index % _height_resolution);
      int const y = static_cast<int>(index / _height_resolution);
      for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox)
        {
          if ((!ox && !oy) || x + ox < 0 || y + oy < 0
              || x + ox >= _height_resolution || y + oy >= _height_resolution)
            continue;
          std::size_t const adjacent = static_cast<std::size_t>(y + oy) * _height_resolution
              + static_cast<std::size_t>(x + ox);
          if (!allowed[adjacent] || visited[adjacent])
            continue;
          visited[adjacent] = 1;
          seeds.push_back(adjacent);
        }
    }
  };

  std::vector<std::uint8_t> feature(sample_count, 0);
  visitConnected(candidate, feature, {seed});
  float const base_margin = std::clamp(height_range * .08f, 4.f * UNITSIZE,
                                       12.f * UNITSIZE);
  std::vector<std::uint8_t> base_allowed(sample_count, 0);
  std::vector<std::size_t> feature_seeds;
  for (std::size_t index = 0; index < sample_count; ++index)
  {
    base_allowed[index] = core[index]
        && _relative_heights[index] >= threshold - base_margin ? 1 : 0;
    if (feature[index])
      feature_seeds.push_back(index);
  }
  std::vector<std::uint8_t> grown(sample_count, 0);
  visitConnected(base_allowed, grown, std::move(feature_seeds));

  // Preserve enclosed valleys and saddles. Only low terrain connected to the capture boundary is
  // surrounding ground; a low pocket completely enclosed by the mountain remains part of it.
  std::vector<std::uint8_t> external_allowed(sample_count, 0);
  std::vector<std::size_t> external_seeds;
  for (int y = 0; y < _height_resolution; ++y)
    for (int x = 0; x < _height_resolution; ++x)
    {
      std::size_t const index = static_cast<std::size_t>(y) * _height_resolution + x;
      if (!core[index] || grown[index])
        continue;
      external_allowed[index] = 1;
      bool boundary = x == 0 || y == 0 || x + 1 == _height_resolution
          || y + 1 == _height_resolution;
      for (int oy = -1; !boundary && oy <= 1; ++oy)
        for (int ox = -1; !boundary && ox <= 1; ++ox)
        {
          if (!ox && !oy)
            continue;
          int const ax = x + ox;
          int const ay = y + oy;
          boundary = ax < 0 || ay < 0 || ax >= _height_resolution
              || ay >= _height_resolution
              || !core[static_cast<std::size_t>(ay) * _height_resolution + ax];
        }
      if (boundary)
        external_seeds.push_back(index);
    }
  std::vector<std::uint8_t> external(sample_count, 0);
  visitConnected(external_allowed, external, std::move(external_seeds));

  for (std::size_t index = 0; index < sample_count; ++index)
  {
    if (!core[index])
      continue;
    if (!grown[index] && !external[index])
    {
      _exact_feature_mask[index] = 1.f;
      continue;
    }
    if (!grown[index])
      continue;
    float const t = std::clamp(
        (_relative_heights[index] - (threshold - base_margin)) / base_margin, 0.f, 1.f);
    _exact_feature_mask[index] = t * t * (3.f - 2.f * t);
  }
}

std::optional<float> MapStampAsset::exactSourceBaseHeight() const
{
  if (_exact_source_base_height_cached)
    return _exact_source_base_height;
  if (_height_is_relief || _height_resolution < 3
      || _relative_heights.size()
         != static_cast<std::size_t>(_height_resolution) * _height_resolution)
  {
    _exact_source_base_height_cached = true;
    _exact_source_base_height.reset();
    return std::nullopt;
  }

  std::size_t const sample_count = _relative_heights.size();
  std::vector<std::uint8_t> feature(sample_count, 0);
  for (std::size_t index = 0; index < sample_count; ++index)
    feature[index] = _shape == MapStampShape::Painted
        ? (index < _painted_footprint_mask.size() && _painted_footprint_mask[index] >= 128)
        : (index < _exact_feature_mask.size() && _exact_feature_mask[index] >= .5f);

  // Mark only unselected terrain connected to the capture exterior. Enclosed
  // valleys and holes are part of the mountain and must not define its base.
  std::vector<std::uint8_t> external(sample_count, 0);
  std::queue<std::size_t> pending;
  for (int y = 0; y < _height_resolution; ++y)
    for (int x = 0; x < _height_resolution; ++x)
    {
      bool const edge = x == 0 || y == 0 || x + 1 == _height_resolution
          || y + 1 == _height_resolution;
      std::size_t const index = static_cast<std::size_t>(y) * _height_resolution + x;
      if (edge && !feature[index] && !external[index])
      {
        external[index] = 1;
        pending.push(index);
      }
    }
  while (!pending.empty())
  {
    std::size_t const index = pending.front();
    pending.pop();
    int const x = static_cast<int>(index % _height_resolution);
    int const y = static_cast<int>(index / _height_resolution);
    for (int oy = -1; oy <= 1; ++oy)
      for (int ox = -1; ox <= 1; ++ox)
      {
        int const adjacent_x = x + ox;
        int const adjacent_y = y + oy;
        if ((!ox && !oy) || adjacent_x < 0 || adjacent_y < 0
            || adjacent_x >= _height_resolution || adjacent_y >= _height_resolution)
          continue;
        std::size_t const adjacent = static_cast<std::size_t>(adjacent_y)
            * _height_resolution + adjacent_x;
        if (feature[adjacent] || external[adjacent])
          continue;
        external[adjacent] = 1;
        pending.push(adjacent);
      }
  }

  std::vector<float> boundary_heights;
  boundary_heights.reserve(static_cast<std::size_t>(_height_resolution) * 4);
  for (int y = 0; y < _height_resolution; ++y)
    for (int x = 0; x < _height_resolution; ++x)
    {
      std::size_t const index = static_cast<std::size_t>(y) * _height_resolution + x;
      if (!feature[index])
        continue;
      bool boundary = x == 0 || y == 0 || x + 1 == _height_resolution
          || y + 1 == _height_resolution;
      for (int oy = -1; !boundary && oy <= 1; ++oy)
        for (int ox = -1; !boundary && ox <= 1; ++ox)
          if (ox || oy)
          {
            std::size_t const adjacent = static_cast<std::size_t>(y + oy)
                * _height_resolution + x + ox;
            boundary = external[adjacent] != 0;
          }
      if (boundary)
        boundary_heights.push_back(_relative_heights[index]);
    }

  if (boundary_heights.empty())
  {
    for (int y = 0; y < _height_resolution; ++y)
      for (int x = 0; x < _height_resolution; ++x)
      {
        float const nx = (static_cast<float>(x) / (_height_resolution - 1) * 2.f - 1.f)
            * _sample_extent;
        float const nz = (static_cast<float>(y) / (_height_resolution - 1) * 2.f - 1.f)
            * _sample_extent;
        float const distance = shapeDistance(nx, nz, _shape);
        if (distance >= .8f && distance <= 1.f)
          boundary_heights.push_back(
              _relative_heights[static_cast<std::size_t>(y) * _height_resolution + x]);
      }
  }
  if (boundary_heights.empty())
  {
    _exact_source_base_height_cached = true;
    _exact_source_base_height.reset();
    return std::nullopt;
  }

  // The selected outline may cross a few elevated foothills. Use the middle of
  // its lowest quartile as a robust contact/base datum: unlike the previous 90th
  // percentile destination fit, one high support point cannot lift the mountain.
  std::sort(boundary_heights.begin(), boundary_heights.end());
  std::size_t const contact_count = std::max<std::size_t>(1, boundary_heights.size() / 4);
  boundary_heights.resize(contact_count);
  _exact_source_base_height = medianValue(std::move(boundary_heights));
  _exact_source_base_height_cached = true;
  return _exact_source_base_height;
}

bool MapStampAsset::apply(World* world, glm::vec3 const& center, float radius,
                          float rotation_degrees, float hardness, float height_scale,
                          float height_offset, float opacity, MapStampHeightMode height_mode)
{
  return apply(world, center, radius, rotation_degrees, hardness, height_scale,
               height_offset, opacity, {}, height_mode);
}

bool MapStampAsset::visitTerrainPlacement(
    World* world, glm::vec3 const& center, float radius, float rotation_degrees,
    float hardness, float height_scale, float height_offset, float opacity,
    MapStampProtectionSettings const& protection, MapStampHeightMode height_mode,
    bool mark_tiles_changed,
    std::function<void(MapChunk*, std::size_t, float)> const& visitor) const
{
  if (!world || !valid() || radius <= 0.f || !visitor)
    return false;

  MapChunk* anchor_chunk = nullptr;
  world->for_maybe_chunk_at(center, [&](MapChunk* chunk)
  {
    anchor_chunk = chunk;
    return true;
  });
  std::optional<float> const sampled_anchor = terrainHeightAt(anchor_chunk, center);
  if (!sampled_anchor)
    return false;
  float const anchor_height = *sampled_anchor;
  bool const mountain_blend = height_mode == MapStampHeightMode::MountainBlend;
  if ((height_mode == MapStampHeightMode::ExactFeature || mountain_blend)
      && !supportsExactHeight())
    return false;
  float exact_anchor_height = anchor_height;
  float exact_source_base_height = 0.f;
  if (height_mode == MapStampHeightMode::ExactFeature || mountain_blend)
  {
    if (std::optional<float> const source_base = exactSourceBaseHeight())
      exact_source_base_height = *source_base;
    // Snap the captured contact/base datum to the terrain directly beneath the
    // placement cursor. The previous perimeter upper-envelope fit could raise
    // the entire feature to satisfy one high destination point.
    if (height_mode == MapStampHeightMode::ExactFeature)
      exact_anchor_height = anchor_height - exact_source_base_height * height_scale;
  }
  std::function<bool(float, float)> painted_target_band;
  if (_shape == MapStampShape::Painted)
    painted_target_band = [this](float nx, float nz)
    {
      float const u = (nx + 1.f) * .5f;
      float const v = (nz + 1.f) * .5f;
      return paintedFootprintWeight(u, v) < .5f
          && paintedOutsideDistance(u, v) <= painted_capture_extent - 1.f;
    };
  // Exact painted features still use one vertical anchor for their untouched core. Their
  // generated outer support needs the destination macro surface to measure how far each
  // captured boundary point stands above the terrain it must meet.
  std::optional<QuadraticCoefficients> const target_macro =
      (height_mode == MapStampHeightMode::ConformToTerrain || mountain_blend
       || protection.automatic
       || (height_mode == MapStampHeightMode::ExactFeature
           && _shape == MapStampShape::Painted))
      ? fitTargetMacroSurface(world, center, radius, rotation_degrees, _shape,
                              painted_target_band, mountain_blend)
      : std::optional<QuadraticCoefficients>{};
  if (height_mode == MapStampHeightMode::ConformToTerrain && !target_macro)
    return false;
  float const traversal_radius = rotatedFootprintBoundingRadius(
      radius * (_shape == MapStampShape::Painted
          ? (height_mode == MapStampHeightMode::ExactFeature
              ? 1.5f : 1.f + std::clamp(1.f - hardness, .05f, .5f))
          : placementExtent(hardness, height_mode)), rotation_degrees, _shape);
  ProtectionGrid const automatic_protection = protection.automatic && target_macro
      ? buildAutomaticProtectionGrid(world, center, radius, rotation_degrees, _shape,
                                     *target_macro, protection)
      : ProtectionGrid{};
  auto protectionAt = [&](float world_x, float world_z, glm::vec2 const& uv)
  {
    float value = protection.automatic ? automatic_protection.sample(uv.x, uv.y) : 0.f;
    if (protection.manual_at)
      if (std::optional<float> const manual = protection.manual_at(world_x, world_z))
        value = *manual >= 0.f ? std::max(value, *manual) : value * (1.f + *manual);
    return std::clamp(value, 0.f, 1.f);
  };
  world->for_all_chunks_in_rect(center, traversal_radius, [&](MapChunk* chunk)
  {
    bool chunk_visited = false;
    for (std::size_t index = 0; index < mapbufsize; ++index)
    {
      glm::vec3 const& vertex = chunk->mVertices[index];
      glm::vec2 const uv = sourceCoordinates(vertex.x, vertex.z, center, radius, rotation_degrees);
      float const nx = uv.x * 2.f - 1.f;
      float const nz = uv.y * 2.f - 1.f;
      // Preserve the core with one vertical translation. Regular shapes extend a bounded edge
      // sample into the skirt. Painted exact features instead generate a support from the real
      // boundary displacement, so the saved mountain remains exact without ending in a wall.
      glm::vec2 const sample_uv = _shape == MapStampShape::Painted
          ? clampToPaintedBoundary(uv)
          : (height_mode != MapStampHeightMode::ConformToTerrain
              ? clampToShapeBoundary(uv, _shape) : uv);
      bool const painted_exact_skirt = height_mode == MapStampHeightMode::ExactFeature
          && _shape == MapStampShape::Painted && paintedFootprintWeight(uv.x, uv.y) < .5f;
      float const source_height = sampleHeight(sample_uv.x, sample_uv.y,
          mountain_blend ? MapStampHeightMode::ExactFeature : height_mode);
      float const saved_height = source_height * height_scale;
      float target = 0.f;
      if (height_mode == MapStampHeightMode::ConformToTerrain)
      {
        target = static_cast<float>(evaluateQuadratic(*target_macro, nx, nz))
            + saved_height + height_offset;
      }
      else if (mountain_blend)
      {
        float const destination_base = target_macro
            ? static_cast<float>(evaluateQuadratic(*target_macro, nx, nz))
            : anchor_height;
        float const positive_relief = std::max(
            0.f, (source_height - exact_source_base_height) * height_scale);
        // Mountain composition is a height union. It can add a captured feature,
        // but it cannot excavate or overwrite a taller mountain already present.
        target = std::max(vertex.y, destination_base + positive_relief + height_offset);
      }
      else
      {
        target = exact_anchor_height + saved_height + height_offset;
      }
      float const feature_weight = height_mode == MapStampHeightMode::ExactFeature
          ? exactFeatureWeight(uv.x, uv.y) : 1.f;
      float coverage = placementCoverage(uv.x, uv.y, hardness, opacity, height_mode);
      if (painted_exact_skirt)
      {
        float const boundary_nx = sample_uv.x * 2.f - 1.f;
        float const boundary_nz = sample_uv.y * 2.f - 1.f;
        float const destination_boundary = target_macro
            ? static_cast<float>(evaluateQuadratic(*target_macro, boundary_nx, boundary_nz))
            : anchor_height;
        float const boundary_displacement = target - destination_boundary;
        coverage = paintedExactSkirtBlend(paintedOutsideDistance(uv.x, uv.y), radius,
                                           hardness, opacity, boundary_displacement);
        // Apply the measured boundary displacement on top of the unmodified local
        // destination. This follows hills beneath the skirt instead of flattening them.
        target = vertex.y + boundary_displacement;
      }
      coverage *= feature_weight * (1.f - protectionAt(vertex.x, vertex.z, uv));
      if (coverage <= 0.f)
        continue;
      float const next = std::lerp(vertex.y, target, coverage);
      if (next == vertex.y)
        continue;
      visitor(chunk, index, next);
      chunk_visited = true;
    }
    return mark_tiles_changed && chunk_visited;
  });
  return true;
}

bool MapStampAsset::previewTerrain(
    World* world, glm::vec3 const& center, float radius, float rotation_degrees,
    float hardness, float height_scale, float height_offset, float opacity,
    MapStampProtectionSettings const& protection, MapStampHeightMode height_mode,
    bool update_textures, std::vector<MapChunk*>& preview_chunks,
    std::vector<std::vector<glm::vec3>>& preview_lines) const
{
  std::unordered_map<MapChunk*, std::array<float, mapbufsize>> preview_heights;
  if (!visitTerrainPlacement(world, center, radius, rotation_degrees, hardness,
                             height_scale, height_offset, opacity, protection, height_mode, false,
      [&](MapChunk* chunk, std::size_t index, float height)
      {
        auto [found, inserted] = preview_heights.try_emplace(chunk);
        if (inserted)
          for (std::size_t i = 0; i < mapbufsize; ++i)
            found->second[i] = chunk->mVertices[i].y;
        found->second[index] = height;
      }))
  {
    return false;
  }

  bool const painted_exact = height_mode == MapStampHeightMode::ExactFeature
      && _shape == MapStampShape::Painted;
  bool const mountain_blend = height_mode == MapStampHeightMode::MountainBlend;
  std::optional<QuadraticCoefficients> const texture_target_macro =
      update_textures && !_textures.empty()
          && (protection.automatic || painted_exact || mountain_blend)
      ? fitTargetMacroSurface(world, center, radius, rotation_degrees, _shape,
          _shape == MapStampShape::Painted
              ? std::function<bool(float, float)>{[this](float nx, float nz)
                {
                  float const u = (nx + 1.f) * .5f;
                  float const v = (nz + 1.f) * .5f;
                  return paintedFootprintWeight(u, v) < .5f
                      && paintedOutsideDistance(u, v) <= painted_capture_extent - 1.f;
                }}
              : std::function<bool(float, float)>{}, mountain_blend)
      : std::optional<QuadraticCoefficients>{};
  float texture_exact_anchor_height = center.y;
  float texture_source_base_height = 0.f;
  if (painted_exact || mountain_blend)
  {
    texture_source_base_height = exactSourceBaseHeight().value_or(0.f);
    MapChunk* anchor_chunk = nullptr;
    world->for_maybe_chunk_at(center, [&](MapChunk* chunk)
    {
      anchor_chunk = chunk;
      return true;
    });
    if (std::optional<float> const anchor = terrainHeightAt(anchor_chunk, center))
      texture_exact_anchor_height = painted_exact
          ? *anchor - texture_source_base_height * height_scale : *anchor;
  }
  ProtectionGrid const texture_automatic_protection =
      protection.automatic && texture_target_macro
      ? buildAutomaticProtectionGrid(world, center, radius, rotation_degrees, _shape,
                                     *texture_target_macro, protection)
      : ProtectionGrid{};
  auto textureProtectionAt = [&](float world_x, float world_z, glm::vec2 const& uv)
  {
    float value = protection.automatic
        ? texture_automatic_protection.sample(uv.x, uv.y) : 0.f;
    if (protection.manual_at)
      if (std::optional<float> const manual = protection.manual_at(world_x, world_z))
        value = *manual >= 0.f ? std::max(value, *manual) : value * (1.f + *manual);
    return std::clamp(value, 0.f, 1.f);
  };

  // Do not replace the opaque ADT height texture while dragging. A tall in-place preview can
  // cross the near plane and black out the viewport. A render-only wire grid carries the exact
  // target heights instead, while the destination surface receives only a transient texture
  // preview. The real terrain is mutated once, on mouse release.
  std::map<long long, std::vector<glm::vec3>> horizontal_lines;
  std::map<long long, std::vector<glm::vec3>> vertical_lines;
  preview_chunks.reserve(preview_chunks.size() + preview_heights.size());
  for (auto& [chunk, heights] : preview_heights)
  {
    for (int row = 0; row < 9; ++row)
      for (int column = 0; column < 9; ++column)
      {
        int const index = row * 17 + column;
        glm::vec3 point = chunk->mVertices[index];
        point.y = heights[index] + .15f;
        horizontal_lines[std::llround(point.z / UNITSIZE)].push_back(point);
        vertical_lines[std::llround(point.x / UNITSIZE)].push_back(point);
      }

    if (update_textures && !_textures.empty())
    {
      TextureSet* destination = chunk->getTextureSet();
      destination->apply_alpha_changes();

      std::vector<std::string> destination_names;
      std::vector<std::array<float, 64 * 64>> destination_weights(destination->num());
      for (std::size_t layer = 0; layer < destination->num(); ++layer)
        destination_names.push_back(destination->filename(layer));
      for (int pixel = 0; pixel < 64 * 64; ++pixel)
      {
        std::array<float, 4> const weights = chunkWeightsAt(destination, pixel);
        for (std::size_t layer = 0; layer < destination->num(); ++layer)
          destination_weights[layer][pixel] = weights[layer];
      }

      std::vector<std::string> names = destination_names;
      std::unordered_map<std::string, std::size_t> indices;
      std::unordered_map<std::string, std::uint32_t> flags;
      for (std::size_t layer = 0; layer < names.size(); ++layer)
      {
        indices.emplace(names[layer], layer);
        flags.emplace(names[layer], destination->getMCLYEntries()[layer].flags);
      }
      for (MapStampTexture const& texture : _textures)
      {
        // Existing destination metadata owns a shared texture identity. Replacing
        // its animation/effect flags would also alter terrain outside the stamp.
        flags.try_emplace(texture.filename, texture.layer.flags);
        if (!indices.contains(texture.filename))
        {
          indices.emplace(texture.filename, names.size());
          names.push_back(texture.filename);
        }
      }

      std::vector<std::array<float, 64 * 64>> mixed(names.size());
      std::vector<double> protected_totals(names.size(), 0.0);
      for (int z = 0; z < 64; ++z)
        for (int x = 0; x < 64; ++x)
        {
          int const pixel = z * 64 + x;
          float const world_x = chunk->xbase + (x + .5f) * CHUNKSIZE / 64.f;
          float const world_z = chunk->zbase + (z + .5f) * CHUNKSIZE / 64.f;
          glm::vec2 const uv = sourceCoordinates(
              world_x, world_z, center, radius, rotation_degrees);
          float const feature_weight = height_mode == MapStampHeightMode::ExactFeature
              ? exactFeatureWeight(uv.x, uv.y) : 1.f;
          glm::vec2 const texture_uv = _shape == MapStampShape::Painted
              ? clampToPaintedBoundary(uv)
              : (height_mode != MapStampHeightMode::ConformToTerrain
                  ? clampToShapeBoundary(uv, _shape) : uv);
          float coverage = placementCoverage(uv.x, uv.y, hardness, opacity, height_mode);
          if (mountain_blend)
          {
            coverage *= 1.f - textureProtectionAt(world_x, world_z, uv);
            float const nx = uv.x * 2.f - 1.f;
            float const nz = uv.y * 2.f - 1.f;
            float const destination_base = texture_target_macro
                ? static_cast<float>(evaluateQuadratic(*texture_target_macro, nx, nz))
                : texture_exact_anchor_height;
            float const source_height = sampleHeight(
                texture_uv.x, texture_uv.y, MapStampHeightMode::ExactFeature);
            float const positive_relief = std::max(
                0.f, (source_height - texture_source_base_height) * height_scale);
            float const desired_height = destination_base + positive_relief + height_offset;
            glm::vec3 const position{world_x, center.y, world_z};
            float const destination_height = terrainHeightAt(chunk, position)
                .value_or(destination_base);
            float const height_contribution = std::max(
                0.f, desired_height - destination_height);
            coverage = heightContributionTextureBlend(coverage, height_contribution);
          }
          else if (painted_exact && paintedFootprintWeight(uv.x, uv.y) < .5f)
          {
            float const boundary_nx = texture_uv.x * 2.f - 1.f;
            float const boundary_nz = texture_uv.y * 2.f - 1.f;
            float const boundary_target = texture_exact_anchor_height
                + sampleHeight(texture_uv.x, texture_uv.y, height_mode) * height_scale
                + height_offset;
            float const destination_boundary = texture_target_macro
                ? static_cast<float>(evaluateQuadratic(
                    *texture_target_macro, boundary_nx, boundary_nz))
                : texture_exact_anchor_height;
            float const boundary_displacement = boundary_target - destination_boundary;
            float const skirt_blend = paintedExactSkirtBlend(
                paintedOutsideDistance(uv.x, uv.y), radius, hardness, opacity,
                boundary_displacement);
            coverage = heightContributionTextureBlend(skirt_blend, boundary_displacement);
          }
          if (!mountain_blend)
            coverage *= feature_weight
                * (1.f - textureProtectionAt(world_x, world_z, uv));

          for (std::size_t layer = 0; layer < destination_names.size(); ++layer)
          {
            std::size_t const output_layer = indices[destination_names[layer]];
            float const contribution = destination_weights[layer][pixel] * (1.f - coverage);
            mixed[output_layer][pixel] += contribution;
            protected_totals[output_layer] += contribution;
          }
          if (coverage > 0.f)
          {
            std::array<float, maximum_textures> sampled_weights;
            float sampled_total = 0.f;
            for (std::size_t layer = 0; layer < _textures.size(); ++layer)
            {
              sampled_weights[layer] = sampleTexture(layer, texture_uv.x, texture_uv.y);
              sampled_total += sampled_weights[layer];
            }
            float const source_scale = sampled_total > .01f ? 255.f / sampled_total : 0.f;
            for (std::size_t layer = 0; layer < _textures.size(); ++layer)
              mixed[indices[_textures[layer].filename]][pixel]
                  += sampled_weights[layer] * source_scale * coverage;
          }
        }

      std::vector<std::size_t> const ranked = selectStampTextureLayers(mixed, protected_totals);

      chunk_mover_texture_preview texture_preview;
      for (std::size_t output_layer = 0; output_layer < ranked.size(); ++output_layer)
      {
        std::string const& name = names[ranked[output_layer]];
        texture_preview.textures.emplace_back(name, Noggit::NoggitRenderContext::MAP_VIEW);
        texture_preview.flags[output_layer] = flags[name];
      }
      for (int pixel = 0; pixel < 64 * 64; ++pixel)
      {
        float total = 0.f;
        for (std::size_t layer : ranked)
          total += mixed[layer][pixel];
        if (total <= 0.f)
          continue;
        for (std::size_t output_layer = 1; output_layer < ranked.size(); ++output_layer)
          texture_preview.alphamaps[output_layer - 1][pixel]
              = mixed[ranked[output_layer]][pixel] / total;
      }
      destination->setChunkMoverTexturePreview(std::move(texture_preview));
    }
    preview_chunks.push_back(chunk);
  }

  auto appendLines = [&](auto& grouped_lines, bool sort_x)
  {
    for (auto& [coordinate, line] : grouped_lines)
    {
      std::sort(line.begin(), line.end(), [sort_x](glm::vec3 const& lhs, glm::vec3 const& rhs)
      {
        return sort_x ? lhs.x < rhs.x : lhs.z < rhs.z;
      });
      line.erase(std::unique(line.begin(), line.end(), [sort_x](glm::vec3 const& lhs,
                                                                glm::vec3 const& rhs)
      {
        return sort_x ? std::abs(lhs.x - rhs.x) < .01f : std::abs(lhs.z - rhs.z) < .01f;
      }), line.end());
      if (line.size() >= 2)
        preview_lines.push_back(std::move(line));
    }
  };
  appendLines(horizontal_lines, true);
  appendLines(vertical_lines, false);
  return true;
}

bool MapStampAsset::apply(World* world, glm::vec3 const& center, float radius,
                          float rotation_degrees, float hardness, float height_scale,
                          float height_offset, float opacity,
                          MapStampProtectionSettings const& protection,
                          MapStampHeightMode height_mode)
{
  if (!world || !valid() || !NOGGIT_CUR_ACTION || radius <= 0.f)
    return false;

  // Textures and terrain must use the same protection mask sampled from the
  // unmodified destination. Terrain placement below mutates the real vertices.
  bool const painted_exact = height_mode == MapStampHeightMode::ExactFeature
      && _shape == MapStampShape::Painted;
  bool const mountain_blend = height_mode == MapStampHeightMode::MountainBlend;
  std::optional<QuadraticCoefficients> const texture_target_macro =
      !_textures.empty() && (protection.automatic || painted_exact || mountain_blend)
      ? fitTargetMacroSurface(world, center, radius, rotation_degrees, _shape,
          _shape == MapStampShape::Painted
              ? std::function<bool(float, float)>{[this](float nx, float nz)
                {
                  float const u = (nx + 1.f) * .5f;
                  float const v = (nz + 1.f) * .5f;
                  return paintedFootprintWeight(u, v) < .5f
                      && paintedOutsideDistance(u, v) <= painted_capture_extent - 1.f;
                }}
              : std::function<bool(float, float)>{}, mountain_blend)
      : std::optional<QuadraticCoefficients>{};
  float texture_exact_anchor_height = center.y;
  float texture_source_base_height = 0.f;
  if (painted_exact || mountain_blend)
  {
    texture_source_base_height = exactSourceBaseHeight().value_or(0.f);
    MapChunk* anchor_chunk = nullptr;
    world->for_maybe_chunk_at(center, [&](MapChunk* chunk)
    {
      anchor_chunk = chunk;
      return true;
    });
    if (std::optional<float> const anchor = terrainHeightAt(anchor_chunk, center))
      texture_exact_anchor_height = painted_exact
          ? *anchor - texture_source_base_height * height_scale : *anchor;
  }
  ProtectionGrid const texture_automatic_protection =
      protection.automatic && texture_target_macro
      ? buildAutomaticProtectionGrid(world, center, radius, rotation_degrees, _shape,
                                     *texture_target_macro, protection)
      : ProtectionGrid{};
  auto textureProtectionAt = [&](float world_x, float world_z, glm::vec2 const& uv)
  {
    float value = protection.automatic
        ? texture_automatic_protection.sample(uv.x, uv.y) : 0.f;
    if (protection.manual_at)
      if (std::optional<float> const manual = protection.manual_at(world_x, world_z))
        value = *manual >= 0.f ? std::max(value, *manual) : value * (1.f + *manual);
    return std::clamp(value, 0.f, 1.f);
  };

  float const traversal_radius = rotatedFootprintBoundingRadius(
      radius * (_shape == MapStampShape::Painted
          ? (height_mode == MapStampHeightMode::ExactFeature
              ? 1.5f : 1.f + std::clamp(1.f - hardness, .05f, .5f))
          : placementExtent(hardness, height_mode)), rotation_degrees, _shape);
  std::unordered_map<MapChunk*, std::array<float, 64 * 64>> mountain_texture_coverage;
  if (mountain_blend && !_textures.empty())
  {
    world->for_all_chunks_in_rect(center, traversal_radius, [&](MapChunk* chunk)
    {
      auto& chunk_coverage = mountain_texture_coverage[chunk];
      chunk_coverage.fill(0.f);
      for (int z = 0; z < 64; ++z)
        for (int x = 0; x < 64; ++x)
        {
          int const pixel = z * 64 + x;
          float const world_x = chunk->xbase + (x + .5f) * CHUNKSIZE / 64.f;
          float const world_z = chunk->zbase + (z + .5f) * CHUNKSIZE / 64.f;
          glm::vec2 const uv = sourceCoordinates(
              world_x, world_z, center, radius, rotation_degrees);
          glm::vec2 const source_uv = _shape == MapStampShape::Painted
              ? clampToPaintedBoundary(uv) : clampToShapeBoundary(uv, _shape);
          float coverage = placementCoverage(
              uv.x, uv.y, hardness, opacity, height_mode)
              * (1.f - textureProtectionAt(world_x, world_z, uv));
          if (coverage <= 0.f)
            continue;
          float const nx = uv.x * 2.f - 1.f;
          float const nz = uv.y * 2.f - 1.f;
          float const destination_base = texture_target_macro
              ? static_cast<float>(evaluateQuadratic(*texture_target_macro, nx, nz))
              : texture_exact_anchor_height;
          float const source_height = sampleHeight(
              source_uv.x, source_uv.y, MapStampHeightMode::ExactFeature);
          float const positive_relief = std::max(
              0.f, (source_height - texture_source_base_height) * height_scale);
          float const desired_height = destination_base + positive_relief + height_offset;
          glm::vec3 const position{world_x, center.y, world_z};
          float const destination_height = terrainHeightAt(chunk, position)
              .value_or(destination_base);
          float const height_contribution = std::max(
              0.f, desired_height - destination_height);
          chunk_coverage[pixel] = heightContributionTextureBlend(
              coverage, height_contribution);
        }
      return false;
    });
  }

  std::vector<MapChunk*> terrain_chunks;
  std::unordered_map<MapChunk*, bool> registered_chunks;
  if (!visitTerrainPlacement(world, center, radius, rotation_degrees, hardness,
      height_scale, height_offset, opacity, protection, height_mode, true,
      [&](MapChunk* chunk, std::size_t index, float height)
      {
        if (registered_chunks.emplace(chunk, true).second)
        {
          NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
          terrain_chunks.push_back(chunk);
        }
        chunk->mVertices[index].y = height;
      }))
  {
    return false;
  }

  for (MapChunk* chunk : terrain_chunks)
  {
    chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);
    world->recalc_norms(chunk);
  }

  if (_textures.empty())
    return !terrain_chunks.empty();

  bool texture_changed = world->for_all_chunks_in_rect(
      center, traversal_radius, [&](MapChunk* chunk)
  {
    TextureSet* destination = chunk->getTextureSet();
    destination->apply_alpha_changes();

    std::vector<std::string> destination_names;
    std::vector<layer_info> destination_layers;
    std::vector<std::array<float, 64 * 64>> destination_weights(destination->num());
    for (std::size_t layer = 0; layer < destination->num(); ++layer)
    {
      destination_names.push_back(destination->filename(layer));
      destination_layers.push_back(destination->getMCLYEntries()[layer]);
    }
    for (int pixel = 0; pixel < 64 * 64; ++pixel)
    {
      std::array<float, 4> const weights = chunkWeightsAt(destination, pixel);
      for (std::size_t layer = 0; layer < destination->num(); ++layer)
        destination_weights[layer][pixel] = weights[layer];
    }

    std::vector<std::string> names = destination_names;
    std::unordered_map<std::string, std::size_t> indices;
    std::unordered_map<std::string, layer_info> metadata;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
      indices.emplace(names[i], i);
      metadata.emplace(names[i], destination_layers[i]);
    }
    for (MapStampTexture const& texture : _textures)
    {
      // Keep destination metadata for shared textures so a partial stamp cannot
      // change animation or ground-effect behavior across the whole chunk.
      metadata.try_emplace(texture.filename, texture.layer);
      if (!indices.contains(texture.filename))
      {
        indices.emplace(texture.filename, names.size());
        names.push_back(texture.filename);
      }
    }

    std::vector<std::array<float, 64 * 64>> mixed(names.size());
    std::vector<double> protected_totals(names.size(), 0.0);
    bool touched = false;
    for (int z = 0; z < 64; ++z)
      for (int x = 0; x < 64; ++x)
      {
        int const pixel = z * 64 + x;
        float const world_x = chunk->xbase + (x + .5f) * CHUNKSIZE / 64.f;
        float const world_z = chunk->zbase + (z + .5f) * CHUNKSIZE / 64.f;
        glm::vec2 const uv = sourceCoordinates(world_x, world_z, center, radius, rotation_degrees);
        float const feature_weight = height_mode == MapStampHeightMode::ExactFeature
            ? exactFeatureWeight(uv.x, uv.y) : 1.f;
        glm::vec2 const texture_uv = _shape == MapStampShape::Painted
            ? clampToPaintedBoundary(uv)
            : (height_mode != MapStampHeightMode::ConformToTerrain
                ? clampToShapeBoundary(uv, _shape) : uv);
        float coverage = 0.f;
        if (mountain_blend)
        {
          auto const found = mountain_texture_coverage.find(chunk);
          if (found != mountain_texture_coverage.end())
            coverage = found->second[pixel];
        }
        else
        {
          coverage = placementCoverage(uv.x, uv.y, hardness, opacity, height_mode);
          if (painted_exact && paintedFootprintWeight(uv.x, uv.y) < .5f)
          {
            float const boundary_nx = texture_uv.x * 2.f - 1.f;
            float const boundary_nz = texture_uv.y * 2.f - 1.f;
            float const boundary_target = texture_exact_anchor_height
                + sampleHeight(texture_uv.x, texture_uv.y, height_mode) * height_scale
                + height_offset;
            float const destination_boundary = texture_target_macro
                ? static_cast<float>(evaluateQuadratic(
                    *texture_target_macro, boundary_nx, boundary_nz))
                : texture_exact_anchor_height;
            float const boundary_displacement = boundary_target - destination_boundary;
            float const skirt_blend = paintedExactSkirtBlend(
                paintedOutsideDistance(uv.x, uv.y), radius, hardness, opacity,
                boundary_displacement);
            coverage = heightContributionTextureBlend(skirt_blend, boundary_displacement);
          }
          coverage *= feature_weight
              * (1.f - textureProtectionAt(world_x, world_z, uv));
        }
        touched |= coverage > 0.f;

        for (std::size_t layer = 0; layer < destination_names.size(); ++layer)
        {
          std::size_t const output_layer = indices[destination_names[layer]];
          float const contribution = destination_weights[layer][pixel] * (1.f - coverage);
          mixed[output_layer][pixel] += contribution;
          protected_totals[output_layer] += contribution;
        }
        if (coverage > 0.f)
        {
          std::array<float, maximum_textures> sampled_weights;
          float sampled_total = 0.f;
          for (std::size_t layer = 0; layer < _textures.size(); ++layer)
          {
            sampled_weights[layer] = sampleTexture(layer, texture_uv.x, texture_uv.y);
            sampled_total += sampled_weights[layer];
          }
          float const source_scale = sampled_total > .01f ? 255.f / sampled_total : 0.f;
          for (std::size_t layer = 0; layer < _textures.size(); ++layer)
            mixed[indices[_textures[layer].filename]][pixel]
                += sampled_weights[layer] * source_scale * coverage;
        }
      }

    if (!touched)
      return false;

    std::vector<std::size_t> const ranked = selectStampTextureLayers(mixed, protected_totals);

    auto output = std::make_unique<tmp_edit_alpha_values>();
    for (auto& layer : output->map)
      layer.fill(0.f);
    for (int pixel = 0; pixel < 64 * 64; ++pixel)
    {
      float total = 0.f;
      for (std::size_t layer : ranked)
        total += mixed[layer][pixel];
      if (total <= 0.f)
      {
        output->map[0][pixel] = 255.f;
        continue;
      }
      for (std::size_t output_layer = 0; output_layer < ranked.size(); ++output_layer)
        output->map[output_layer][pixel] = mixed[ranked[output_layer]][pixel] * 255.f / total;
    }

    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    destination->getTextures()->clear();
    for (std::size_t layer : ranked)
      destination->getTextures()->emplace_back(names[layer], Noggit::NoggitRenderContext::MAP_VIEW);
    destination->setNTextures(ranked.size());

    std::array<std::unique_ptr<Alphamap>, MAX_ALPHAMAPS> alphamaps;
    for (std::size_t layer = 1; layer < ranked.size(); ++layer)
      alphamaps[layer - 1] = std::make_unique<Alphamap>();
    destination->setAlphamaps(alphamaps);
    for (std::size_t layer = 0; layer < 4; ++layer)
      destination->getMCLYEntries()[layer] = layer_info{};
    for (std::size_t output_layer = 0; output_layer < ranked.size(); ++output_layer)
      destination->getMCLYEntries()[output_layer] = metadata[names[ranked[output_layer]]];
    destination->getTempAlphamaps() = std::move(output);
    destination->apply_alpha_changes();
    destination->markDirty();
    chunk->registerChunkUpdate(ChunkUpdateFlags::GROUND_EFFECT);
    return true;
  });

  if (texture_changed)
    Noggit::DetailDoodads::bumpDbcStamp();
  return !terrain_chunks.empty() || texture_changed;
}

bool MapStampAsset::save(QString const& path, QString* error) const
{
  auto fail = [error](QString const& message)
  {
    if (error)
      *error = message;
    return false;
  };
  if (!valid())
    return fail("There is no captured map stamp to save.");

  QByteArray payload;
  QDataStream stream(&payload, QIODevice::WriteOnly);
  configureStream(stream);
  stream << _source_radius << static_cast<quint16>(_height_resolution)
         << static_cast<quint16>(_texture_resolution) << static_cast<quint8>(_shape)
         << static_cast<quint8>(_height_is_relief ? 1 : 0) << _sample_extent;
  for (float height : _relative_heights)
    stream << height;
  quint16 const mask_resolution = _shape == MapStampShape::Painted
      ? static_cast<quint16>(_height_resolution) : 0;
  stream << mask_resolution;
  if (mask_resolution
      && stream.writeRawData(reinterpret_cast<char const*>(_painted_footprint_mask.data()),
                             static_cast<int>(_painted_footprint_mask.size()))
         != static_cast<int>(_painted_footprint_mask.size()))
    return fail("Unable to encode the painted stamp footprint.");
  stream << static_cast<quint32>(_textures.size());
  for (MapStampTexture const& texture : _textures)
  {
    writeString(stream, texture.filename);
    stream << static_cast<quint32>(texture.layer.flags)
           << static_cast<quint32>(texture.layer.effectID);
    if (stream.writeRawData(reinterpret_cast<char const*>(texture.weights.data()),
                            static_cast<int>(texture.weights.size()))
        != static_cast<int>(texture.weights.size()))
      return fail("Unable to encode the map-stamp texture data.");
  }
  if (stream.status() != QDataStream::Ok)
    return fail("Unable to encode the map stamp.");

  QByteArray const checksum = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
  QByteArray const compressed = qCompress(payload, 6);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return fail(QString("Unable to open %1 for writing: %2").arg(path, file.errorString()));
  QDataStream output(&file);
  configureStream(output);
  if (output.writeRawData("NZST", 4) != 4)
    return fail("Unable to write the map-stamp header.");
  output << format_version << checksum << compressed;
  if (output.status() != QDataStream::Ok || !file.commit())
    return fail(QString("Unable to finish writing %1: %2").arg(path, file.errorString()));
  return true;
}

bool MapStampAsset::load(QString const& path, QString* error)
{
  auto fail = [error](QString const& message)
  {
    if (error)
      *error = message;
    return false;
  };
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return fail(QString("Unable to open %1: %2").arg(path, file.errorString()));
  if (file.size() <= 0 || file.size() > maximum_file_size)
    return fail("The map-stamp file is empty or too large.");

  QDataStream input(&file);
  configureStream(input);
  char magic[4]{};
  quint16 version = 0;
  QByteArray checksum;
  QByteArray compressed;
  if (input.readRawData(magic, 4) != 4 || std::memcmp(magic, "NZST", 4) != 0)
    return fail("This is not a Noggit map stamp.");
  input >> version >> checksum >> compressed;
  if (input.status() != QDataStream::Ok || version < 1 || version > format_version)
    return fail(QString("Unsupported map-stamp format version %1.").arg(version));
  QByteArray const payload = qUncompress(compressed);
  if (payload.isEmpty() || payload.size() > maximum_file_size
      || QCryptographicHash::hash(payload, QCryptographicHash::Sha256) != checksum)
    return fail("The map stamp is corrupt or incomplete.");

  QDataStream stream(payload);
  configureStream(stream);
  float source_radius = 0.f;
  quint16 height_resolution = 0;
  quint16 texture_resolution = 0;
  quint8 stored_shape = static_cast<quint8>(MapStampShape::Circle);
  quint8 stored_height_mode = 0;
  float sample_extent = 1.f;
  stream >> source_radius >> height_resolution;
  if (version == 1)
    texture_resolution = height_resolution;
  else
    stream >> texture_resolution;
  if (version >= 3)
    stream >> stored_shape;
  if (version >= 5)
    stream >> stored_height_mode;
  if (version >= 6)
    stream >> sample_extent;
  MapStampShape const shape = static_cast<MapStampShape>(stored_shape);
  if (!std::isfinite(source_radius) || source_radius <= 0.f
      || height_resolution < 3 || height_resolution > maximum_height_resolution
      || texture_resolution < 3 || texture_resolution > maximum_texture_resolution
      || (shape != MapStampShape::Circle && shape != MapStampShape::Square
          && shape != MapStampShape::Painted)
      || (shape == MapStampShape::Painted && version < 7)
      || stored_height_mode > 1 || !std::isfinite(sample_extent)
      || sample_extent < 1.f || sample_extent > maximum_stored_extent)
    return fail("The map stamp has invalid dimensions.");
  if (version == 1 && height_resolution != legacy_resolution)
    return fail("The legacy map stamp has invalid dimensions.");
  std::vector<float> heights(static_cast<std::size_t>(height_resolution) * height_resolution);
  for (float& height : heights)
    stream >> height;
  if (stream.status() != QDataStream::Ok
      || !std::all_of(heights.begin(), heights.end(), [](float height)
         { return std::isfinite(height); }))
    return fail("The map stamp contains invalid terrain data.");
  std::vector<std::uint8_t> painted_mask;
  if (version >= 7)
  {
    quint16 mask_resolution = 0;
    stream >> mask_resolution;
    if ((shape == MapStampShape::Painted && mask_resolution != height_resolution)
        || (shape != MapStampShape::Painted && mask_resolution != 0))
      return fail("The map stamp contains an invalid painted footprint.");
    if (mask_resolution)
    {
      painted_mask.resize(static_cast<std::size_t>(mask_resolution) * mask_resolution);
      if (stream.readRawData(reinterpret_cast<char*>(painted_mask.data()),
                             static_cast<int>(painted_mask.size()))
          != static_cast<int>(painted_mask.size())
          || std::none_of(painted_mask.begin(), painted_mask.end(), [](std::uint8_t value)
             { return value >= 128; }))
        return fail("The map stamp contains an invalid painted footprint.");
    }
  }
  bool const stored_as_relief = version >= 5 ? stored_height_mode != 0 : version >= 3;
  std::vector<float> relief_heights = heights;
  if (version == 3 || !stored_as_relief)
    detrendHeightRelief(relief_heights, height_resolution, shape, sample_extent);
  if (stored_as_relief)
    heights = relief_heights;
  quint32 texture_count = 0;
  stream >> texture_count;
  if (stream.status() != QDataStream::Ok || texture_count > maximum_textures)
    return fail("The map stamp has an invalid texture count.");
  quint64 const texture_texels = static_cast<quint64>(texture_resolution) * texture_resolution;
  quint64 const texture_bytes = texture_texels * texture_count * (version == 1 ? 4u : 1u);
  if (texture_bytes > static_cast<quint64>(maximum_file_size))
    return fail("The map stamp contains too much texture data.");
  std::vector<MapStampTexture> textures(texture_count);
  for (MapStampTexture& texture : textures)
  {
    quint32 flags = 0;
    quint32 effect = 0;
    if (!readString(stream, texture.filename))
      return fail("The map stamp contains an invalid texture path.");
    stream >> flags >> effect;
    texture.layer.flags = flags;
    texture.layer.effectID = effect;
    texture.weights.resize(static_cast<std::size_t>(texture_resolution) * texture_resolution);
    if (version == 1)
    {
      for (std::uint8_t& weight : texture.weights)
      {
        float legacy_weight = 0.f;
        stream >> legacy_weight;
        if (!std::isfinite(legacy_weight))
          return fail("The map stamp contains invalid texture data.");
        weight = static_cast<std::uint8_t>(std::clamp(std::lround(legacy_weight), 0l, 255l));
      }
    }
    else if (stream.readRawData(reinterpret_cast<char*>(texture.weights.data()),
                                static_cast<int>(texture.weights.size()))
             != static_cast<int>(texture.weights.size()))
      return fail("The map stamp ended unexpectedly.");
  }
  if (stream.status() != QDataStream::Ok || !stream.atEnd())
    return fail("The map stamp ended unexpectedly.");

  _source_radius = source_radius;
  _height_resolution = height_resolution;
  _texture_resolution = texture_resolution;
  _shape = shape;
  _height_is_relief = stored_as_relief;
  _sample_extent = sample_extent;
  _relative_heights = std::move(heights);
  _relief_heights = std::move(relief_heights);
  _painted_footprint_mask = std::move(painted_mask);
  rebuildPaintedFootprintData();
  if (_shape == MapStampShape::Painted && !_height_is_relief)
  {
    _relief_heights = _relative_heights;
    detrendPaintedHeightRelief(_relief_heights, _height_resolution,
                               _painted_footprint_mask, _painted_outside_distances,
                               _sample_extent);
  }
  rebuildExactFeatureMask();
  _textures = std::move(textures);
  return true;
}

bool MapStampAsset::valid() const
{
  if (!std::isfinite(_source_radius) || _source_radius <= 0.f
      || !std::isfinite(_sample_extent) || _sample_extent < 1.f
      || _sample_extent > maximum_stored_extent
      || _height_resolution < 3 || _height_resolution > maximum_height_resolution
      || _texture_resolution < 3 || _texture_resolution > maximum_texture_resolution
      || (_shape != MapStampShape::Circle && _shape != MapStampShape::Square
          && _shape != MapStampShape::Painted)
      || _relative_heights.size()
         != static_cast<std::size_t>(_height_resolution) * _height_resolution
      || _relief_heights.size()
         != static_cast<std::size_t>(_height_resolution) * _height_resolution
      || (!_height_is_relief && _exact_feature_mask.size()
          != static_cast<std::size_t>(_height_resolution) * _height_resolution)
      || (_shape == MapStampShape::Painted
          && (_painted_footprint_mask.size()
              != static_cast<std::size_t>(_height_resolution) * _height_resolution
              || _painted_outside_distances.size() != _painted_footprint_mask.size()
              || _painted_nearest_indices.size() != _painted_footprint_mask.size()))
      || (_shape != MapStampShape::Painted && !_painted_footprint_mask.empty()))
    return false;
  if (!std::all_of(_relative_heights.begin(), _relative_heights.end(), [](float height)
      { return std::isfinite(height); }))
    return false;
  if (!std::all_of(_relief_heights.begin(), _relief_heights.end(), [](float height)
      { return std::isfinite(height); }))
    return false;
  return std::all_of(_textures.begin(), _textures.end(), [&](MapStampTexture const& texture)
  {
    return texture.weights.size()
        == static_cast<std::size_t>(_texture_resolution) * _texture_resolution;
  });
}

float MapStampAsset::sourceRadius() const
{
  return _source_radius;
}

int MapStampAsset::heightResolution() const
{
  return _height_resolution;
}

int MapStampAsset::textureResolution() const
{
  return _texture_resolution;
}

MapStampShape MapStampAsset::shape() const
{
  return _shape;
}

std::size_t MapStampAsset::textureCount() const
{
  return _textures.size();
}

bool MapStampAsset::supportsExactHeight() const
{
  return valid() && !_height_is_relief;
}

float MapStampAsset::footprintBoundingRadius(float radius, float rotation_degrees,
                                             float hardness,
                                             MapStampHeightMode height_mode) const
{
  return rotatedFootprintBoundingRadius(
      radius * (_shape == MapStampShape::Painted
          ? (height_mode == MapStampHeightMode::ExactFeature
              ? 1.5f : 1.f + std::clamp(1.f - hardness, .05f, .5f))
          : placementExtent(hardness, height_mode)), rotation_degrees, _shape);
}

QImage MapStampAsset::previewImage() const
{
  constexpr int preview_resolution = 129;
  QImage image(preview_resolution, preview_resolution, QImage::Format_RGBA8888);
  image.fill(Qt::black);
  if (!valid())
    return image;
  bool const exact_height = !_height_is_relief;
  MapStampHeightMode const preview_mode = exact_height
      ? MapStampHeightMode::ExactFeature : MapStampHeightMode::ConformToTerrain;

  float maximum = 0.f;
  for (int y = 0; y < preview_resolution; ++y)
    for (int x = 0; x < preview_resolution; ++x)
    {
      float const u = static_cast<float>(x) / (preview_resolution - 1);
      float const v = static_cast<float>(y) / (preview_resolution - 1);
      float const coverage = placementCoverage(u, v, .75f, 1.f, preview_mode)
          * (exact_height ? exactFeatureWeight(u, v) : 1.f);
      glm::vec2 const sample_uv = _shape == MapStampShape::Painted
          ? clampToPaintedBoundary({u, v}) : glm::vec2{u, v};
      maximum = std::max(maximum,
          std::abs(sampleHeight(sample_uv.x, sample_uv.y, preview_mode)) * coverage);
    }
  maximum = std::max(maximum, .001f);
  for (int y = 0; y < preview_resolution; ++y)
    for (int x = 0; x < preview_resolution; ++x)
    {
      float const u = static_cast<float>(x) / (preview_resolution - 1);
      float const v = static_cast<float>(y) / (preview_resolution - 1);
      float const nx = u * 2.f - 1.f;
      float const nz = v * 2.f - 1.f;
      float const coverage = placementCoverage(u, v, .75f, 1.f, preview_mode)
          * (exact_height ? exactFeatureWeight(u, v) : 1.f);
      glm::vec2 const sample_uv = _shape == MapStampShape::Painted
          ? clampToPaintedBoundary({u, v}) : glm::vec2{u, v};
      float const normalized = std::clamp(.5f + sampleHeight(
          sample_uv.x, sample_uv.y, preview_mode)
          / (2.f * maximum), 0.f, 1.f);
      float const distance = shapeDistance(nx, nz, _shape);
      float const outline_width = 2.5f / (preview_resolution - 1);
      float const outline = _shape == MapStampShape::Painted
          ? 1.f - std::clamp(std::abs(paintedFootprintWeight(u, v) - .5f) * 2.f, 0.f, 1.f)
          : 1.f - std::clamp(std::abs(distance - 1.f) / outline_width, 0.f, 1.f);
      float const relief_tint = (.14f + normalized * .18f) * coverage;
      int const value = static_cast<int>(std::round(
          std::max(relief_tint, outline * .65f) * 255.f));
      image.setPixelColor(x, y, QColor(value, value, value, 255));
    }
  return image;
}
