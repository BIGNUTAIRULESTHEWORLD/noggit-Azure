#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Noggit
{
  // Sparse, tile-sized masks. Painting touches only brush pixels, never the
  // already selected terrain. Heights are unnecessary for a terrain-shader mask.
  class ScatterSelection
  {
  public:
    std::string texture;
    float coverage = 60.0f;
    enum class Shape { Circle, Square };
    static constexpr int resolution = 512;
    static constexpr int mapCells = 64 * resolution;
    using Cell = std::pair<int, int>; // z, x, also used by the scatter sampler
    struct Page
    {
      std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(resolution * resolution, 0);
      std::size_t count = 0;
      std::uint64_t revision = 0;
    };

    Page const* page(int tileX, int tileZ) const
    {
      auto found = _pages.find({tileZ, tileX});
      return found == _pages.end() ? nullptr : &found->second;
    }
    std::size_t size() const { return _count; }
    bool empty() const { return !_count; }
    std::uint64_t clearRevision() const { return _clearRevision; }
    void clear()
    {
      _pages.clear();
      _count = 0;
      ++_clearRevision;
    }

    // Coordinates and radius are in mask cells. Clip only to the actual map
    // extent; there is deliberately no selected-area or cell-count ceiling.
    bool paint(float x, float z, float radius, bool erase, Shape shape = Shape::Circle)
    {
      if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius) || radius <= 0) return false;
      int const minZ = static_cast<int>(std::clamp(std::ceil(z-radius-0.5f), 0.0f, static_cast<float>(mapCells)));
      int const maxZ = static_cast<int>(std::clamp(std::floor(z+radius-0.5f), -1.0f, static_cast<float>(mapCells-1)));
      bool changed = false;
      for (int row = minZ; row <= maxZ; ++row)
      {
        float const dz = row + 0.5f - z;
        float const halfWidth = shape == Shape::Square ? radius
            : std::sqrt(std::max(0.0f, radius*radius - dz*dz));
        int const minX = static_cast<int>(std::clamp(std::ceil(x-halfWidth-0.5f), 0.0f, static_cast<float>(mapCells)));
        int const maxX = static_cast<int>(std::clamp(std::floor(x+halfWidth-0.5f), -1.0f, static_cast<float>(mapCells-1)));
        for (int first = minX; first <= maxX;)
        {
          int const tileX = first / resolution, tileZ = row / resolution;
          int const end = std::min(maxX + 1, (tileX+1)*resolution);
          Cell const key{tileZ, tileX};
          auto found = _pages.find(key);
          if (found == _pages.end())
          {
            if (erase) { first = end; continue; }
            found = _pages.try_emplace(key).first;
          }
          auto& data = found->second;
          bool pageChanged = false;
          for (int column = first; column < end; ++column)
          {
            auto& pixel = data.pixels[(row % resolution)*resolution + column % resolution];
            std::uint8_t const value = erase ? 0 : 255;
            if (pixel == value) continue;
            pixel = value;
            if (erase) { --data.count; --_count; }
            else { ++data.count; ++_count; }
            pageChanged = true;
          }
          if (pageChanged) { data.revision = ++_revision; changed = true; }
          if (!data.count) _pages.erase(found);
          first = end;
        }
      }
      return changed;
    }

    std::vector<Cell> cells() const
    {
      std::vector<Cell> result;
      result.reserve(_count);
      for (auto const& [tile, data] : _pages)
        for (int z = 0; z < resolution; ++z)
          for (int x = 0; x < resolution; ++x)
            if (data.pixels[z*resolution + x])
              result.emplace_back(tile.first*resolution+z, tile.second*resolution+x);
      return result;
    }

  private:
    std::map<Cell, Page> _pages;
    std::size_t _count = 0;
    std::uint64_t _revision = 0;
    std::uint64_t _clearRevision = 0;
  };
}
