#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace Noggit::TextureScatterSampling
{
  using Cell = std::pair<int, int>; // z, x
  struct Settings
  {
    float cellSize;
    double density; // candidates per 100 square world units
    float spacing;
    float scaleMin;
    float scaleMax;
    bool randomYaw;
    unsigned seed;
    std::size_t limit;
    std::vector<double> weights;
  };
  struct Placement
  {
    float x, y, z, scale, yaw;
    std::size_t model;
  };

  inline bool matchesTexture(float target, float strongest, double percent)
  {
    // Express the comparison in percent to include fully opaque texels at 100%.
    return target * 100.0 / 255.0 >= percent && target >= strongest;
  }

  // Resumable sampling keeps terrain access on the UI thread. Cell-local
  // randomness prevents unrelated painted regions from rerolling candidates.
  class Job
  {
  public:
    Job(std::vector<Cell> cells, Settings settings)
      : _cells(std::move(cells)), _settings(std::move(settings)),
        _choose(_settings.weights.begin(), _settings.weights.end())
    {
      // Stable pseudorandom cell priority spreads a capped batch over the
      // entire selection without rerolling when unrelated cells are added.
      auto priority = [seed = _settings.seed](Cell cell) {
        std::uint64_t v = (std::uint64_t(static_cast<unsigned>(cell.first)) << 32)
          | static_cast<unsigned>(cell.second);
        v ^= seed;
        v += 0x9e3779b97f4a7c15ULL;
        v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
        v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
        return v ^ (v >> 31);
      };
      std::sort(_cells.begin(), _cells.end(), [&](Cell a, Cell b) {
        auto pa = priority(a), pb = priority(b);
        return pa != pb ? pa < pb : a < b;
      });
      if (_settings.weights.empty() || !_settings.limit || _settings.spacing <= 0
          || _settings.cellSize <= 0 || _settings.density <= 0
          || _settings.scaleMin > _settings.scaleMax) _cells.clear();
    }
    bool done() const { return (_cell == _cells.size() && !_attempts) || _result.size() >= _settings.limit; }
    std::vector<Placement> const& result() const { return _result; }
    template<class Sample>
    void step(Sample sample)
    {
      if (done()) return;
      if (!_attempts)
      {
        _current = _cells[_cell++];
        std::seed_seq seed{_settings.seed, static_cast<unsigned>(_current.first), static_cast<unsigned>(_current.second)};
        _positions.seed(seed);
        std::seed_seq appearanceSeed{_settings.seed ^ 0x9e3779b9u,
          static_cast<unsigned>(_current.first), static_cast<unsigned>(_current.second)};
        _appearance.seed(appearanceSeed);
        std::poisson_distribution<int> count(_settings.density * _settings.cellSize * _settings.cellSize / 100.0);
        _attempts = count(_positions);
        if (!_attempts) return;
      }
      --_attempts;
      float const x = (_current.second + _unit(_positions)) * _settings.cellSize;
      float const z = (_current.first + _unit(_positions)) * _settings.cellSize;
      // Consume appearance even for rejected candidates.
      float const scale = _settings.scaleMin + _unit(_appearance) * (_settings.scaleMax - _settings.scaleMin);
      float const yaw = _unit(_appearance) * 360.0f;
      auto const model = _choose(_appearance);
      Cell const bin{static_cast<int>(std::floor(z / _settings.spacing)), static_cast<int>(std::floor(x / _settings.spacing))};
      for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
          auto found = _nearby.find({bin.first + dz, bin.second + dx});
          if (found == _nearby.end()) continue;
          for (auto index : found->second)
          {
            auto const& point = _result[index];
            if ((point.x-x)*(point.x-x) + (point.z-z)*(point.z-z) < _settings.spacing*_settings.spacing) return;
          }
        }
      auto height = sample(x, z);
      if (!height) return;
      _nearby[bin].push_back(_result.size());
      _result.push_back({x, *height, z, scale, _settings.randomYaw ? yaw : 0.0f, model});
    }
  private:
    std::vector<Cell> _cells;
    Settings _settings;
    std::size_t _cell = 0;
    Cell _current{};
    int _attempts = 0;
    std::mt19937 _positions, _appearance;
    std::uniform_real_distribution<float> _unit{0, 1};
    std::discrete_distribution<std::size_t> _choose;
    std::map<Cell, std::vector<std::size_t>> _nearby;
    std::vector<Placement> _result;
  };
  template<class Sample>
  std::vector<Placement> generate(std::vector<Cell> cells, Settings const& settings, Sample sample)
  {
    Job job(std::move(cells), settings);
    while (!job.done()) job.step(sample);
    return job.result();
  }
}
