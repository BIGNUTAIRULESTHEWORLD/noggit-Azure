// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/texture_set.hpp>

#include <QImage>
#include <QString>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class World;
class MapChunk;

namespace Noggit::Ui::Tools::Stamp
{
  struct MapStampGridCellHash
  {
    std::size_t operator()(std::pair<int, int> const& cell) const noexcept
    {
      std::uint64_t const packed =
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.first)) << 32)
          | static_cast<std::uint32_t>(cell.second);
      return std::hash<std::uint64_t>{}(packed);
    }
  };

  using MapStampPaintedCells =
      std::unordered_map<std::pair<int, int>, float, MapStampGridCellHash>;

  enum class MapStampShape : std::uint8_t
  {
    Circle,
    Square,
    Painted
  };

  struct MapStampPaintedSelection
  {
    float grid_step = 0.f;
    MapStampPaintedCells cells;
  };

  enum class MapStampHeightMode : std::uint8_t
  {
    ExactFeature,
    MountainBlend,
    ConformToTerrain
  };

  struct MapStampTexture
  {
    std::string filename;
    layer_info layer;
    std::vector<std::uint8_t> weights;
  };

  struct MapStampProtectionSettings
  {
    bool automatic = false;
    float slope_start_degrees = 32.f;
    float slope_full_degrees = 48.f;
    float relief_start = 18.f;
    float relief_full = 35.f;
    std::function<std::optional<float>(float, float)> manual_at;
  };

  class MapStampAsset
  {
  public:
    bool capture(World* world, glm::vec3 const& center, float radius,
                 MapStampShape shape = MapStampShape::Circle, QString* error = nullptr);
    bool capture(World* world, MapStampPaintedSelection const& selection,
                 QString* error = nullptr);
    bool apply(World* world, glm::vec3 const& center, float radius, float rotation_degrees,
               float hardness, float height_scale, float height_offset, float opacity,
               MapStampHeightMode height_mode = MapStampHeightMode::ExactFeature);
    bool apply(World* world, glm::vec3 const& center, float radius, float rotation_degrees,
               float hardness, float height_scale, float height_offset, float opacity,
               MapStampProtectionSettings const& protection, MapStampHeightMode height_mode);
    bool previewTerrain(World* world, glm::vec3 const& center, float radius,
                         float rotation_degrees, float hardness, float height_scale,
                         float height_offset, float opacity,
                         MapStampProtectionSettings const& protection, MapStampHeightMode height_mode,
                         bool update_textures,
                         std::vector<MapChunk*>& preview_chunks,
                         std::vector<std::vector<glm::vec3>>& preview_lines) const;
    bool save(QString const& path, QString* error = nullptr) const;
    bool load(QString const& path, QString* error = nullptr);

    [[nodiscard]] bool valid() const;
    [[nodiscard]] float sourceRadius() const;
    [[nodiscard]] int heightResolution() const;
    [[nodiscard]] int textureResolution() const;
    [[nodiscard]] MapStampShape shape() const;
    [[nodiscard]] std::size_t textureCount() const;
    [[nodiscard]] bool supportsExactHeight() const;
    [[nodiscard]] QImage previewImage() const;
    [[nodiscard]] float footprintBoundingRadius(float radius, float rotation_degrees,
                                                float hardness,
                                                MapStampHeightMode height_mode) const;

  private:
    bool visitTerrainPlacement(
        World* world, glm::vec3 const& center, float radius, float rotation_degrees,
        float hardness, float height_scale, float height_offset, float opacity,
        MapStampProtectionSettings const& protection, MapStampHeightMode height_mode,
        bool mark_tiles_changed,
        std::function<void(MapChunk*, std::size_t, float)> const& visitor) const;
    float sampleHeight(float u, float v, MapStampHeightMode height_mode) const;
    float sampleTexture(std::size_t layer, float u, float v) const;
    float exactFeatureWeight(float u, float v) const;
    float paintedFootprintWeight(float u, float v) const;
    float paintedOutsideDistance(float u, float v) const;
    glm::vec2 clampToPaintedBoundary(glm::vec2 const& uv) const;
    float placementCoverage(float u, float v, float hardness, float opacity,
                            MapStampHeightMode height_mode) const;
    void rebuildPaintedFootprintData();
    void rebuildExactFeatureMask();
    std::optional<float> exactSourceBaseHeight() const;

    float _source_radius = 0.f;
    int _height_resolution = 0;
    int _texture_resolution = 0;
    MapStampShape _shape = MapStampShape::Circle;
    bool _height_is_relief = false;
    float _sample_extent = 1.f;
    std::vector<float> _relative_heights;
    std::vector<float> _relief_heights;
    std::vector<std::uint8_t> _painted_footprint_mask;
    std::vector<float> _painted_outside_distances;
    std::vector<std::int32_t> _painted_nearest_indices;
    std::vector<float> _exact_feature_mask;
    mutable bool _exact_source_base_height_cached = false;
    mutable std::optional<float> _exact_source_base_height;
    std::vector<MapStampTexture> _textures;
  };
}
