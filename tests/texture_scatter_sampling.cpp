#include "../src/noggit/tools/TextureScatterSampling.hpp"
#include <iostream>
#include <stdexcept>

using namespace Noggit::TextureScatterSampling;

void require(bool pass, char const* message)
{
  if (!pass) throw std::runtime_error(message);
}

int main()
{
  try
  {
    require(matchesTexture(255, 255, 100), "Opaque texture must pass 100% coverage");
    require(!matchesTexture(100, 155, 20), "Supporting dirt must not pass dominance");
    require(!matchesTexture(150, 150, 60), "Dominance alone must not bypass coverage");
    require(!matchesTexture(0, 255, 1), "Absent textures must never pass");
    std::vector<Cell> cells;
    for (int z = -10; z < 10; ++z)
      for (int x = -10; x < 10; ++x) cells.emplace_back(z, x);
    Settings settings{2, 150, 0.6f, 0.8f, 1.2f, true, 17, 5000, {1, 3}};
    auto mask = [](float x, float z) -> std::optional<float> {
      // Dirt on the right half, plus a hole through the left half.
      if (x >= 0 || (z > -2 && z < 2)) return std::nullopt;
      return 2*x + 0.5f*z;
    };
    auto points = generate(cells, settings, mask);
    require(points.size() > 100, "Expected a populated eligible area");
    std::size_t heavier = 0;
    for (std::size_t i = 0; i < points.size(); ++i)
    {
      auto const& p = points[i];
      require(p.x >= -20 && p.x < 0 && p.z >= -20 && p.z < 20, "Point outside selected eligible ground");
      require(p.z <= -2 || p.z >= 2, "Point placed in a masked hole");
      require(p.y == 2*p.x + 0.5f*p.z, "Placement did not preserve terrain height");
      require(p.scale >= 0.8f && p.scale <= 1.2f && p.yaw >= 0 && p.yaw < 360, "Transform outside configured range");
      require(p.model < 2, "Invalid model index");
      heavier += p.model == 1;
      for (std::size_t j = 0; j < i; ++j)
      {
        float const dx = p.x-points[j].x, dz = p.z-points[j].z;
        require(dx*dx + dz*dz >= settings.spacing*settings.spacing, "Minimum spacing violated across cell boundaries");
      }
    }
    require(heavier > points.size()/2, "Weighted model selection was not respected");
    auto repeated = generate(cells, settings, mask);
    require(repeated.size() == points.size(), "Same seed changed object count");
    for (std::size_t i = 0; i < points.size(); ++i)
      require(points[i].x == repeated[i].x && points[i].z == repeated[i].z
        && points[i].scale == repeated[i].scale && points[i].yaw == repeated[i].yaw
        && points[i].model == repeated[i].model, "Same seed changed preview");
    Job sliced(cells, settings);
    while (!sliced.done()) sliced.step(mask);
    require(sliced.result().size() == points.size(), "Incremental sampling changed count");
    for (std::size_t i = 0; i < points.size(); ++i)
      require(sliced.result()[i].x == points[i].x && sliced.result()[i].z == points[i].z,
        "Incremental sampling changed positions");
    auto extendedCells = cells;
    extendedCells.emplace_back(-100, -100);
    extendedCells.emplace_back(100, 100);
    auto extended = generate(extendedCells, settings, mask);
    for (auto const& p : points)
    {
      auto found = std::find_if(extended.begin(), extended.end(), [&](auto const& q) {
        return p.x == q.x && p.z == q.z && p.scale == q.scale && p.yaw == q.yaw && p.model == q.model;
      });
      require(found != extended.end(), "Distant painting rerolled existing previews");
    }
    std::reverse(extendedCells.begin(), extendedCells.end());
    auto reordered = generate(extendedCells, settings, mask);
    require(reordered.size() == extended.size(), "Input order changed count");
    for (std::size_t i = 0; i < extended.size(); ++i)
      require(reordered[i].x == extended[i].x && reordered[i].z == extended[i].z,
        "Input order changed preview");
    settings.scaleMin = settings.scaleMax = 2;
    settings.randomYaw = false;
    auto transformed = generate(cells, settings, mask);
    require(transformed.size() == points.size(), "Appearance changed placement count");
    for (std::size_t i = 0; i < points.size(); ++i)
      require(points[i].x == transformed[i].x && points[i].z == transformed[i].z
        && transformed[i].scale == 2 && transformed[i].yaw == 0, "Appearance changed locations or ignored transform settings");
    ++settings.seed;
    auto rerolled = generate(cells, settings, mask);
    require(!rerolled.empty() && (rerolled[0].x != points[0].x || rerolled[0].z != points[0].z), "Reroll did not change positions");
    settings.limit = 37;
    require(generate(cells, settings, mask).size() == 37, "Batch limit was not enforced");
    require(generate(cells, settings, [](float, float) -> std::optional<float> { return std::nullopt; }).empty(),
      "Completely ineligible terrain generated placements");
    require(generate(std::vector<Cell>{}, settings, mask).empty(), "Empty selection generated placements");
    std::vector<Cell> circles;
    for (int center : {100, 200, 300})
      for (int z = center-15; z <= center+15; ++z)
        for (int x = 85; x <= 115; ++x)
          if ((z-center)*(z-center)+(x-100)*(x-100) <= 225) circles.emplace_back(z, x);
    Settings cappedSettings{1, 1000, 0.1f, 1, 1, false, 17, 5000, {1}};
    auto capped = generate(circles, cappedSettings, [](float, float) -> std::optional<float> { return 0.0f; });
    int populations[3]{};
    for (auto const& p : capped) ++populations[p.z < 150 ? 0 : p.z < 250 ? 1 : 2];
    require(capped.size() == 5000, "Distributed batch exceeded or missed cap");
    for (auto n : populations) require(n > 1200 && n < 2200, "Capped batch starved an identical circle");
    std::cout << "Capped circle populations: " << populations[0] << ", " << populations[1] << ", " << populations[2] << '\n';
    // Low spacing makes rejection negligible: check density against eligible area,
    // not the full selection (half the terrain is eligible).
    settings = {2, 10, 0.001f, 1, 1, false, 71, 5000, {1}};
    auto half = [](float x, float) -> std::optional<float> { return x < 0 ? std::optional<float>(0.0f) : std::nullopt; };
    std::size_t total = 0;
    for (unsigned seed = 0; seed < 40; ++seed)
    { settings.seed = seed; total += generate(cells, settings, half).size(); }
    require(total > 2800 && total < 3600, "Density was not based on eligible area (expected mean 80)");
    std::cout << "Texture scatter: texture dominance/coverage, area and holes, ground height, spacing, weights, seeds, transforms, cap, and density passed.\n";
    return 0;
  }
  catch (std::exception const& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
