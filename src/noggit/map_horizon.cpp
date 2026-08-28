// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "map_horizon.h"

#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Log.h>
#include <noggit/map_index.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/World.h>

#include <opengl/context.hpp>
#include <opengl/context.inl>
#include <noggit/Misc.h>

#include <bitset>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

#include <QSettings>

struct color
{
  color(unsigned char r, unsigned char g, unsigned char b)
    : _r(r)
    , _g(g)
    , _b(b)
  {}

  uint32_t to_int() const {
    return (_b) | (_g << 8) | (_r << 16) | (uint32_t)(0xFFu << 24);
  }

  operator uint32_t () const {
    return to_int();
  }

  unsigned char _r;
  unsigned char _g;
  unsigned char _b;
};

struct ranged_color
{
  ranged_color (const color& c, const int16_t& start, const int16_t& stop)
    : _color (c)
    , _start (start)
    , _stop (stop)
  {}

  const color   _color;
  const int16_t _start;
  const int16_t _stop;
};

static inline color lerp_color(const color& start, const color& end, float t)
{
  return color ( (end._r) * t + (start._r) * (1.0 - t)
               , (end._g) * t + (start._g) * (1.0 - t)
               , (end._b) * t + (start._b) * (1.0 - t)
               );
}

static inline float smooth_step(float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline float hash_noise(int x, int y, uint32_t seed)
{
  uint32_t value = static_cast<uint32_t>(x) * 0x8da6b343u;
  value ^= static_cast<uint32_t>(y) * 0xd8163841u;
  value ^= seed * 0xcb1ab31fu;
  value ^= value >> 13;
  value *= 0x85ebca6bu;
  value ^= value >> 16;
  return static_cast<float>(value & 0xffffu) / 32767.5f - 1.0f;
}

class noise_lattice
{
public:
  noise_lattice(float scale, uint32_t seed)
    : _scale(scale)
    , _size(static_cast<int>(std::ceil((64.0f * 64.0f) / scale)) + 2)
    , _values(static_cast<std::size_t>(_size) * _size)
  {
    for (int y = 0; y < _size; ++y)
      for (int x = 0; x < _size; ++x)
        _values[static_cast<std::size_t>(y) * _size + x] = hash_noise(x, y, seed);
  }

  float sample(float pixel_x, float pixel_y) const
  {
    float const x = pixel_x / _scale;
    float const y = pixel_y / _scale;
    int const x0 = std::clamp(static_cast<int>(std::floor(x)), 0, _size - 2);
    int const y0 = std::clamp(static_cast<int>(std::floor(y)), 0, _size - 2);
    float const tx = smooth_step(x - static_cast<float>(x0));
    float const ty = smooth_step(y - static_cast<float>(y0));
    auto at = [this](int px, int py)
    {
      return _values[static_cast<std::size_t>(py) * _size + px];
    };

    float const top = std::lerp(at(x0, y0), at(x0 + 1, y0), tx);
    float const bottom = std::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx);
    return std::lerp(top, bottom, ty);
  }

private:
  float _scale;
  int _size;
  std::vector<float> _values;
};

static float terrain_texture(float x, float y)
{
  static const noise_lattice broad(30.0f, 17u);
  static const noise_lattice medium(12.0f, 43u);
  static const noise_lattice fine(4.5f, 91u);
  return broad.sample(x, y) * 0.52f
       + medium.sample(x, y) * 0.31f
       + fine.sample(x, y) * 0.17f;
}

static color to_color(QColor const& value)
{
  return color(static_cast<unsigned char>(value.red()),
               static_cast<unsigned char>(value.green()),
               static_cast<unsigned char>(value.blue()));
}

static color detailed_color_for_height(float height, float lighting, float texture,
                                       Noggit::map_horizon_minimap_palette const& palette)
{
  // Closely spaced stops around sea level create the pale shoreline seen on
  // authored maps. Higher bands deliberately overlap through interpolation so
  // hills do not form the hard rings produced by the old four-colour ramp.
  // Let broad texture variation move land between neighbouring elevation
  // bands. This breaks up uniform green fields and gives mountain/snow edges
  // the irregular authored-map character of the reference image. Keep water
  // tied to its real height so coastlines do not drift.
  float const classified_height = height > palette.stops[4].height ? height + texture * 85.0f : height;
  color base = to_color(palette.stops.back().color);
  for (size_t i = 0; i + 1 < palette.stops.size(); ++i)
  {
    if (classified_height <= palette.stops[i + 1].height)
    {
      float const t = smooth_step((classified_height - palette.stops[i].height)
                                / static_cast<float>(palette.stops[i + 1].height - palette.stops[i].height));
      base = lerp_color(to_color(palette.stops[i].color), to_color(palette.stops[i + 1].color), t);
      break;
    }
  }

  float const texture_strength = height < palette.stops[3].height
                               ? 5.0f
                               : (classified_height > palette.stops[12].height ? 8.0f : 19.0f);
  float const brightness = std::clamp(lighting + texture * texture_strength, -42.0f, 42.0f);
  auto channel = [brightness](unsigned char value)
  {
    return static_cast<unsigned char>(std::clamp(static_cast<float>(value) + brightness, 0.0f, 255.0f));
  };
  return color(channel(base._r), channel(base._g), channel(base._b));
}

static inline uint32_t color_for_height (int16_t height)
{
  static const ranged_color colors[] =
    { ranged_color (color (20, 149, 7), 0, 600)
    , ranged_color (color (137, 84, 21), 600, 1200)
    , ranged_color (color (96, 96, 96), 1200, 1600)
    , ranged_color (color (255, 255, 255), 1600, 0x7FFF)
    };
  static const size_t num_colors (sizeof (colors) / sizeof (ranged_color));

  if (height < colors[0]._start)
  {
    return color (0, 0, 255 + std::max (height / 2.0, -255.0));
  }
  else if (height >= colors[num_colors - 1]._stop)
  {
    return colors[num_colors - 1]._color;
  }

  float t (1.0);
  size_t correct_color (num_colors - 1);

  for (size_t i (0); i < num_colors - 1; ++i)
  {
    if (height >= colors[i]._start && height < colors[i]._stop)
    {
      t = float(height - colors[i]._start) / float (colors[i]._stop - colors[i]._start);
      correct_color = i;
      break;
    }
  }

  return lerp_color(colors[correct_color]._color, colors[correct_color + 1]._color, t);
}
namespace Noggit
{

map_horizon_minimap_palette default_map_horizon_minimap_palette()
{
  map_horizon_minimap_palette palette;
  palette.stops = {{
    { -400, QColor(7, 22, 37) },
    { -100, QColor(12, 42, 66) },
    {  -35, QColor(47, 105, 142) },
    {   -6, QColor(121, 177, 191) },
    {    8, QColor(53, 103, 56) },
    {   90, QColor(63, 118, 60) },
    {  220, QColor(82, 134, 65) },
    {  350, QColor(102, 143, 71) },
    {  455, QColor(143, 138, 78) },
    {  565, QColor(165, 135, 81) },
    {  650, QColor(151, 108, 73) },
    {  700, QColor(132, 94, 68) },
    {  735, QColor(190, 177, 155) },
    {  800, QColor(239, 236, 224) },
    {  980, QColor(250, 249, 242) }
  }};
  return palette;
}

map_horizon_minimap_palette load_map_horizon_minimap_palette()
{
  auto palette = default_map_horizon_minimap_palette();
  QSettings settings;
  for (std::size_t i = 0; i < palette.stops.size(); ++i)
  {
    QString const prefix = QStringLiteral("mainMenu/heightmap/stop%1/").arg(i);
    palette.stops[i].height = settings.value(prefix + QStringLiteral("height"),
                                              palette.stops[i].height).toInt();
    QColor const stored(settings.value(prefix + QStringLiteral("color"),
                                       palette.stops[i].color.name(QColor::HexRgb)).toString());
    if (stored.isValid())
      palette.stops[i].color = stored;
  }

  // Settings can be hand-edited. Preserve a valid interpolation ramp even if
  // persisted values arrive out of order.
  for (std::size_t i = 1; i < palette.stops.size(); ++i)
    palette.stops[i].height = std::max(palette.stops[i].height,
                                       palette.stops[i - 1].height + 1);
  return palette;
}

void save_map_horizon_minimap_palette(map_horizon_minimap_palette const& palette)
{
  QSettings settings;
  for (std::size_t i = 0; i < palette.stops.size(); ++i)
  {
    QString const prefix = QStringLiteral("mainMenu/heightmap/stop%1/").arg(i);
    settings.setValue(prefix + QStringLiteral("height"), palette.stops[i].height);
    settings.setValue(prefix + QStringLiteral("color"), palette.stops[i].color.name(QColor::HexRgb));
  }
}

map_horizon::map_horizon(const std::string& basename, World * const world)
  : _minimap_palette(load_map_horizon_minimap_palette())
{
  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << ".wdl";
  _filename = filename.str();

  if (!Application::NoggitApplication::instance()->clientData()->exists(_filename))
  {
    LogError << "file \"World\\Maps\\" << basename << "\\" << basename << ".wdl\" does not exist." << std::endl;
    return;
  }

  BlizzardArchive::ClientFile wdl_file (_filename, Application::NoggitApplication::instance()->clientData());

  uint32_t fourcc;
  uint32_t size;

  bool done = false;

  do
  {
    wdl_file.read(&fourcc, 4);
    wdl_file.read(&size, 4);

    switch (fourcc)
    {
      case 'MVER':
      {
        uint32_t version;
        wdl_file.read(&version, 4);
        assert(size == 4 && version == 18);

        break;
      }

      case 'MWMO':
      {
        {
          // TODO : use WMID instead for proper string parsing.

            char const* lCurPos = reinterpret_cast<char const*>(wdl_file.getPointer());
            char const* lEnd = lCurPos + size;
        
            while (lCurPos < lEnd)
            {
                mWMOFilenames.push_back(BlizzardArchive::ClientData::normalizeFilenameInternal(std::string(lCurPos)));
                lCurPos += strlen(lCurPos) + 1;
            }
        }
        wdl_file.seekRelative(size);
        break;
      }
      case 'MWID':
          // TODO
          wdl_file.seekRelative(size); // jump to end of chunk
          break;
      case 'MODF':
      {
        ENTRY_MODF const* modf_ptr = reinterpret_cast<ENTRY_MODF const*>(wdl_file.getPointer());
        for (unsigned int i = 0; i < size / sizeof(ENTRY_MODF); ++i)
        {
            lWMOInstances.push_back(modf_ptr[i]);
            if (lWMOInstances[i].scale == 0.0f)
              lWMOInstances[i].scale = 1024.0f;
        }
        
        wdl_file.seekRelative(size); // jump to end of chunk
        break;
      }
      case 'MAOF':
      {
        assert(size == 64 * 64 * sizeof(uint32_t));

        uint32_t mare_offsets[64][64];
        wdl_file.read(mare_offsets, 64 * 64 * sizeof(uint32_t));

        // - MARE and MAHO by offset ---------------------------
        for (size_t y(0); y < 64; ++y)
        {
          for (size_t x(0); x < 64; ++x)
          {
            if (!mare_offsets[y][x])
            {
              continue;
            }

            wdl_file.seek(mare_offsets[y][x]);
            wdl_file.read(&fourcc, 4);
            wdl_file.read(&size, 4);

            assert(fourcc == 'MARE');
            assert(size == 0x442);

            _tiles[y][x] = std::make_unique<map_horizon_tile>();

            //! \todo There also is MAHO giving holes into this heightmap.
            wdl_file.read(_tiles[y][x]->height_17, 17 * 17 * sizeof(int16_t));
            wdl_file.read(_tiles[y][x]->height_16, 16 * 16 * sizeof(int16_t));

            if (wdl_file.getPos() < wdl_file.getSize())
            {
                wdl_file.read(&fourcc, 4);
                if (fourcc == 'MAHO')
                {
                    wdl_file.read(&size, 4);
                    assert(size == 0x20);
                    wdl_file.read(_tiles[y][x]->holes, 16 * sizeof(int16_t));
                }
            }

          }
        }
        done = true;
        break;
      }
      default:
        LogError << "unknown chunk in wdl: code=" << fourcc << std::endl;
        wdl_file.seekRelative(size);
        break;
    }
  } while (!done && !wdl_file.isEof());

  constexpr bool _load_models = true;
  if (_load_models)
  {
    // - Load WMOs -----------------------------------------

    // Don't load them to storage, they share UIDs wth regular models

    // for rendering in unloaded tiles
    for (auto const& object : lWMOInstances)
    {
      // world->add_wmo_instance(WMOInstance(mWMOFilenames[object.nameID],
      //   &object, world->getRenderContext()), false, false);

      // auto& filepath = mWMOFilenames[object.nameID];
      // wmos.push_back(scoped_wmo_reference(filepath, world->getRenderContext()));
    }
  }

  wdl_file.close();

  set_minimap(&world->mapIndex);
}

void map_horizon::render_minimap_tile(int y, int x, bool has_data,
                                      uint32_t* pixels, int stride) const
{
    if (_tiles[y][x])
    {
        constexpr int resolution = minimap_pixels_per_tile;
        constexpr int cached_side = resolution + 2;

        auto sample_tile = [this](int tile_x, int tile_y, float local_x, float local_y)
        {
          tile_x = std::clamp(tile_x, 0, 63);
          tile_y = std::clamp(tile_y, 0, 63);
          auto const* tile = _tiles[tile_y][tile_x].get();
          if (!tile)
            return std::numeric_limits<float>::quiet_NaN();

          local_x = std::clamp(local_x, 0.0f, 16.0f);
          local_y = std::clamp(local_y, 0.0f, 16.0f);
          int const cell_x = std::min(static_cast<int>(local_x), 15);
          int const cell_y = std::min(static_cast<int>(local_y), 15);
          float const fx = local_x - static_cast<float>(cell_x);
          float const fy = local_y - static_cast<float>(cell_y);

          float const h00 = tile->height_17[cell_y][cell_x];
          float const h10 = tile->height_17[cell_y][cell_x + 1];
          float const h01 = tile->height_17[cell_y + 1][cell_x];
          float const h11 = tile->height_17[cell_y + 1][cell_x + 1];
          float const bilinear = std::lerp(std::lerp(h00, h10, fx), std::lerp(h01, h11, fx), fy);

          // The 16x16 array stores the centre vertex of each WDL cell. Blend
          // its deviation from the four corners with a smooth tent function.
          float const corner_centre = (h00 + h10 + h01 + h11) * 0.25f;
          float const centre_weight = (1.0f - std::abs(fx * 2.0f - 1.0f))
                                    * (1.0f - std::abs(fy * 2.0f - 1.0f));
          return bilinear + centre_weight * (tile->height_16[cell_y][cell_x] - corner_centre);
        };

        auto sample = [&](float local_x, float local_y)
        {
          float const original_x = local_x;
          float const original_y = local_y;
          int tile_x = x;
          int tile_y = y;
          while (local_x < 0.0f) { local_x += 16.0f; --tile_x; }
          while (local_y < 0.0f) { local_y += 16.0f; --tile_y; }
          while (local_x > 16.0f) { local_x -= 16.0f; ++tile_x; }
          while (local_y > 16.0f) { local_y -= 16.0f; ++tile_y; }

          float result = sample_tile(tile_x, tile_y, local_x, local_y);
          if (std::isnan(result))
            result = sample_tile(x, y, std::clamp(original_x, 0.0f, 16.0f),
                                       std::clamp(original_y, 0.0f, 16.0f));
          return result;
        };

        float const sample_step = 16.0f / static_cast<float>(resolution);
        std::array<float, cached_side * cached_side> heights;
        for (int cached_y = -1; cached_y <= resolution; ++cached_y)
        {
          for (int cached_x = -1; cached_x <= resolution; ++cached_x)
          {
            float const local_x = (static_cast<float>(cached_x) + 0.5f) * sample_step;
            float const local_y = (static_cast<float>(cached_y) + 0.5f) * sample_step;
            heights[static_cast<std::size_t>(cached_y + 1) * cached_side + cached_x + 1]
              = sample(local_x, local_y);
          }
        }

        for (int pixel_y = 0; pixel_y < resolution; ++pixel_y)
        {
          uint32_t* output = pixels + (y * resolution + pixel_y) * stride + x * resolution;
          for (int pixel_x = 0; pixel_x < resolution; ++pixel_x)
          {
            std::size_t const centre = static_cast<std::size_t>(pixel_y + 1) * cached_side + pixel_x + 1;
            float const height = heights[centre];
            float const dx = heights[centre + 1] - heights[centre - 1];
            float const dy = heights[centre + cached_side] - heights[centre - cached_side];

            // A north-west light gives the overview readable relief without
            // turning it into a literal grayscale height map.
            float const hill_light = std::clamp((-dx - dy) * 0.16f, -28.0f, 28.0f);
            float const global_x = static_cast<float>(x * resolution + pixel_x);
            float const global_y = static_cast<float>(y * resolution + pixel_y);
            output[pixel_x] = detailed_color_for_height(height, hill_light,
                                                        terrain_texture(global_x, global_y),
                                                        _minimap_palette);
          }
        }
    }
    // the adt exist but there's no data in the wdl
    else if (has_data)
    {
        constexpr int resolution = minimap_pixels_per_tile;
        for (int j(0); j < resolution; ++j)
        {
          uint32_t* output = pixels + (y * resolution + j) * stride + x * resolution;
          std::fill_n(output, resolution, color(122, 94, 67).to_int());
        }
    }
}

void map_horizon::update_minimap_tile(int y, int x, bool has_data)
{
  if (_qt_minimap.isNull())
    return;
  render_minimap_tile(y, x, has_data,
                      reinterpret_cast<uint32_t*>(_qt_minimap.bits()),
                      _qt_minimap.bytesPerLine() / static_cast<int>(sizeof(uint32_t)));
}

void map_horizon::set_minimap(const MapIndex* const index, bool set_empty)
{
    auto const render_started = std::chrono::steady_clock::now();
    _qt_minimap = QImage(minimap_pixels_per_tile * 64, minimap_pixels_per_tile * 64, QImage::Format_ARGB32);
    _qt_minimap.fill(Qt::transparent);

    if (set_empty)
        return;

    struct tile_job { int x; int y; bool has_data; };
    std::vector<tile_job> jobs;
    jobs.reserve(64 * 64);
    for (int y(0); y < 64; ++y)
    {
        for (int x(0); x < 64; ++x)
        {
          bool const has_data = index->hasTile(TileIndex(x, y));
          if (_tiles[y][x] || has_data)
            jobs.push_back({x, y, has_data});
        }
    }

    // Force construction of the shared immutable noise lattices before worker
    // threads begin, then render disjoint ADT rectangles directly into the
    // detached QImage buffer.
    (void)terrain_texture(0.0f, 0.0f);
    uint32_t* pixels = reinterpret_cast<uint32_t*>(_qt_minimap.bits());
    int const stride = _qt_minimap.bytesPerLine() / static_cast<int>(sizeof(uint32_t));
    unsigned int const worker_count = std::max(1u, std::min<unsigned int>(
      std::thread::hardware_concurrency(), static_cast<unsigned int>(jobs.size())));
    std::atomic_size_t next_job{0};
    auto worker = [this, &jobs, &next_job, pixels, stride]()
    {
      while (true)
      {
        std::size_t const job_index = next_job.fetch_add(1, std::memory_order_relaxed);
        if (job_index >= jobs.size())
          return;
        tile_job const& job = jobs[job_index];
        render_minimap_tile(job.y, job.x, job.has_data, pixels, stride);
      }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for (unsigned int i = 1; i < worker_count; ++i)
      workers.emplace_back(worker);
    worker();
    for (auto& thread : workers)
      thread.join();

    auto const render_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - render_started).count();
    LogDebug << "Rendered high-resolution WDL minimap (" << jobs.size() << " ADTs, "
             << worker_count << " workers) in " << render_ms << " ms." << std::endl;
}

void map_horizon::remove_horizon_tile(int y, int x)
{
    _tiles[y][x].reset();

    for (int j(0); j < minimap_pixels_per_tile; ++j)
    {
        for (int i(0); i < minimap_pixels_per_tile; ++i)
        {
            _qt_minimap.setPixel(x * minimap_pixels_per_tile + i,
                                 y * minimap_pixels_per_tile + j, color(255, 25, 25));
        }
    }
}

Noggit::map_horizon_tile* map_horizon::get_horizon_tile(int y, int x)
{
    return _tiles[y][x].get();
}

int16_t map_horizon::getWdlheight(MapTile* tile, float x, float y)
{
    int cx = std::min(std::max(static_cast<int>(x / CHUNKSIZE), 0), 15);
    int cy = std::min(std::max(static_cast<int>(y / CHUNKSIZE), 0), 15);

    x -= cx * CHUNKSIZE;
    y -= cy * CHUNKSIZE;

    int row = static_cast<int>(y / (UNITSIZE * 0.5f) + 0.5f);
    int col = static_cast<int>((x - UNITSIZE * 0.5f * (row % 2)) / UNITSIZE + 0.5f);
    bool inner = (row % 2) == 1;

    if (row < 0 || col < 0 || row > 16 || col >(inner ? 8 : 9))
        return 0;

    // truncate and clamp the float value
    auto chunk = tile->getChunk(cx, cy);
    // float height = heights[cy * 16 + cx][17 * (row / 2) + (inner ? 9 : 0) + col];
    if (!chunk)
        return 0.0f;

    float height = chunk->getHeightmap()[17 * (row / 2) + (inner ? 9 : 0) + col].y;
    return std::min(std::max(static_cast<int16_t>(height), static_cast<int16_t>(SHRT_MIN)), static_cast<int16_t>(SHRT_MAX));
}

void map_horizon::update_horizon_tile(MapTile* mTile)
{
    auto tile_index = mTile->index;

    // calculate the heightmap as a short array
    float x, y;
    for (int i = 0; i < 17; i++)
    {
        for (int j = 0; j < 17; j++)
        {
            // outer - correct
            x = j * CHUNKSIZE;
            y = i * CHUNKSIZE;

            if (!_tiles[tile_index.z][tile_index.x].get()) // tile has not been initialised
                //     continue;
            {
                _tiles[tile_index.z][tile_index.x] = std::make_unique<map_horizon_tile>();
                // do we need to use memcpy as well ?
            }
            // only works for initialised
            _tiles[tile_index.z][tile_index.x].get()->height_17[i][j] = getWdlheight(mTile, x, y);

            // inner - close enough; correct values appear to use some form of averaging
            if (i < 16 && j < 16)
                _tiles[tile_index.z][tile_index.x].get()->height_16[i][j] = getWdlheight(mTile, x + CHUNKSIZE / 2.0f, y + CHUNKSIZE / 2.0f);
        }
    }
    // Holes
    for (int i = 0; i < 16; ++i)
    {
        std::bitset<16>wdlHoleMask(0);

        for (int j = 0; j < 16; ++j)
        {
            auto chunk = mTile->getChunk(j, i);
            if (!chunk)
                continue;
            // the ordering seems to be : short array = Y axis, flags values = X axis and the values are for a whole chunk.

            std::bitset<16> holeBits(chunk->getHoleMask());

            if (holeBits.count() == 16) // if all holes are set in a chunk
                wdlHoleMask.set(j, true);
        }
        _tiles[tile_index.z][tile_index.x].get()->holes[i] = static_cast<int16_t>(wdlHoleMask.to_ulong());
    }

    update_minimap_tile(tile_index.z, tile_index.x, true);
}

void map_horizon::save_wdl(World* world, bool regenerate)
{
    world->wait_for_all_tile_updates();

    std::stringstream filename;
    filename << "World\\Maps\\" << world->basename << "\\" << world->basename << ".wdl";
    //Log << "Saving WDL \"" << filename << "\"." << std::endl;

    util::sExtendableArray wdlFile;
    int curPos = 0;

    // MVER
    //  {
    wdlFile.Extend(8 + 0x4);
    SetChunkHeader(wdlFile, curPos, 'MVER', 4);

    // MVER data
    *(wdlFile.GetPointer<int>(8)) = 18; // write version 18
    curPos += 8 + 0x4;
    //  }

    // WMO objects export code is copy pasta from MapTile

    struct filenameOffsetThing
    {
      int nameID;
      int filenamePosition;
    };

    filenameOffsetThing nullyThing = { 0, 0 };

    std::map<std::string, filenameOffsetThing> lObjects;

    // avoid duplicates, not really necessary here as we directly used MWMO string list
    for (auto const& filename : mWMOFilenames)
    {
      if (lObjects.find(filename) == lObjects.end())
      {
        lObjects.emplace(filename, nullyThing);
      }
    }

    int lID = 0;
    for (auto& object : lObjects)
    {
      object.second.nameID = lID++;
    }

    // MWMO
    //  {
    int lMWMO_Position = curPos;
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MWMO', 0);

    curPos += 8;

    // MWMO data
    for (auto& object : lObjects)
    {
      object.second.filenamePosition = wdlFile.GetPointer<sChunkHeader>(lMWMO_Position)->mSize;
      wdlFile.Insert(curPos, static_cast<unsigned long>(object.first.size() + 1), misc::normalize_adt_filename(object.first).c_str());
      curPos += static_cast<int>(object.first.size() + 1);
      wdlFile.GetPointer<sChunkHeader>(lMWMO_Position)->mSize += static_cast<int>(object.first.size() + 1);
      LogDebug << "Added WDL object \"" << object.first << "\"." << std::endl;
    }
    //  }

    // MWID
    //  {
    int lMWID_Size = static_cast<int>(4 * lObjects.size());
    wdlFile.Extend(8 + lMWID_Size);
    SetChunkHeader(wdlFile, curPos, 'MWID', lMWID_Size);

    // MWID data
    auto const lMWID_Data = wdlFile.GetPointer<int>(curPos + 8);

    lID = 0;
    for (auto const& object : lObjects)
      lMWID_Data[lID++] = object.second.filenamePosition;

    curPos += 8 + lMWID_Size;
    //  }

    // MODF
    //  {
    int lMODF_Size = static_cast<int>(0x40 * lWMOInstances.size());
    wdlFile.Extend(8 + lMODF_Size);
    SetChunkHeader(wdlFile, curPos, 'MODF', lMODF_Size);

    // MODF data
    auto const lMODF_Data = wdlFile.GetPointer<ENTRY_MODF>(curPos + 8);

    lID = 0;
    for (auto const& object : lWMOInstances)
    {
      auto filename_to_offset_and_name = lObjects.find(mWMOFilenames[object.nameID]);
      if (filename_to_offset_and_name == lObjects.end())
      {
        LogError << "There is a problem with saving the WDL objects. We have an object that somehow changed the name during the saving function." << std::endl;
        return;
      }

      lMODF_Data[lID] = object;
      // only need to update name id
      lMODF_Data[lID].nameID = filename_to_offset_and_name->second.nameID;
      lID++;
    }
    LogDebug << "Added " << lID << " wmos to WDL MODF" << std::endl;

    curPos += 8 + lMODF_Size;
    //  }

    //uint32_t mare_offsets[64][64] = { 0 };
    // MAOF
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MAOF', 64 * 64 * 4);
    curPos += 8;
    wdlFile.Extend(64 * 64 * 4);
    uint mareoffset = curPos + 64 * 64 * 4;

    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            TileIndex index(x, y);

            bool has_tile = world->mapIndex.hasTile(index);
            // write offset in MAOF entry
            *(wdlFile.GetPointer<uint>(curPos)) = has_tile ? mareoffset : 0;

            if (has_tile)
            {
                // MARE Header
                //  {
                wdlFile.Extend(8);
                SetChunkHeader(wdlFile, mareoffset, 'MARE', (2 * (17 * 17)) + (2 * (16 * 16))); // outer heights+inner heights
                mareoffset += 8;

                // this might be invalid if map had no WDL
                Noggit::map_horizon_tile* horizon_tile = get_horizon_tile(y, x);

                // laod tile and extract WDL data
                if (!horizon_tile || regenerate)
                {
                    bool unload = !world->mapIndex.tileLoaded(index) && !world->mapIndex.tileAwaitingLoading(index);
                    MapTile* mTile = world->mapIndex.loadTile(index, false, false, false);

                    auto nloadedtiles = world->mapIndex.getNLoadedTiles();

                    if (mTile)
                        mTile->wait_until_loaded();

                    update_horizon_tile(mTile);
                    if (unload)
                        world->mapIndex.unloadTile(index);

                    auto test = get_horizon_tile(y, x);
                    horizon_tile = get_horizon_tile(y, x);
                }
                if (!horizon_tile)
                {
                    return; // failed to generate data somehow
                    LogError << "Failed to generate the WDL file." << std::endl;
                }

                wdlFile.Insert(mareoffset, sizeof(Noggit::map_horizon_tile::height_17), reinterpret_cast<char*>(&horizon_tile->height_17));
                mareoffset += sizeof(Noggit::map_horizon_tile::height_17);
                wdlFile.Insert(mareoffset, sizeof(Noggit::map_horizon_tile::height_16), reinterpret_cast<char*>(&horizon_tile->height_16));
                mareoffset += sizeof(Noggit::map_horizon_tile::height_16);

                // MAHO (maparea holes) MAHO was added in WOTLK ?
                //  {
                wdlFile.Extend(8);
                SetChunkHeader(wdlFile, mareoffset, 'MAHO', (2 * 16)); // 1 hole mask for each chunk
                mareoffset += 8;
                wdlFile.Extend(32);
                for (int i = 0; i < 16; ++i)
                {
                    wdlFile.Insert(mareoffset, 2, (char*)&horizon_tile->holes[i]);
                    mareoffset += 2;
                }
            }
            curPos += 4;
        }
    }
    BlizzardArchive::ClientFile f(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData(),
    BlizzardArchive::ClientFile::NEW_FILE);
    f.setBuffer(wdlFile.all_data());
    f.save();
    f.close();

    set_minimap(&world->mapIndex);
}

bool map_horizon::wmoHasLowRes(WMOInstance* instance)
{
  assert(instance->lowResWmo.has_value() == false);
  if (instance->lowResWmo.has_value())
    return true;

  int i = 0;
  for (auto& lowres_wmo : lWMOInstances)
  {
    if (lowres_wmo.uniqueID == instance->uid)
    {
      auto low_res_model = mWMOFilenames[lowres_wmo.nameID];

      // TODO check positions
      // need to convert coords?
      auto dir = math::degrees::vec3{ math::degrees(
        lowres_wmo.rot[0])._, math::degrees(lowres_wmo.rot[1])._, math::degrees(lowres_wmo.rot[2])._ };

      if (misc::vec3d_equals(glm::vec3(lowres_wmo.pos[0], lowres_wmo.pos[1], lowres_wmo.pos[2]), instance->pos)
        && misc::deg_vec3d_equals(dir, instance->dir)
        && misc::float_equals( (lowres_wmo.scale / 1024.0f), instance->scale))
      {
        // instance->lowResWmo = scoped_wmo_reference(low_res_model, instance->wmo->_context);
        instance->lowResInstance = &lowres_wmo;
        instance->lowResWmo = &wmos[instance->lowResInstance->nameID];

        return true;
      }
      else
      {
        assert(false);
      }
    }
    
    i++;
  }

  return false;
}

map_horizon::minimap::minimap(const map_horizon& horizon)
{
  std::vector<uint32_t> texture(1024 * 1024);

  for (size_t y (0); y < 64; ++y)
  {
    for (size_t x (0); x < 64; ++x)
    {
      if (!horizon._tiles[y][x])
        continue;

      //! \todo There also is a second heightmap appended which has additional 16*16 pixels.

      // use the (nearly) full resolution available to us.
      // the data is layed out as a triangle fans with with 17 outer values
      // and 16 midpoints per tile. which in turn means:
      //      _tiles[y][x]->height_17[16][16] == _tiles[y][x + 1]->height_17[0][0]
      for (size_t j (0); j < 16; ++j)
      {
        for (size_t i (0); i < 16; ++i)
        {
          texture[(y * 16 + j) * 1024 + x * 16 + i] = color_for_height (horizon._tiles[y][x]->height_17[j][i]);
        }
      }
    }
  }

  bind();
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_BGRA, GL_UNSIGNED_BYTE, texture.data());
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

map_horizon::render::render(const map_horizon& horizon)
{
  std::vector<glm::vec3> vertices;

  for (size_t y (0); y < 64; ++y)
  {
    for (size_t x (0); x < 64; ++x)
    {
      if (!horizon._tiles[y][x])
        continue;

      _batches[y][x] = map_horizon_batch (static_cast<std::uint32_t>(vertices.size()), 17 * 17 + 16 * 16);

      for (size_t j (0); j < 17; ++j)
      {
        for (size_t i (0); i < 17; ++i)
        {
          vertices.emplace_back ( TILESIZE * (x + i / 16.0f)
                                , horizon._tiles[y][x]->height_17[j][i]
                                , TILESIZE * (y + j / 16.0f)
                                );
        }
      }

      for (size_t j (0); j < 16; ++j)
      {
        for (size_t i (0); i < 16; ++i)
        {
          vertices.emplace_back ( TILESIZE * (x + (i + 0.5f) / 16.0f)
                                , horizon._tiles[y][x]->height_16[j][i]
                                , TILESIZE * (y + (j + 0.5f) / 16.0f)
                                );
        }
      }
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3> (_vertex_buffer, vertices, GL_STATIC_DRAW);
}

static inline uint32_t outer_index(const map_horizon_batch &batch, int y, int x)
{
  return batch.vertex_start + y * 17 + x;
};

static inline uint32_t inner_index(const map_horizon_batch &batch, int y, int x)
{
  return batch.vertex_start + 17 * 17 + y * 16 + x;
};

void map_horizon::render::draw( glm::mat4x4 const& model_view
                              , glm::mat4x4 const& projection
                              , MapIndex *index
                              , const glm::vec3& color
                              , const float& cull_distance
                              , const math::frustum& frustum
                              , const glm::vec3& camera 
                              , display_mode display
                              )
{
  std::vector<uint32_t> indices;

  const TileIndex current_index(camera);
  const int lrr = 2;

  for (size_t y (current_index.z - lrr); y <= current_index.z + lrr; ++y)
  {
    for (size_t x (current_index.x - lrr); x < current_index.x + lrr; ++x)
    {
      // x and y are unsigned so negative signed int value are positive and > 63
      if (x > 63 || y > 63)
      {
        continue;
      }

      map_horizon_batch const& batch = _batches[y][x];

      if (batch.vertex_count == 0)
        continue;

      for (int j (0); j < 16; ++j)
      {
        for (int i (0); i < 16; ++i)
        {
          // do not draw over visible chunks

          /* TODO: when this optimization is turned off, we end up with inconsistent rendering between chunks and horizon batches.
           * Potentially it is caused by inconsistent coordinate space in visibility checking or chunk update system.
          if (index->tileLoaded({y, x}) && index->getTile({y, x})->getChunk(j, i)->is_visible(cull_distance, frustum, camera, display))
          {
            //continue;
          }
          */

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i));
          indices.push_back (outer_index (batch, j + 1, i + 1));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j + 1, i + 1));
          indices.push_back (outer_index (batch, j, i + 1));

          indices.push_back (inner_index (batch, j, i));
          indices.push_back (outer_index (batch, j, i + 1));
          indices.push_back (outer_index (batch, j, i));
        }
      }
    }
  }

  if (_map_horizon_program)
  {
    gl.bufferSubData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(_index_buffer, 0, indices);
  }
  else
  {
    gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(_index_buffer, indices, GL_DYNAMIC_DRAW);

    _map_horizon_program.reset
      ( new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("horizon_vs") }
          , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("horizon_fs") }
          }
      );
  
    _vaos.upload();
  }
   

  OpenGL::Scoped::use_program shader {*_map_horizon_program.get()};

  OpenGL::Scoped::vao_binder const _ (_vao);

  shader.uniform ("model_view", model_view);
  shader.uniform ("projection", projection);
  shader.uniform ("color", glm::vec3(color.x, color.y, color.z));

  shader.attrib ("position", _vertex_buffer, 3, GL_FLOAT, GL_FALSE, 0, 0);

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder (_index_buffer);

  
  gl.drawElements (GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
}

}
