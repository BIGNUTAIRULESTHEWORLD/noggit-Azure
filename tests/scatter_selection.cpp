#include "../src/noggit/tools/ScatterSelection.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>

using Noggit::ScatterSelection;
void require(bool condition, char const* message)
{
  if (!condition) throw std::runtime_error(message);
}
bool selected(ScatterSelection const& selection, int x, int z)
{
  auto page = selection.page(x / 512, z / 512);
  return page && page->pixels[(z % 512)*512 + x % 512];
}

int main()
{
  try
  {
    ScatterSelection selection;
    // Squares use the same radius (half-width) as the cursor, including corners
    // across tile seams. Erasing must use the selected shape as well.
    selection.paint(512, 512, 30, false, ScatterSelection::Shape::Square);
    require(selection.size() == 3600, "Square brush has the wrong side length");
    for (int z = 480; z < 544; ++z)
      for (int x = 480; x < 544; ++x)
        require(selected(selection, x, z) == (x >= 482 && x < 542 && z >= 482 && z < 542),
          "Square brush does not match its footprint across tile seams");
    selection.paint(512, 512, 15, true, ScatterSelection::Shape::Square);
    require(selection.size() == 2700 && !selected(selection, 497, 497) && selected(selection, 482, 482),
      "Square erase did not preserve the surrounding selection");
    selection.paint(512, 512, 30, true, ScatterSelection::Shape::Square);
    require(selection.empty(), "Square erase did not clear the full brush footprint");
    require(!selection.paint(512, 512, 30, true) && selection.empty(), "Erasing empty ground changed the mask");
    selection.paint(512, 512, 30, false);
    require(selection.page(0, 0) && selection.page(1, 0) && selection.page(0, 1) && selection.page(1, 1),
      "Brush did not cross all four ADT corners");
    for (int z = 480; z < 544; ++z)
      for (int x = 480; x < 544; ++x)
      {
        float dx = x+0.5f-512, dz = z+0.5f-512;
        require(selected(selection, x, z) == (dx*dx+dz*dz <= 900), "Solid circle has missing or extra pixels");
      }
    auto const count = selection.size();
    auto const revision = selection.page(0, 0)->revision;
    require(!selection.paint(512, 512, 30, false) && selection.size() == count
      && selection.page(0, 0)->revision == revision, "Unchanged painting forced a texture upload");
    selection.paint(2000, 2000, 50, false);
    require(selection.page(0, 0)->revision == revision, "Distant brush invalidated old terrain");
    selection.paint(512, 512, 15, true);
    require(!selected(selection, 511, 511) && selected(selection, 533, 511), "Erase did not preserve the surrounding ring");
    auto oldRevision = selection.page(0, 0)->revision;
    selection.paint(512, 512, 35, true);
    require(!selection.page(0, 0), "Empty page was not released");
    selection.paint(512, 512, 30, false);
    require(selection.page(0, 0)->revision > oldRevision, "Repaint reused a stale GPU revision");
    auto const clearRevision = selection.clearRevision();
    selection.clear();
    require(selection.empty() && !selection.page(0, 0) && selection.clearRevision() != clearRevision,
      "Clear did not invalidate cached masks");

    auto start = std::chrono::steady_clock::now();
    for (int z = 0; z < 20; ++z)
      for (int x = 0; x < 20; ++x)
        selection.paint(200.0f+x*80, 200.0f+z*80, 96, false);
    auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    require(selection.size() > 1000000, "Selection stopped growing past the previous 32,768-cell cap");
    auto largeCount = selection.size();
    require(selection.paint(5000, 5000, 96, false) && selection.size() > largeCount,
      "Painting stopped after a large selection");
    require(selection.cells().size() == selection.size(), "Placement mask and displayed mask differ");

    selection.clear();
    selection.paint(0, 0, 5, false);
    selection.paint(32768, 32768, 5, false);
    for (auto const& cell : selection.cells())
      require(cell.first >= 0 && cell.second >= 0 && cell.first < 32768 && cell.second < 32768,
        "Brush escaped map bounds");
    std::cout << "Scatter selection: circle/square fill and erase, ADT seams, revision caching, clear, map bounds, and >1 million cells passed.\n"
              << "400 large brush dabs: " << elapsed << " ms total (" << elapsed/400 << " ms/dab).\n";
    return 0;
  }
  catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
