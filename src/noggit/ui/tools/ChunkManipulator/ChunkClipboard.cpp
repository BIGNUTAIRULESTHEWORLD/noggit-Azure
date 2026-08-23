// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ChunkClipboard.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Alphamap.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/ChunkWater.hpp>
#include <noggit/ModelInstance.h>
#include <noggit/WMOInstance.h>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <noggit/texture_set.hpp>
#include <noggit/world_model_instances_storage.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

using namespace Noggit::Ui::Tools::ChunkManipulator;

namespace
{
  constexpr quint16 chunkAssetFormatMajor = 1;
  constexpr quint16 chunkAssetFormatMinor = 0;
  constexpr quint32 maximumAssetChunks = 4096;
  constexpr quint32 maximumAssetObjects = 1000000;
  constexpr quint32 maximumTextureLayers = 16;
  constexpr quint32 maximumLiquidLayers = 64;
  constexpr quint32 maximumAssetStringBytes = 1024 * 1024;
  constexpr qint64 maximumAssetFileBytes = 512ll * 1024ll * 1024ll;
  // Level 9 is dramatically slower for large selections while producing only
  // marginally smaller chunk assets. Level 6 keeps the existing zlib-based
  // format fully compatible and makes saving substantially faster.
  constexpr int chunkAssetCompressionLevel = 6;

  void configureAssetStream(QDataStream& stream)
  {
    stream.setVersion(QDataStream::Qt_5_12);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  }

  void writeAssetString(QDataStream& stream, std::string const& value)
  {
    QByteArray const bytes(value.data(), static_cast<int>(value.size()));
    stream << static_cast<quint32>(bytes.size());
    if (!bytes.isEmpty())
      stream.writeRawData(bytes.constData(), bytes.size());
  }

  bool readAssetString(QDataStream& stream, std::string& value)
  {
    quint32 size = 0;
    stream >> size;
    if (stream.status() != QDataStream::Ok || size > maximumAssetStringBytes)
      return false;
    QByteArray bytes(static_cast<int>(size), Qt::Uninitialized);
    if (size && stream.readRawData(bytes.data(), static_cast<int>(size)) != static_cast<int>(size))
      return false;
    value.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    return true;
  }

  template<std::size_t Size>
  void writeFloatArray(QDataStream& stream, std::array<float, Size> const& values)
  {
    for (float value : values)
      stream << value;
  }

  template<std::size_t Size>
  bool readFloatArray(QDataStream& stream, std::array<float, Size>& values)
  {
    for (float& value : values)
      stream >> value;
    return stream.status() == QDataStream::Ok;
  }

  template<std::size_t Size>
  void writeVec2Array(QDataStream& stream, std::array<glm::vec2, Size> const& values)
  {
    for (glm::vec2 const& value : values)
      stream << value.x << value.y;
  }

  template<std::size_t Size>
  bool readVec2Array(QDataStream& stream, std::array<glm::vec2, Size>& values)
  {
    for (glm::vec2& value : values)
      stream >> value.x >> value.y;
    return stream.status() == QDataStream::Ok;
  }

  template<std::size_t Size>
  void writeVec3Array(QDataStream& stream, std::array<glm::vec3, Size> const& values)
  {
    for (glm::vec3 const& value : values)
      stream << value.x << value.y << value.z;
  }

  template<std::size_t Size>
  bool readVec3Array(QDataStream& stream, std::array<glm::vec3, Size>& values)
  {
    for (glm::vec3& value : values)
      stream >> value.x >> value.y >> value.z;
    return stream.status() == QDataStream::Ok;
  }

  std::uint64_t chunkGridKey(float x, float z)
  {
    auto const grid_x = static_cast<std::int32_t>(std::floor(x / CHUNKSIZE));
    auto const grid_z = static_cast<std::int32_t>(std::floor(z / CHUNKSIZE));
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(grid_x)) << 32)
         | static_cast<std::uint32_t>(grid_z);
  }

  std::uint64_t relativeChunkKey(MapChunk const* chunk, glm::vec2 const& pivot)
  {
    auto const grid_x = static_cast<std::int32_t>(std::lround(
        (chunk->getCenter().x - pivot.x) / CHUNKSIZE));
    auto const grid_z = static_cast<std::int32_t>(std::lround(
        (chunk->getCenter().z - pivot.y) / CHUNKSIZE));
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(grid_x)) << 32)
         | static_cast<std::uint32_t>(grid_z);
  }

  int normalizeQuarterTurns(int turns)
  {
    return ((turns % 4) + 4) % 4;
  }

  float rotationDegrees(ChunkPasteOptions const& options)
  {
    return static_cast<float>(normalizeQuarterTurns(options.rotation_quarter_turns) * 90)
         + options.rotation_degrees;
  }

  std::uint8_t textureTransformSignature(ChunkPasteOptions const& options)
  {
    return static_cast<std::uint8_t>(normalizeQuarterTurns(options.rotation_quarter_turns)
        | (options.mirror_horizontal ? 1u << 2 : 0u)
        | (options.mirror_vertical ? 1u << 3 : 0u));
  }

  glm::vec2 rotateQuarterTurns(glm::vec2 value, int turns)
  {
    switch (normalizeQuarterTurns(turns))
    {
      // Noggit3's R hotkey maps (x, z) to (z, -x), while model headings gain
      // 90 degrees. Keep the terrain, objects, liquids, and texture animation
      // transforms on that same convention.
      case 1: return {value.y, -value.x};
      case 2: return {-value.x, -value.y};
      case 3: return {-value.y, value.x};
      default: return value;
    }
  }

  glm::vec2 transformRelative(glm::vec2 value, ChunkPasteOptions const& options)
  {
    if (options.mirror_horizontal)
      value.x = -value.x;
    if (options.mirror_vertical)
      value.y = -value.y;
    value = rotateQuarterTurns(value, options.rotation_quarter_turns);
    float const radians = glm::radians(options.rotation_degrees);
    float const cosine = std::cos(radians);
    float const sine = std::sin(radians);
    return {cosine * value.x + sine * value.y,
            -sine * value.x + cosine * value.y};
  }

  glm::vec2 inverseTransformRelative(glm::vec2 value, ChunkPasteOptions const& options)
  {
    float const radians = glm::radians(-options.rotation_degrees);
    float const cosine = std::cos(radians);
    float const sine = std::sin(radians);
    value = {cosine * value.x + sine * value.y,
             -sine * value.x + cosine * value.y};
    value = rotateQuarterTurns(value, -options.rotation_quarter_turns);
    if (options.mirror_horizontal)
      value.x = -value.x;
    if (options.mirror_vertical)
      value.y = -value.y;
    return value;
  }

  glm::vec3 transformPackedNormal(glm::vec3 packed, ChunkPasteOptions const& options)
  {
    // The tile heightmap texture stores (-normal.z, normal.y, -normal.x).
    glm::vec3 const world_normal{-packed.z, packed.y, -packed.x};
    glm::vec2 const transformed = transformRelative({world_normal.x, world_normal.z}, options);
    return {-transformed.y, world_normal.y, -transformed.x};
  }

  std::uint32_t transformTextureAnimation(std::uint32_t flags, ChunkPasteOptions const& options)
  {
    if ((flags & FLAG_ANIMATE) == 0)
      return flags;
    int direction = static_cast<int>(flags & 0x7u);
    if (options.mirror_horizontal)
      direction = 8 - direction;
    if (options.mirror_vertical)
      direction = 4 - direction;
    direction -= static_cast<int>(std::lround(rotationDegrees(options) / 45.f));
    direction = ((direction % 8) + 8) % 8;
    return (flags & ~0x7u) | static_cast<std::uint32_t>(direction);
  }

  float applyHeightMode(float destination, float source, ChunkHeightMode mode)
  {
    switch (mode)
    {
      case ChunkHeightMode::MINIMUM: return std::min(destination, source);
      case ChunkHeightMode::MAXIMUM: return std::max(destination, source);
      case ChunkHeightMode::ADD: return destination + source;
      case ChunkHeightMode::SUBTRACT: return destination - source;
      case ChunkHeightMode::NORMAL:
      default: return source;
    }
  }

  int nearestVertexIndex(ChunkCache const& cache, glm::vec2 point)
  {
    float const half_unit = UNITSIZE * 0.5f;
    int row = std::clamp(static_cast<int>(std::round((point.y - cache.zbase) / half_unit)), 0, 16);
    bool const inner = (row & 1) != 0;
    float const offset = inner ? half_unit : 0.f;
    int column = std::clamp(static_cast<int>(std::round((point.x - cache.xbase - offset) / UNITSIZE)),
                            0, inner ? 7 : 8);
    return 17 * (row / 2) + (inner ? 9 : 0) + column;
  }
}

ChunkClipboard::ChunkClipboard(MapView* map_view, QObject* parent)
  : QObject(parent)
  , _world(map_view->getWorld())
  , _map_view(map_view)
{
}

ChunkClipboard::ChunkClipboard(World* world, QObject* parent)
  : QObject(parent)
  , _world(world)
{
}

void ChunkClipboard::setSourcePivot(glm::vec2 pivot)
{
  glm::vec2 const adjustment = _source_pivot - pivot;
  for (ChunkObjectCacheEntry& object : _cached_objects)
  {
    object.relative_pos.x += adjustment.x;
    object.relative_pos.z += adjustment.y;
  }
  _source_pivot = pivot;
}

void ChunkClipboard::setWorldForPaste(World* world)
{
  clearPreview();
  for (auto const& index : _selected_chunks)
    setOverlay(index, 0);
  _selected_chunks.clear();
  _world = world;
}

ChunkClipboard::~ChunkClipboard()
{
  clearPreview();
  for (auto const& index : _selected_chunks)
    setOverlay(index, 0);
}

void ChunkClipboard::setOverlay(SelectedChunkIndex const& index, int value, bool force_upload)
{
  if (!_map_view)
    return;
  MapTile* tile = _world->mapIndex.getTile(index.tile_index);
  if (!tile || !tile->finishedLoading() || index.x >= 16 || index.z >= 16)
    return;
  tile->getChunk(index.x, index.z)->setChunkMoverOverlay(value, force_upload);
}

bool ChunkClipboard::selectRange(glm::vec3 const& cursor_pos, float radius, bool square,
                                 ChunkSelectionMode mode)
{
  bool changed = false;
  auto select = [this, mode, &changed](MapChunk* chunk)
  {
    SelectedChunkIndex const index{chunk->mt->index, static_cast<unsigned>(chunk->px),
                                    static_cast<unsigned>(chunk->py)};
    if (mode == ChunkSelectionMode::SELECT)
    {
      if (_selected_chunks.emplace(index).second)
      {
        chunk->setChunkMoverOverlay(2);
        changed = true;
      }
    }
    else
    {
      if (_selected_chunks.erase(index))
      {
        chunk->setChunkMoverOverlay(0);
        changed = true;
      }
    }
    return false; // selection is viewport state, not an ADT edit
  };
  if (square)
    _world->for_all_chunks_in_rect(cursor_pos, radius, select);
  else
    _world->for_all_chunks_in_range(cursor_pos, radius, select);
  if (changed)
    emit selectionChanged(_selected_chunks);
  return changed;
}

void ChunkClipboard::selectChunk(glm::vec3 const& pos, ChunkSelectionMode mode)
{
  _world->for_chunk_at(pos, [this, mode](MapChunk* chunk)
  {
    selectChunk(chunk->mt->index, static_cast<unsigned>(chunk->px),
                static_cast<unsigned>(chunk->py), mode);
  });
}

void ChunkClipboard::selectChunk(TileIndex const& tile_index, unsigned x, unsigned z,
                                 ChunkSelectionMode mode)
{
  if (!tile_index.is_valid() || x >= 16 || z >= 16 || !_world->mapIndex.hasTile(tile_index))
    return;

  SelectedChunkIndex const index{tile_index, x, z};
  if (mode == ChunkSelectionMode::SELECT)
  {
    _selected_chunks.emplace(index);
    setOverlay(index, 2);
  }
  else
  {
    _selected_chunks.erase(index);
    setOverlay(index, 0);
  }
  emit selectionChanged(_selected_chunks);
}

bool ChunkClipboard::copySelected()
{
  clearPreview();
  _history_blocked_preview_pivot.reset();
  _cached_chunks.clear();
  _cached_chunk_lookup.clear();
  _cached_objects.clear();
  _is_adt_asset = false;
  _texture_preview_cache.clear();
  _texture_preview_cache_transform.reset();

  if (_selected_chunks.empty())
  {
    clearPreview();
    emit copied(0, 0, 0);
    return false;
  }

  glm::vec2 minimum{std::numeric_limits<float>::max()};
  glm::vec2 maximum{std::numeric_limits<float>::lowest()};
  std::vector<glm::vec2> selected_centers;
  selected_centers.reserve(_selected_chunks.size());
  for (auto const& index : _selected_chunks)
  {
    MapTile* tile = _world->mapIndex.loadTile(index.tile_index);
    if (!tile)
      continue;
    tile->wait_until_loaded();
    glm::vec3 const center = tile->getChunk(index.x, index.z)->getCenter();
    selected_centers.emplace_back(center.x, center.z);
    minimum = glm::min(minimum, glm::vec2{center.x, center.z});
    maximum = glm::max(maximum, glm::vec2{center.x, center.z});
  }
  if (selected_centers.empty())
  {
    emit copied(0, 0, 0);
    return false;
  }

  // Match the proven Noggit3 mover convention: selections live on an integer
  // chunk grid and use start + size / 2 as their anchor. For even dimensions
  // this deliberately chooses the positive-side centre chunk rather than a
  // half-chunk geometric midpoint.
  int const width = static_cast<int>(std::lround((maximum.x - minimum.x) / CHUNKSIZE)) + 1;
  int const depth = static_cast<int>(std::lround((maximum.y - minimum.y) / CHUNKSIZE)) + 1;
  if (_map_view)
    _source_pivot = {minimum.x + (width / 2) * CHUNKSIZE,
                     minimum.y + (depth / 2) * CHUNKSIZE};
  else
    _source_pivot = {(minimum.x + maximum.x) * .5f,
                     (minimum.y + maximum.y) * .5f};
  _cached_chunks.reserve(_selected_chunks.size());

  for (auto const& index : _selected_chunks)
  {
    MapTile* tile = _world->mapIndex.loadTile(index.tile_index);
    if (!tile)
      continue;
    tile->wait_until_loaded();
    MapChunk* chunk = tile->getChunk(index.x, index.z);

    ChunkCache cache;
    cache.index = index;
    cache.xbase = chunk->xbase;
    cache.zbase = chunk->zbase;
    cache.holes = chunk->holes;
    cache.area_id = chunk->areaID;
    cache.flags = chunk->header_flags;

    for (int i = 0; i < 145; ++i)
    {
      cache.heights[i] = chunk->mVertices[i].y;
      cache.vertex_colors[i] = chunk->mccv[i];
    }
    auto const& heightmap_buffer = chunk->mt->getChunkHeightmapBuffer();
    int const chunk_start = (chunk->px * 16 + chunk->py) * mapbufsize * 4;
    for (int i = 0; i < mapbufsize; ++i)
      cache.normals[i] = {heightmap_buffer[chunk_start + i * 4],
                          heightmap_buffer[chunk_start + i * 4 + 1],
                          heightmap_buffer[chunk_start + i * 4 + 2]};
    std::copy(std::begin(chunk->_shadow_map), std::end(chunk->_shadow_map), cache.shadows.begin());
    std::copy(std::begin(chunk->texture_set->_doodadStencil),
              std::end(chunk->texture_set->_doodadStencil), cache.ground_effect_exclusion.begin());

    TextureSet* texture_set = chunk->getTextureSet();
    cache.textures.textures.reserve(texture_set->num());
    cache.textures.weights.resize(texture_set->num());
    cache.textures.layers.resize(texture_set->num());
    for (std::size_t layer = 0; layer < texture_set->num(); ++layer)
    {
      cache.textures.textures.push_back(texture_set->filename(layer));
      cache.textures.layers[layer] = texture_set->getMCLYEntries()[layer];
    }
    for (int pixel = 0; pixel < 64 * 64; ++pixel)
    {
      float base = 255.f;
      for (std::size_t layer = 1; layer < texture_set->num(); ++layer)
      {
        float const weight = static_cast<float>((*texture_set->getAlphamaps())[layer - 1]->getAlpha(pixel));
        cache.textures.weights[layer][pixel] = weight;
        base -= weight;
      }
      if (texture_set->num())
        cache.textures.weights[0][pixel] = std::max(0.f, base);
    }

    for (liquid_layer const& source_layer : *chunk->liquid_chunk()->getLayers())
    {
      ChunkLiquidLayerCache liquid;
      liquid.liquid_id = source_layer.liquidID();
      liquid.subchunks = const_cast<liquid_layer&>(source_layer).getSubchunks();
      auto const& vertices = const_cast<liquid_layer&>(source_layer).getVertices();
      for (int i = 0; i < 9 * 9; ++i)
      {
        liquid.heights[i] = vertices[i].position.y;
        liquid.depths[i] = vertices[i].depth;
        liquid.uvs[i] = vertices[i].uv;
      }
      cache.liquids.emplace_back(std::move(liquid));
    }
    std::size_t const cache_index = _cached_chunks.size();
    _cached_chunks.emplace_back(std::move(cache));
    _cached_chunk_lookup.emplace(
        chunkGridKey(chunk->xbase + CHUNKSIZE * 0.5f, chunk->zbase + CHUNKSIZE * 0.5f),
        cache_index);
  }

  {
    auto capture = [this](SceneObject& object)
    {
      if (object.chunk_mover_preview)
        return;
      // Copy an object only when its placement origin is inside a selected chunk.
      // Bounds may cross chunk borders, but that must not pull neighbouring objects
      // into the clipboard unexpectedly.
      if (!_cached_chunk_lookup.contains(chunkGridKey(object.pos.x, object.pos.z)))
        return;
      bool const is_wmo = object.which() == eWMO;
      ChunkObjectCacheEntry entry{object.instance_model()->file_key(),
                                  is_wmo ? ChunkManipulatorObjectTypes::WMO : ChunkManipulatorObjectTypes::M2,
                                  {object.pos.x - _source_pivot.x, object.pos.y,
                                   object.pos.z - _source_pivot.y}, object.dir, object.scale};
      if (is_wmo)
      {
        auto& wmo = static_cast<WMOInstance&>(object);
        entry.wmo_nameset = wmo.mNameset;
        entry.wmo_doodadset = wmo.doodadset();
      }
      _cached_objects.push_back(std::move(entry));
    };
    _world->getModelInstanceStorage().for_each_m2_instance([&](ModelInstance& object) { capture(object); });
    _world->getModelInstanceStorage().for_each_wmo_instance([&](WMOInstance& object) { capture(object); });
  }

  std::size_t m2_count = 0;
  std::size_t wmo_count = 0;
  for (auto const& object : _cached_objects)
    object.type == ChunkManipulatorObjectTypes::M2 ? ++m2_count : ++wmo_count;
  emit copied(_cached_chunks.size(), m2_count, wmo_count);
  return !_cached_chunks.empty();
}

bool ChunkClipboard::copyTileAt(glm::vec3 const& position, QString* error)
{
  TileIndex const tile_index(position);
  if (!tile_index.is_valid() || !_world->mapIndex.hasTile(tile_index))
  {
    if (error)
      *error = "There is no ADT under the cursor.";
    return false;
  }

  MapTile* tile = _world->mapIndex.loadTile(tile_index);
  if (!tile)
  {
    if (error)
      *error = "Unable to load the ADT under the cursor.";
    return false;
  }
  tile->wait_until_loaded();

  clearSelection();
  for (unsigned x = 0; x < 16; ++x)
  {
    for (unsigned z = 0; z < 16; ++z)
    {
      SelectedChunkIndex const index{tile_index, x, z};
      _selected_chunks.emplace(index);
      setOverlay(index, 2);
    }
  }
  emit selectionChanged(_selected_chunks);
  if (!copySelected() || _cached_chunks.size() != 256)
  {
    if (error)
      *error = "The current ADT could not be captured completely.";
    return false;
  }
  _is_adt_asset = true;
  return true;
}

bool ChunkClipboard::saveAsset(QString const& path, QString* error) const
{
  return saveAssetWithMagic(path, "NCHK", error);
}

bool ChunkClipboard::saveAdtAsset(QString const& path, QString* error) const
{
  if (!_is_adt_asset || _cached_chunks.size() != 256)
  {
    if (error)
      *error = "The clipboard does not contain a complete ADT export.";
    return false;
  }
  return saveAssetWithMagic(path, "NADT", error);
}

bool ChunkClipboard::saveAssetWithMagic(QString const& path, char const* magic,
                                        QString* error) const
{
  auto fail = [error](QString const& message)
  {
    if (error)
      *error = message;
    return false;
  };

  if (_cached_chunks.empty())
    return fail("The chunk clipboard is empty.");
  if (_cached_chunks.size() > maximumAssetChunks || _cached_objects.size() > maximumAssetObjects)
    return fail("The chunk selection is too large to save as an asset.");

  QByteArray payload;
  QDataStream stream(&payload, QIODevice::WriteOnly);
  configureAssetStream(stream);
  stream << _source_pivot.x << _source_pivot.y;
  stream << static_cast<quint32>(_cached_chunks.size());

  for (ChunkCache const& cache : _cached_chunks)
  {
    if (cache.textures.textures.size() != cache.textures.weights.size()
        || cache.textures.textures.size() != cache.textures.layers.size()
        || cache.textures.textures.size() > maximumTextureLayers
        || cache.liquids.size() > maximumLiquidLayers)
      return fail("The clipboard contains inconsistent chunk data.");

    stream << cache.xbase << cache.zbase;
    writeFloatArray(stream, cache.heights);
    writeVec3Array(stream, cache.normals);
    writeVec3Array(stream, cache.vertex_colors);
    stream.writeRawData(reinterpret_cast<char const*>(cache.shadows.data()),
                        static_cast<int>(cache.shadows.size()));

    stream << static_cast<quint32>(cache.textures.textures.size());
    for (std::size_t layer = 0; layer < cache.textures.textures.size(); ++layer)
    {
      writeAssetString(stream, cache.textures.textures[layer]);
      stream << static_cast<quint32>(cache.textures.layers[layer].flags)
             << static_cast<quint32>(cache.textures.layers[layer].effectID);
      writeFloatArray(stream, cache.textures.weights[layer]);
    }

    stream << static_cast<quint32>(cache.liquids.size());
    for (ChunkLiquidLayerCache const& liquid : cache.liquids)
    {
      stream << static_cast<qint32>(liquid.liquid_id)
             << static_cast<quint64>(liquid.subchunks);
      writeFloatArray(stream, liquid.heights);
      writeFloatArray(stream, liquid.depths);
      writeVec2Array(stream, liquid.uvs);
    }

    stream.writeRawData(reinterpret_cast<char const*>(cache.ground_effect_exclusion.data()),
                        static_cast<int>(cache.ground_effect_exclusion.size()));
    stream << static_cast<qint32>(cache.holes)
           << static_cast<quint32>(cache.area_id)
           << static_cast<quint32>(cache.flags.value);
  }

  stream << static_cast<quint32>(_cached_objects.size());
  for (ChunkObjectCacheEntry const& object : _cached_objects)
  {
    stream << static_cast<quint8>(object.type == ChunkManipulatorObjectTypes::WMO ? 0 : 1)
           << static_cast<quint8>(object.file_key.hasFilepath() ? 1 : 0);
    if (object.file_key.hasFilepath())
      writeAssetString(stream, object.file_key.filepath());
    stream << static_cast<quint32>(object.file_key.hasFileDataID()
                                   ? object.file_key.fileDataID() : 0)
           << object.relative_pos.x << object.relative_pos.y << object.relative_pos.z
           << object.dir.x << object.dir.y << object.dir.z
           << object.scale
           << static_cast<quint16>(object.wmo_nameset)
           << static_cast<quint16>(object.wmo_doodadset);
  }

  if (stream.status() != QDataStream::Ok)
    return fail("Unable to encode the chunk asset.");

  QByteArray const compressed = qCompress(payload, chunkAssetCompressionLevel);
  QByteArray const checksum = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return fail(QString("Unable to open %1 for writing: %2").arg(path, file.errorString()));

  QDataStream output(&file);
  configureAssetStream(output);
  if (output.writeRawData(magic, 4) != 4)
    return fail("Unable to write the chunk asset header.");
  output << chunkAssetFormatMajor << chunkAssetFormatMinor << checksum << compressed;
  if (output.status() != QDataStream::Ok || !file.commit())
    return fail(QString("Unable to finish writing %1: %2").arg(path, file.errorString()));
  return true;
}

bool ChunkClipboard::loadAsset(QString const& path, QString* error)
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
  if (file.size() <= 0 || file.size() > maximumAssetFileBytes)
    return fail("The chunk asset file is empty or exceeds the supported size limit.");

  QDataStream input(&file);
  configureAssetStream(input);
  char magic[4]{};
  if (input.readRawData(magic, 4) != 4
      || (std::memcmp(magic, "NCHK", 4) != 0 && std::memcmp(magic, "NADT", 4) != 0))
    return fail("This is not a Noggit chunk or ADT asset.");
  bool const is_adt_asset = std::memcmp(magic, "NADT", 4) == 0;

  quint16 major = 0;
  quint16 minor = 0;
  QByteArray checksum;
  QByteArray compressed;
  input >> major >> minor >> checksum >> compressed;
  if (input.status() != QDataStream::Ok || major != chunkAssetFormatMajor)
    return fail(QString("Unsupported chunk asset format version %1.%2.").arg(major).arg(minor));
  if (checksum.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256))
    return fail("The chunk asset checksum is invalid.");

  QByteArray const payload = qUncompress(compressed);
  if (payload.isEmpty() || payload.size() > maximumAssetFileBytes
      || QCryptographicHash::hash(payload, QCryptographicHash::Sha256) != checksum)
    return fail("The chunk asset is corrupt or incomplete.");

  QDataStream stream(payload);
  configureAssetStream(stream);
  glm::vec2 source_pivot{};
  quint32 chunk_count = 0;
  stream >> source_pivot.x >> source_pivot.y >> chunk_count;
  if (stream.status() != QDataStream::Ok || chunk_count == 0 || chunk_count > maximumAssetChunks)
    return fail("The chunk asset contains an invalid chunk count.");
  if (is_adt_asset && chunk_count != 256)
    return fail("The ADT asset does not contain exactly 256 chunks.");

  std::vector<ChunkCache> chunks;
  std::unordered_map<std::uint64_t, std::size_t> lookup;
  chunks.reserve(chunk_count);
  for (quint32 chunk_index = 0; chunk_index < chunk_count; ++chunk_index)
  {
    ChunkCache cache;
    stream >> cache.xbase >> cache.zbase;
    if (!readFloatArray(stream, cache.heights)
        || !readVec3Array(stream, cache.normals)
        || !readVec3Array(stream, cache.vertex_colors)
        || stream.readRawData(reinterpret_cast<char*>(cache.shadows.data()),
                              static_cast<int>(cache.shadows.size()))
             != static_cast<int>(cache.shadows.size()))
      return fail("The chunk asset ended while reading terrain data.");

    quint32 texture_count = 0;
    stream >> texture_count;
    if (stream.status() != QDataStream::Ok || texture_count > maximumTextureLayers)
      return fail("The chunk asset contains an invalid texture count.");
    cache.textures.textures.resize(texture_count);
    cache.textures.weights.resize(texture_count);
    cache.textures.layers.resize(texture_count);
    for (quint32 layer = 0; layer < texture_count; ++layer)
    {
      quint32 flags = 0;
      quint32 effect_id = 0;
      if (!readAssetString(stream, cache.textures.textures[layer]))
        return fail("The chunk asset contains an invalid texture path.");
      stream >> flags >> effect_id;
      cache.textures.layers[layer].flags = flags;
      cache.textures.layers[layer].effectID = effect_id;
      if (!readFloatArray(stream, cache.textures.weights[layer]))
        return fail("The chunk asset ended while reading texture weights.");
    }

    quint32 liquid_count = 0;
    stream >> liquid_count;
    if (stream.status() != QDataStream::Ok || liquid_count > maximumLiquidLayers)
      return fail("The chunk asset contains an invalid liquid-layer count.");
    cache.liquids.resize(liquid_count);
    for (ChunkLiquidLayerCache& liquid : cache.liquids)
    {
      qint32 liquid_id = 0;
      quint64 subchunks = 0;
      stream >> liquid_id >> subchunks;
      liquid.liquid_id = liquid_id;
      liquid.subchunks = subchunks;
      if (!readFloatArray(stream, liquid.heights)
          || !readFloatArray(stream, liquid.depths)
          || !readVec2Array(stream, liquid.uvs))
        return fail("The chunk asset ended while reading liquid data.");
    }

    if (stream.readRawData(reinterpret_cast<char*>(cache.ground_effect_exclusion.data()),
                           static_cast<int>(cache.ground_effect_exclusion.size()))
        != static_cast<int>(cache.ground_effect_exclusion.size()))
      return fail("The chunk asset ended while reading ground-effect data.");
    qint32 holes = 0;
    quint32 area_id = 0;
    quint32 flags = 0;
    stream >> holes >> area_id >> flags;
    cache.holes = holes;
    cache.area_id = area_id;
    cache.flags.value = flags;
    if (stream.status() != QDataStream::Ok)
      return fail("The chunk asset ended while reading chunk metadata.");

    std::uint64_t const key = chunkGridKey(cache.xbase + CHUNKSIZE * 0.5f,
                                           cache.zbase + CHUNKSIZE * 0.5f);
    if (!lookup.emplace(key, chunks.size()).second)
      return fail("The chunk asset contains duplicate chunk coordinates.");
    chunks.emplace_back(std::move(cache));
  }

  quint32 object_count = 0;
  stream >> object_count;
  if (stream.status() != QDataStream::Ok || object_count > maximumAssetObjects)
    return fail("The chunk asset contains an invalid object count.");
  std::vector<ChunkObjectCacheEntry> objects;
  objects.reserve(object_count);
  for (quint32 object_index = 0; object_index < object_count; ++object_index)
  {
    quint8 type = 0;
    quint8 has_path = 0;
    quint32 file_data_id = 0;
    std::string file_path;
    stream >> type >> has_path;
    if (type > 1 || has_path > 1 || (has_path && !readAssetString(stream, file_path)))
      return fail("The chunk asset contains invalid object metadata.");
    stream >> file_data_id;
    if (!has_path && file_data_id == 0)
      return fail("The chunk asset contains an object without a file reference.");

    ChunkObjectCacheEntry object;
    if (has_path && file_data_id)
      object.file_key = BlizzardArchive::Listfile::FileKey(file_path, file_data_id);
    else if (has_path)
      object.file_key = BlizzardArchive::Listfile::FileKey(file_path);
    else
      object.file_key = BlizzardArchive::Listfile::FileKey(file_data_id);
    object.type = type == 0 ? ChunkManipulatorObjectTypes::WMO
                            : ChunkManipulatorObjectTypes::M2;
    stream >> object.relative_pos.x >> object.relative_pos.y >> object.relative_pos.z
           >> object.dir.x >> object.dir.y >> object.dir.z
           >> object.scale >> object.wmo_nameset >> object.wmo_doodadset;
    if (stream.status() != QDataStream::Ok || !std::isfinite(object.scale)
        || object.scale <= 0.f)
      return fail("The chunk asset contains an invalid object placement.");
    objects.emplace_back(std::move(object));
  }
  if (stream.status() != QDataStream::Ok || !stream.atEnd())
    return fail("The chunk asset contains unexpected trailing or incomplete data.");

  clearPreview();
  for (SelectedChunkIndex const& index : _selected_chunks)
    setOverlay(index, 0, true);
  _selected_chunks.clear();
  _cached_chunks = std::move(chunks);
  _cached_chunk_lookup = std::move(lookup);
  _cached_objects = std::move(objects);
  _source_pivot = source_pivot;
  _is_adt_asset = is_adt_asset;
  _texture_preview_cache.clear();
  _texture_preview_cache_transform.reset();
  _history_blocked_preview_pivot.reset();

  std::size_t m2_count = 0;
  std::size_t wmo_count = 0;
  for (ChunkObjectCacheEntry const& object : _cached_objects)
    object.type == ChunkManipulatorObjectTypes::M2 ? ++m2_count : ++wmo_count;
  emit selectionChanged(_selected_chunks);
  emit selectionCleared();
  emit copied(_cached_chunks.size(), m2_count, wmo_count);
  return true;
}

void ChunkClipboard::clearSelection()
{
  clearPreview();
  _history_blocked_preview_pivot.reset();
  for (auto const& index : _selected_chunks)
    setOverlay(index, 0);
  _selected_chunks.clear();
  _cached_chunks.clear();
  _cached_chunk_lookup.clear();
  _cached_objects.clear();
  _is_adt_asset = false;
  _texture_preview_cache.clear();
  _texture_preview_cache_transform.reset();
  emit selectionChanged(_selected_chunks);
  emit copied(0, 0, 0);
  emit selectionCleared();
}

void ChunkClipboard::clearPreview()
{
  bool const had_preview = _preview_state_valid || _preview_overlay_state_valid
      || !_preview_chunks.empty() || !_preview_overlay_chunks.empty()
      || !_preview_object_uids.empty();

  for (auto const& preview : _preview_object_uids)
    _world->deleteChunkMoverPreviewInstance(preview.second);
  _preview_object_uids.clear();

  for (auto const& index : _preview_chunks)
  {
    MapTile* tile = _world->mapIndex.getTile(index.tile_index);
    if (tile && tile->finishedLoading() && index.x < 16 && index.z < 16)
    {
      MapChunk* chunk = tile->getChunk(index.x, index.z);
      if (_preview_display_options.heightmap
          && hasFlag(_preview_options.components, ChunkCopyFlags::TERRAIN))
      {
        chunk->setChunkMoverPreviewHeights(std::nullopt);
        chunk->setChunkMoverPreviewNormals(std::nullopt);
      }
      if (_preview_display_options.textures
          && hasFlag(_preview_options.components, ChunkCopyFlags::TEXTURES)
          && hasFlag(_preview_options.components, ChunkCopyFlags::ALPHAMAPS))
        chunk->texture_set->setChunkMoverTexturePreview(std::nullopt);
      if (hasFlag(_preview_options.components, ChunkCopyFlags::LIQUID))
        chunk->liquid_chunk()->setChunkMoverLiquidPreview(std::nullopt);
    }
  }
  _preview_chunks.clear();
  _preview_state_valid = false;

  for (auto const& index : _preview_overlay_chunks)
    if (_selected_chunks.find(index) == _selected_chunks.end())
      setOverlay(index, 0);
  _preview_overlay_chunks.clear();
  _preview_overlay_state_valid = false;

  // Preview teardown and terrain uploads can replace GPU instance data. Restamp
  // the logical selection even when the CPU-side overlay value is unchanged.
  if (had_preview)
    for (auto const& index : _selected_chunks)
      setOverlay(index, 2, true);
}

void ChunkClipboard::clearPreviewForHistoryChange(glm::vec3 const& destination)
{
  clearPreview();

  // Undo/redo can remove or recreate the real objects while the chunk mover's
  // viewport-only copies are still at the cursor. Keep that destination in
  // footprint-only mode until the cursor enters another chunk so the ghosts
  // cannot be mistaken for objects left behind by the action.
  if (MapChunk* pivot = _world->getChunkAt(destination))
    _history_blocked_preview_pivot = glm::vec2{pivot->getCenter().x, pivot->getCenter().z};
  else
    _history_blocked_preview_pivot.reset();
}

void ChunkClipboard::setOverlaysVisible(bool visible)
{
  if (!visible)
    clearPreview();
  for (auto const& index : _selected_chunks)
    setOverlay(index, visible ? 2 : 0, true);
}

bool ChunkClipboard::updatePreviewFootprint(glm::vec3 const& destination,
                                            ChunkPasteOptions const& options,
                                            ChunkPreviewOptions const& preview_options)
{
  if (_cached_chunks.empty() || !preview_options.enabled)
  {
    bool const changed = _preview_overlay_state_valid || !_preview_overlay_chunks.empty();
    clearPreview();
    _preview_overlay_display_options = preview_options;
    return changed;
  }

  MapChunk* pivot = _world->getChunkAt(destination);
  if (!pivot)
  {
    bool const changed = _preview_overlay_state_valid || !_preview_overlay_chunks.empty();
    clearPreview();
    return changed;
  }

  glm::vec2 const destination_pivot = _map_view
    ? glm::vec2{pivot->getCenter().x, pivot->getCenter().z}
    : glm::vec2{destination.x, destination.z};
  bool const same_target = _preview_overlay_state_valid
      && _preview_overlay_pivot.x == destination_pivot.x
      && _preview_overlay_pivot.y == destination_pivot.y
      && _preview_overlay_options.components == options.components
      && _preview_overlay_options.rotation_quarter_turns == options.rotation_quarter_turns
      && _preview_overlay_options.rotation_degrees == options.rotation_degrees
      && _preview_overlay_options.mirror_horizontal == options.mirror_horizontal
      && _preview_overlay_options.mirror_vertical == options.mirror_vertical
      && _preview_overlay_options.height_offset == options.height_offset
      && _preview_overlay_options.height_mode == options.height_mode
      && _preview_overlay_display_options == preview_options;
  if (same_target)
    return false;

  std::vector<MapChunk*> const destination_chunks = destinationChunks(destination_pivot, options);
  std::vector<SelectedChunkIndex> desired;
  desired.reserve(destination_chunks.size());
  std::set<SelectedChunkIndex> desired_set;
  for (MapChunk* chunk : destination_chunks)
  {
    SelectedChunkIndex const index{chunk->mt->index, static_cast<unsigned>(chunk->px),
                                    static_cast<unsigned>(chunk->py)};
    desired.push_back(index);
    desired_set.insert(index);
  }

  for (auto const& index : _preview_overlay_chunks)
    if (!desired_set.contains(index) && !_selected_chunks.contains(index))
      setOverlay(index, 0);
  for (auto const& index : desired)
    if (!_selected_chunks.contains(index))
      setOverlay(index, 3);

  _preview_overlay_chunks = std::move(desired);
  _preview_overlay_pivot = destination_pivot;
  _preview_overlay_options = options;
  _preview_overlay_display_options = preview_options;
  _preview_overlay_state_valid = true;
  return true;
}

glm::vec2 ChunkClipboard::inverseTransform(glm::vec2 const& destination_pos,
                                           glm::vec2 const& destination_pivot,
                                           ChunkPasteOptions const& options) const
{
  return _source_pivot + inverseTransformRelative(destination_pos - destination_pivot, options);
}

ChunkCache const* ChunkClipboard::sourceAt(glm::vec2 const& source_pos) const
{
  auto const found = _cached_chunk_lookup.find(chunkGridKey(source_pos.x, source_pos.y));
  if (found == _cached_chunk_lookup.end())
    return nullptr;

  ChunkCache const& cache = _cached_chunks[found->second];
  if (source_pos.x >= cache.xbase && source_pos.x < cache.xbase + CHUNKSIZE
      && source_pos.y >= cache.zbase && source_pos.y < cache.zbase + CHUNKSIZE)
    return &cache;
  return nullptr;
}

float ChunkClipboard::sampleHeight(ChunkCache const& cache, glm::vec2 const& point) const
{
  // Quarter-turns and mirrors map every destination terrain vertex onto an
  // exact source vertex. Preserve that discrete 145-vertex grid, as the
  // reference mover does, instead of interpolating triangles at paste time.
  return cache.heights[nearestVertexIndex(cache, point)];
}

std::optional<chunk_mover_texture_preview> ChunkClipboard::buildTexturePreview(
    MapChunk* destination_chunk, glm::vec2 const& destination_pivot,
    ChunkPasteOptions const& options) const
{
  glm::vec2 const destination_center{destination_chunk->getCenter().x,
                                     destination_chunk->getCenter().z};
  ChunkCache const* source_chunk = sourceAt(
      inverseTransform(destination_center, destination_pivot, options));
  if (!source_chunk || source_chunk->textures.textures.empty())
    return std::nullopt;

  // Chunk Mover supports only exact quarter-turns and mirrors. A destination
  // chunk therefore maps one-to-one to a source chunk, and its 64x64 alpha grid
  // can be permuted directly. Avoid the generic per-pixel world-coordinate
  // lookup and repeated string hashing that made large rotations stall.
  std::unordered_map<std::string, double> totals;
  std::unordered_map<std::string, std::uint32_t> flags;
  for (std::size_t layer = 0; layer < source_chunk->textures.textures.size(); ++layer)
  {
    std::string const& name = source_chunk->textures.textures[layer];
    double& total = totals[name];
    for (float weight : source_chunk->textures.weights[layer])
      total += weight;
    flags.try_emplace(name,
        transformTextureAnimation(source_chunk->textures.layers[layer].flags, options));
  }

  if (totals.empty())
    return std::nullopt;

  std::vector<std::pair<std::string, double>> ranked(totals.begin(), totals.end());
  std::sort(ranked.begin(), ranked.end(), [](auto const& lhs, auto const& rhs)
  {
    return lhs.second > rhs.second;
  });
  ranked.resize(std::min<std::size_t>(4, ranked.size()));

  chunk_mover_texture_preview preview;
  std::unordered_map<std::string, std::size_t> output_index;
  for (std::size_t layer = 0; layer < ranked.size(); ++layer)
  {
    output_index.emplace(ranked[layer].first, layer);
    preview.textures.emplace_back(ranked[layer].first, Noggit::NoggitRenderContext::MAP_VIEW);
    preview.flags[layer] = flags[ranked[layer].first];
  }

  std::vector<int> source_layer_to_output(source_chunk->textures.textures.size(), -1);
  for (std::size_t layer = 0; layer < source_chunk->textures.textures.size(); ++layer)
    if (auto const found = output_index.find(source_chunk->textures.textures[layer]);
        found != output_index.end())
      source_layer_to_output[layer] = static_cast<int>(found->second);

  int const turns = normalizeQuarterTurns(options.rotation_quarter_turns);
  for (int z = 0; z < 64; ++z)
    for (int x = 0; x < 64; ++x)
    {
      int const pixel_index = z * 64 + x;
      int sx = x;
      int sz = z;
      switch (turns)
      {
        case 1: sx = 63 - z; sz = x; break;
        case 2: sx = 63 - x; sz = 63 - z; break;
        case 3: sx = z; sz = 63 - x; break;
        default: break;
      }
      if (options.mirror_horizontal)
        sx = 63 - sx;
      if (options.mirror_vertical)
        sz = 63 - sz;
      int const source_pixel = sz * 64 + sx;

      std::array<float, 4> weights{};
      for (std::size_t layer = 0; layer < source_layer_to_output.size(); ++layer)
        if (source_layer_to_output[layer] >= 0)
          weights[static_cast<std::size_t>(source_layer_to_output[layer])]
              += source_chunk->textures.weights[layer][source_pixel];
      float total = weights[0] + weights[1] + weights[2] + weights[3];
      if (total <= 0.f)
        weights[0] = total = 255.f;
      for (std::size_t layer = 1; layer < ranked.size(); ++layer)
        preview.alphamaps[layer - 1][pixel_index] = weights[layer] / total;
    }

  return preview;
}

std::vector<liquid_layer> ChunkClipboard::buildLiquidPreview(
    MapChunk* destination_chunk, glm::vec2 const& destination_pivot,
    ChunkPasteOptions const& options) const
{
  ChunkWater* destination_water = destination_chunk->liquid_chunk();
  std::vector<liquid_layer> preview;
  auto find_layer = [&preview](int liquid_id) -> liquid_layer*
  {
    auto const found = std::find_if(preview.begin(), preview.end(),
      [liquid_id](liquid_layer const& layer) { return layer.liquidID() == liquid_id; });
    return found == preview.end() ? nullptr : &*found;
  };

  // Recreate the transformed 8x8 visibility mask for every liquid type.
  for (int z = 0; z < 8; ++z)
    for (int x = 0; x < 8; ++x)
    {
      glm::vec2 const cell{destination_chunk->xbase + (x + 0.5f) * CHUNKSIZE / 8.f,
                           destination_chunk->zbase + (z + 0.5f) * CHUNKSIZE / 8.f};
      glm::vec2 const source = inverseTransform(cell, destination_pivot, options);
      ChunkCache const* source_chunk = sourceAt(source);
      if (!source_chunk)
        continue;
      int const sx = std::clamp(static_cast<int>((source.x - source_chunk->xbase)
                                                 / CHUNKSIZE * 8.f), 0, 7);
      int const sz = std::clamp(static_cast<int>((source.y - source_chunk->zbase)
                                                 / CHUNKSIZE * 8.f), 0, 7);
      for (ChunkLiquidLayerCache const& source_layer : source_chunk->liquids)
      {
        if ((source_layer.subchunks & (std::uint64_t{1} << (sz * 8 + sx))) == 0)
          continue;
        liquid_layer* target = find_layer(source_layer.liquid_id);
        if (!target)
        {
          float const initial_height = source_layer.heights[sz * 9 + sx]
                                     + options.height_offset;
          preview.emplace_back(destination_water,
            glm::vec3{destination_chunk->xbase, initial_height, destination_chunk->zbase},
            initial_height, source_layer.liquid_id);
          target = &preview.back();
        }
        target->setSubchunk(x, z, true);
      }
    }

  glm::vec2 const center{destination_chunk->getCenter().x, destination_chunk->getCenter().z};
  ChunkCache const* scalar_source = sourceAt(inverseTransform(center, destination_pivot, options));
  if (!scalar_source)
    return {};

  // Copy the transformed 9x9 height/depth/UV grid for each visible layer.
  for (liquid_layer& target : preview)
  {
    auto const source_layer = std::find_if(scalar_source->liquids.begin(),
      scalar_source->liquids.end(), [&](ChunkLiquidLayerCache const& layer)
      { return layer.liquid_id == target.liquidID(); });
    if (source_layer == scalar_source->liquids.end())
      continue;

    auto& vertices = target.getVertices();
    for (int z = 0; z < 9; ++z)
      for (int x = 0; x < 9; ++x)
      {
        glm::vec2 const point{destination_chunk->xbase + x * UNITSIZE,
                              destination_chunk->zbase + z * UNITSIZE};
        glm::vec2 const source = inverseTransform(point, destination_pivot, options);
        int const sx = std::clamp(static_cast<int>(std::round(
            (source.x - scalar_source->xbase) / UNITSIZE)), 0, 8);
        int const sz = std::clamp(static_cast<int>(std::round(
            (source.y - scalar_source->zbase) / UNITSIZE)), 0, 8);
        int const source_vertex = sz * 9 + sx;
        int const target_vertex = z * 9 + x;
        vertices[target_vertex].position.y = source_layer->heights[source_vertex]
                                             + options.height_offset;
        vertices[target_vertex].depth = source_layer->depths[source_vertex];
        vertices[target_vertex].uv = source_layer->uvs[source_vertex];
      }
    target.refresh();
  }
  return preview;
}

std::vector<MapChunk*> ChunkClipboard::destinationChunks(glm::vec2 const& destination_pivot,
                                                         ChunkPasteOptions const& options,
                                                         bool load_missing_tiles) const
{
  std::vector<MapChunk*> result;
  result.reserve(_cached_chunks.size());
  std::unordered_set<MapChunk*> seen;
  for (ChunkCache const& cache : _cached_chunks)
  {
    std::array<glm::vec2, 4> const corners{{
      {cache.xbase, cache.zbase},
      {cache.xbase + CHUNKSIZE, cache.zbase},
      {cache.xbase + CHUNKSIZE, cache.zbase + CHUNKSIZE},
      {cache.xbase, cache.zbase + CHUNKSIZE}}};
    glm::vec2 minimum{std::numeric_limits<float>::max()};
    glm::vec2 maximum{std::numeric_limits<float>::lowest()};
    for (glm::vec2 const& corner : corners)
    {
      glm::vec2 const transformed = destination_pivot
          + transformRelative(corner - _source_pivot, options);
      minimum = glm::min(minimum, transformed);
      maximum = glm::max(maximum, transformed);
    }
    int const first_x = static_cast<int>(std::floor(minimum.x / CHUNKSIZE));
    int const first_z = static_cast<int>(std::floor(minimum.y / CHUNKSIZE));
    int const last_x = static_cast<int>(std::floor((maximum.x - .001f) / CHUNKSIZE));
    int const last_z = static_cast<int>(std::floor((maximum.y - .001f) / CHUNKSIZE));

    // Arbitrary angles can make one source chunk overlap several destination
    // chunks. Visit its complete rotated bounding box; individual component
    // samplers preserve destination data outside the actual rotated source.
    for (int grid_z = first_z; grid_z <= last_z; ++grid_z)
      for (int grid_x = first_x; grid_x <= last_x; ++grid_x)
      {
        if (grid_x < 0 || grid_z < 0 || grid_x >= 64 * 16 || grid_z >= 64 * 16)
          continue;
        glm::vec2 const candidate_center{(static_cast<float>(grid_x) + 0.5f) * CHUNKSIZE,
                                         (static_cast<float>(grid_z) + 0.5f) * CHUNKSIZE};
        glm::vec3 const candidate_position{candidate_center.x, 0.f, candidate_center.y};
        TileIndex const tile_index(candidate_position);
        if (!tile_index.is_valid() || !_world->mapIndex.hasTile(tile_index))
          continue;

        MapTile* tile = _world->mapIndex.getTile(tile_index);
        if (load_missing_tiles && !tile)
          tile = _world->mapIndex.loadTile(tile_index);
        if (load_missing_tiles && tile)
          tile->wait_until_loaded();
        MapChunk* chunk = tile && tile->finishedLoading()
            ? _world->getChunkAt(candidate_position) : nullptr;
        if (chunk && seen.emplace(chunk).second)
          result.push_back(chunk);
      }
  }
  return result;
}

void ChunkClipboard::updatePreview(glm::vec3 const& destination,
                                   ChunkPasteOptions const& options,
                                   ChunkPreviewOptions const& preview_options)
{
  if (_cached_chunks.empty())
  {
    clearPreview();
    return;
  }
  if (!preview_options.enabled)
  {
    clearPreview();
    _preview_display_options = preview_options;
    return;
  }
  MapChunk* pivot = _world->getChunkAt(destination);
  if (!pivot)
  {
    clearPreview();
    return;
  }
  glm::vec2 const destination_pivot{pivot->getCenter().x, pivot->getCenter().z};
  updatePreviewFootprint(destination, options, preview_options);
  if (_history_blocked_preview_pivot)
  {
    if (_history_blocked_preview_pivot->x == destination_pivot.x
        && _history_blocked_preview_pivot->y == destination_pivot.y)
      return;
    _history_blocked_preview_pivot.reset();
  }
  bool const same_options = _preview_state_valid
    && _preview_pivot.x == destination_pivot.x && _preview_pivot.y == destination_pivot.y
    && _preview_options.components == options.components
    && _preview_options.rotation_quarter_turns == options.rotation_quarter_turns
    && _preview_options.rotation_degrees == options.rotation_degrees
    && _preview_options.mirror_horizontal == options.mirror_horizontal
    && _preview_options.mirror_vertical == options.mirror_vertical
    && _preview_options.height_offset == options.height_offset
    && _preview_options.height_mode == options.height_mode
    && _preview_display_options == preview_options;
  if (same_options)
    return;

  bool const previous_state_valid = _preview_state_valid;
  glm::vec2 const previous_pivot = _preview_pivot;
  ChunkPasteOptions const previous_options = _preview_options;
  ChunkPreviewOptions const previous_display_options = _preview_display_options;

  std::vector<MapChunk*> const destination_chunks = destinationChunks(destination_pivot, options);
  std::vector<SelectedChunkIndex> desired;
  desired.reserve(destination_chunks.size());
  for (MapChunk* chunk : destination_chunks)
    desired.push_back({chunk->mt->index, static_cast<unsigned>(chunk->px),
                       static_cast<unsigned>(chunk->py)});

  std::set<SelectedChunkIndex> const previous_chunks(_preview_chunks.begin(), _preview_chunks.end());
  std::set<SelectedChunkIndex> const desired_chunks(desired.begin(), desired.end());

  bool const previous_height_preview = previous_state_valid && previous_display_options.heightmap
      && hasFlag(previous_options.components, ChunkCopyFlags::TERRAIN);
  bool const previous_texture_preview = previous_state_valid && previous_display_options.textures
      && hasFlag(previous_options.components, ChunkCopyFlags::TEXTURES)
      && hasFlag(previous_options.components, ChunkCopyFlags::ALPHAMAPS);
  bool const previous_liquid_preview = previous_state_valid
      && hasFlag(previous_options.components, ChunkCopyFlags::LIQUID);
  bool const show_height_preview = preview_options.heightmap
      && hasFlag(options.components, ChunkCopyFlags::TERRAIN);
  bool const show_texture_preview = preview_options.textures
      && hasFlag(options.components, ChunkCopyFlags::TEXTURES)
      && hasFlag(options.components, ChunkCopyFlags::ALPHAMAPS);
  bool const show_liquid_preview = hasFlag(options.components, ChunkCopyFlags::LIQUID);

  bool const terrain_mapping_changed = !previous_state_valid
      || previous_pivot.x != destination_pivot.x || previous_pivot.y != destination_pivot.y
      || previous_options.components != options.components
      || previous_options.rotation_quarter_turns != options.rotation_quarter_turns
      || previous_options.rotation_degrees != options.rotation_degrees
      || previous_options.mirror_horizontal != options.mirror_horizontal
      || previous_options.mirror_vertical != options.mirror_vertical
      || previous_options.height_offset != options.height_offset
      || previous_options.height_mode != options.height_mode;

  // Clear only previews that are leaving the destination footprint or whose
  // display component was disabled. Overlapping chunks are overwritten once.
  for (auto const& index : _preview_chunks)
  {
    bool const remains = desired_chunks.contains(index);
    MapTile* tile = _world->mapIndex.getTile(index.tile_index);
    if (tile && tile->finishedLoading() && index.x < 16 && index.z < 16)
    {
      MapChunk* chunk = tile->getChunk(index.x, index.z);
      if (previous_height_preview && (!remains || !show_height_preview))
      {
        chunk->setChunkMoverPreviewHeights(std::nullopt);
        chunk->setChunkMoverPreviewNormals(std::nullopt);
      }
      if (previous_texture_preview && (!remains || !show_texture_preview))
        chunk->texture_set->setChunkMoverTexturePreview(std::nullopt);
      if (previous_liquid_preview && (!remains || !show_liquid_preview))
        chunk->liquid_chunk()->setChunkMoverLiquidPreview(std::nullopt);
    }
  }

  _preview_state_valid = true;
  _preview_pivot = destination_pivot;
  _preview_options = options;
  _preview_display_options = preview_options;
  _preview_chunks = std::move(desired);

  if (show_texture_preview)
  {
    std::uint8_t const transform = textureTransformSignature(options);
    if (_texture_preview_cache_transform != transform)
    {
      _texture_preview_cache.clear();
      _texture_preview_cache_transform = transform;
    }
  }

  for (MapChunk* chunk : destination_chunks)
  {
    SelectedChunkIndex const index{chunk->mt->index, static_cast<unsigned>(chunk->px),
                                    static_cast<unsigned>(chunk->py)};
    glm::vec2 const center{chunk->getCenter().x, chunk->getCenter().z};
    ChunkCache const* source_chunk = sourceAt(inverseTransform(center, destination_pivot, options));

    bool const newly_previewed = !previous_chunks.contains(index);
    if (source_chunk && show_height_preview
        && (terrain_mapping_changed || newly_previewed || !previous_height_preview))
    {
      std::array<float, mapbufsize> heights{};
      std::array<glm::vec3, mapbufsize> normals{};
      for (int i = 0; i < mapbufsize; ++i)
      {
        glm::vec2 const source = inverseTransform({chunk->mVertices[i].x, chunk->mVertices[i].z},
                                                  destination_pivot, options);
        float const source_height = sampleHeight(*source_chunk, source) + options.height_offset;
        heights[i] = applyHeightMode(chunk->mVertices[i].y, source_height, options.height_mode);
        normals[i] = transformPackedNormal(
            source_chunk->normals[nearestVertexIndex(*source_chunk, source)], options);
      }
      chunk->setChunkMoverPreviewHeights(std::move(heights));
      chunk->setChunkMoverPreviewNormals(std::move(normals));
    }
    if (show_texture_preview
        && (terrain_mapping_changed || newly_previewed || !previous_texture_preview))
    {
      std::uint64_t const key = relativeChunkKey(chunk, destination_pivot);
      auto cached = _texture_preview_cache.find(key);
      if (cached == _texture_preview_cache.end())
        cached = _texture_preview_cache.emplace(
            key, buildTexturePreview(chunk, destination_pivot, options)).first;
      chunk->texture_set->setChunkMoverTexturePreview(cached->second);
    }
    if (show_liquid_preview
        && (terrain_mapping_changed || newly_previewed || !previous_liquid_preview))
      chunk->liquid_chunk()->setChunkMoverLiquidPreview(
          buildLiquidPreview(chunk, destination_pivot, options));
  }

  bool const object_transform_changed = !previous_state_valid
      || previous_pivot.x != destination_pivot.x || previous_pivot.y != destination_pivot.y
      || previous_options.rotation_quarter_turns != options.rotation_quarter_turns
      || previous_options.rotation_degrees != options.rotation_degrees
      || previous_options.mirror_horizontal != options.mirror_horizontal
      || previous_options.mirror_vertical != options.mirror_vertical
      || previous_options.height_offset != options.height_offset;

  for (std::size_t object_index = 0; object_index < _cached_objects.size(); ++object_index)
  {
    auto const& object = _cached_objects[object_index];
    auto existing = _preview_object_uids.find(object_index);
    if ((object.type == ChunkManipulatorObjectTypes::WMO
          && (!preview_options.wmos || !hasFlag(options.components, ChunkCopyFlags::WMOs)))
        || (object.type == ChunkManipulatorObjectTypes::M2
          && (!preview_options.m2s || !hasFlag(options.components, ChunkCopyFlags::MODELS))))
    {
      if (existing != _preview_object_uids.end())
      {
        _world->deleteChunkMoverPreviewInstance(existing->second);
        _preview_object_uids.erase(existing);
      }
      continue;
    }

    glm::vec2 const transformed = destination_pivot
      + transformRelative({object.relative_pos.x, object.relative_pos.z}, options);
    glm::vec3 const pos{transformed.x, object.relative_pos.y + options.height_offset, transformed.y};
    auto dir = object.dir;
    if (options.mirror_horizontal)
      dir.y = 180.f - dir.y;
    if (options.mirror_vertical)
      dir.y = -dir.y;
    dir.y += rotationDegrees(options);

    if (existing != _preview_object_uids.end() && object_transform_changed
        && !_world->updateChunkMoverPreviewInstance(existing->second, pos, dir, object.scale))
    {
      _preview_object_uids.erase(existing);
      existing = _preview_object_uids.end();
    }
    if (existing != _preview_object_uids.end())
      continue;

    SceneObject* preview = nullptr;
    if (object.type == ChunkManipulatorObjectTypes::WMO)
    {
      auto* wmo = _world->addChunkMoverPreviewWMO(object.file_key, pos, dir, object.scale);
      wmo->change_nameset(object.wmo_nameset);
      wmo->change_doodadset(object.wmo_doodadset);
      preview = wmo;
    }
    else
    {
      preview = _world->addChunkMoverPreviewM2(object.file_key, pos, object.scale, dir);
    }
    if (preview)
      _preview_object_uids.emplace(object_index, preview->uid);
  }
}

void ChunkClipboard::sewTerrain(std::vector<MapChunk*> const& changed_chunks)
{
  if (changed_chunks.empty())
    return;

  int min_x = 64 * 16;
  int min_z = 64 * 16;
  int max_x = -1;
  int max_z = -1;
  for (MapChunk* chunk : changed_chunks)
  {
    int const grid_x = static_cast<int>(chunk->mt->index.x) * 16 + chunk->px;
    int const grid_z = static_cast<int>(chunk->mt->index.z) * 16 + chunk->py;
    min_x = std::min(min_x, grid_x);
    min_z = std::min(min_z, grid_z);
    max_x = std::max(max_x, grid_x);
    max_z = std::max(max_z, grid_z);
  }

  int const width = max_x - min_x + 1;
  int const depth = max_z - min_z + 1;
  glm::vec3 const origin{min_x * CHUNKSIZE + 5.f, 0.f, min_z * CHUNKSIZE + 5.f};
  glm::vec3 const left_offset{-CHUNKSIZE, 0.f, 0.f};
  glm::vec3 const above_offset{0.f, 0.f, -CHUNKSIZE};

  // This is the Noggit3 chunk mover fix_gaps() traversal and edge-copy order.
  for (int x = -1; x <= width; ++x)
    for (int z = -1; z <= depth; ++z)
    {
      glm::vec3 const position = origin + glm::vec3{x * CHUNKSIZE, 0.f, z * CHUNKSIZE};
      MapChunk* chunk = _world->getChunkAt(position);
      if (chunk)
      {
        MapChunk* left = _world->getChunkAt(position + left_offset);
        MapChunk* above = _world->getChunkAt(position + above_offset);
        bool changed = false;

        // fixGapLeft/fixGapAbove modify only `chunk`. Noggit3 predates Yellow's
        // action system, so capture every possible target before either call.
        // This is undo/save bookkeeping only; the reference seam math and order
        // remain unchanged.
        if (left || above)
          NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        if (left)
          changed |= chunk->fixGapLeft(left);
        if (above)
          changed |= chunk->fixGapAbove(above);
        if (changed)
          _world->mapIndex.setChanged(chunk->mt);
      }
    }
}

void ChunkClipboard::recalcNormalsAroundTerrain(std::vector<MapChunk*> const& changed_chunks)
{
  if (changed_chunks.empty())
    return;

  int min_x = 64 * 16;
  int min_z = 64 * 16;
  int max_x = -1;
  int max_z = -1;
  for (MapChunk* chunk : changed_chunks)
  {
    int const grid_x = static_cast<int>(chunk->mt->index.x) * 16 + chunk->px;
    int const grid_z = static_cast<int>(chunk->mt->index.z) * 16 + chunk->py;
    min_x = std::min(min_x, grid_x);
    min_z = std::min(min_z, grid_z);
    max_x = std::max(max_x, grid_x);
    max_z = std::max(max_z, grid_z);
  }

  int const width = max_x - min_x + 1;
  int const depth = max_z - min_z + 1;
  glm::vec3 const origin{min_x * CHUNKSIZE + 5.f, 0.f, min_z * CHUNKSIZE + 5.f};

  // This is the matching Noggit3 recalc_normals_around_selection() pass.
  for (int x = -1; x < width + 1; ++x)
    for (int z = -1; z < depth + 1; ++z)
      if (MapChunk* chunk = _world->getChunkAt(
              origin + glm::vec3{x * CHUNKSIZE, 0.f, z * CHUNKSIZE}))
        _world->recalc_norms(chunk);
}

ChunkPasteResult ChunkClipboard::pasteSelection(glm::vec3 const& destination,
                                                ChunkPasteOptions const& options)
{
  ChunkPasteResult result;
  if (_cached_chunks.empty() || NOGGIT_CUR_ACTION)
    return result;

  MapChunk* pivot = _world->getChunkAt(destination);
  if (_map_view && !pivot)
    return result;
  // In the viewport the cursor must resolve to a real chunk. Offline callers
  // such as the ADT porter use the destination as a free rotation pivot, which
  // may intentionally sit over an empty map square outside the rotated shape.
  glm::vec2 const destination_pivot = pivot
      ? glm::vec2{pivot->getCenter().x, pivot->getCenter().z}
      : glm::vec2{destination.x, destination.z};
  // The reference mover guarantees that every destination ADT is loaded before
  // applying a cached chunk. Preview may remain limited to visible tiles, but a
  // real paste must never silently omit chunks at the loaded-area boundary.
  std::vector<MapChunk*> candidates = destinationChunks(destination_pivot, options, true);
  if (candidates.empty())
    return result;

  // Restore real destination data and remove render-only ghost objects before recording undo state.
  clearPreview();
  NOGGIT_ACTION_MGR->beginAction(_map_view,
    _map_view ? ActionFlags::eNO_FLAG : ActionFlags::eDO_NOT_WRITE_HISTORY);
  std::vector<MapChunk*> terrain_changed;

  for (MapChunk* chunk : candidates)
  {
    bool changed = false;
    glm::vec2 const center{chunk->getCenter().x, chunk->getCenter().z};
    glm::vec2 const source_center = inverseTransform(center, destination_pivot, options);
    ChunkCache const* scalar_source = sourceAt(source_center);

    if (hasFlag(options.components, ChunkCopyFlags::TERRAIN))
    {
      bool any = false;
      for (int i = 0; i < 145; ++i)
      {
        glm::vec2 const source = inverseTransform({chunk->mVertices[i].x, chunk->mVertices[i].z},
                                                  destination_pivot, options);
        ChunkCache const* source_chunk = sourceAt(source);
        if (!source_chunk)
          continue;
        if (!any)
          NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        float const source_height = sampleHeight(*source_chunk, source) + options.height_offset;
        chunk->mVertices[i].y = applyHeightMode(chunk->mVertices[i].y,
                                                source_height, options.height_mode);
        any = true;
      }
      if (any)
      {
        chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);
        terrain_changed.push_back(chunk);
        changed = true;
      }
    }

    if (scalar_source && hasFlag(options.components, ChunkCopyFlags::AREA_ID))
    {
      NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
      chunk->areaID = scalar_source->area_id;
      chunk->registerChunkUpdate(ChunkUpdateFlags::AREA_ID);
      changed = true;
    }
    if (scalar_source && hasFlag(options.components, ChunkCopyFlags::FLAGS))
    {
      NOGGIT_CUR_ACTION->registerChunkFlagChange(chunk);
      chunk->header_flags = scalar_source->flags;
      chunk->registerChunkUpdate(ChunkUpdateFlags::FLAGS);
      changed = true;
    }
    if (hasFlag(options.components, ChunkCopyFlags::HOLES))
    {
      int new_holes = chunk->holes;
      bool any = false;
      for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x)
        {
          glm::vec2 const cell{chunk->xbase + (x + 0.5f) * CHUNKSIZE / 4.f,
                               chunk->zbase + (z + 0.5f) * CHUNKSIZE / 4.f};
          glm::vec2 const source = inverseTransform(cell, destination_pivot, options);
          ChunkCache const* source_chunk = sourceAt(source);
          if (!source_chunk)
            continue;
          int sx = std::clamp(static_cast<int>((source.x - source_chunk->xbase) / (CHUNKSIZE / 4.f)), 0, 3);
          int sz = std::clamp(static_cast<int>((source.y - source_chunk->zbase) / (CHUNKSIZE / 4.f)), 0, 3);
          int const bit = 1 << (z * 4 + x);
          int const source_bit = 1 << (sz * 4 + sx);
          new_holes = (source_chunk->holes & source_bit) ? (new_holes | bit) : (new_holes & ~bit);
          any = true;
        }
      if (any)
      {
        NOGGIT_CUR_ACTION->registerChunkHoleChange(chunk);
        chunk->holes = new_holes;
        chunk->registerChunkUpdate(ChunkUpdateFlags::HOLES);
        changed = true;
      }
    }
    if (hasFlag(options.components, ChunkCopyFlags::SHADOWS))
    {
      bool any = false;
      std::array<std::uint8_t, 64 * 64> new_shadows{};
      std::copy(std::begin(chunk->_shadow_map), std::end(chunk->_shadow_map), new_shadows.begin());
      for (int z = 0; z < 64; ++z)
        for (int x = 0; x < 64; ++x)
        {
          glm::vec2 const pixel{chunk->xbase + (x + 0.5f) * CHUNKSIZE / 64.f,
                                chunk->zbase + (z + 0.5f) * CHUNKSIZE / 64.f};
          glm::vec2 const source = inverseTransform(pixel, destination_pivot, options);
          ChunkCache const* source_chunk = sourceAt(source);
          if (!source_chunk)
            continue;
          int sx = std::clamp(static_cast<int>((source.x - source_chunk->xbase) / CHUNKSIZE * 64.f), 0, 63);
          int sz = std::clamp(static_cast<int>((source.y - source_chunk->zbase) / CHUNKSIZE * 64.f), 0, 63);
          new_shadows[z * 64 + x] = source_chunk->shadows[sz * 64 + sx];
          any = true;
        }
      if (any)
      {
        NOGGIT_CUR_ACTION->registerChunkShadowChange(chunk);
        std::copy(new_shadows.begin(), new_shadows.end(), std::begin(chunk->_shadow_map));
        chunk->registerChunkUpdate(ChunkUpdateFlags::SHADOW);
        changed = true;
      }
    }
    if (hasFlag(options.components, ChunkCopyFlags::VERTEX_COLORS))
    {
      bool any = false;
      for (int i = 0; i < 145; ++i)
      {
        glm::vec2 const source = inverseTransform({chunk->mVertices[i].x, chunk->mVertices[i].z},
                                                  destination_pivot, options);
        ChunkCache const* source_chunk = sourceAt(source);
        if (!source_chunk)
          continue;
        if (!any)
          NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        chunk->mccv[i] = source_chunk->vertex_colors[nearestVertexIndex(*source_chunk, source)];
        any = true;
      }
      if (any)
      {
        chunk->registerChunkUpdate(ChunkUpdateFlags::MCCV);
        changed = true;
      }
    }
    if (hasFlag(options.components, ChunkCopyFlags::LIQUID))
    {
      ChunkWater* water = chunk->liquid_chunk();
      bool registered = false;
      auto ensure_registered = [&]
      {
        if (!registered)
        {
          NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
          registered = true;
        }
      };
      auto find_target_layer = [water](int liquid_id) -> liquid_layer*
      {
        for (liquid_layer& layer : *water->getLayers())
          if (layer.liquidID() == liquid_id)
            return &layer;
        return nullptr;
      };

      for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x)
        {
          glm::vec2 const cell{chunk->xbase + (x + 0.5f) * CHUNKSIZE / 8.f,
                               chunk->zbase + (z + 0.5f) * CHUNKSIZE / 8.f};
          glm::vec2 const source = inverseTransform(cell, destination_pivot, options);
          ChunkCache const* source_chunk = sourceAt(source);
          if (!source_chunk)
            continue;
          ensure_registered();
          for (liquid_layer& layer : *water->getLayers())
            layer.setSubchunk(x, z, false);

          int sx = std::clamp(static_cast<int>((source.x - source_chunk->xbase) / CHUNKSIZE * 8.f), 0, 7);
          int sz = std::clamp(static_cast<int>((source.y - source_chunk->zbase) / CHUNKSIZE * 8.f), 0, 7);
          for (auto const& source_layer : source_chunk->liquids)
          {
            if ((source_layer.subchunks & (std::uint64_t{1} << (sz * 8 + sx))) == 0)
              continue;
            liquid_layer* target = find_target_layer(source_layer.liquid_id);
            if (!target)
            {
              float const initial_height = source_layer.heights[sz * 9 + sx] + options.height_offset;
              water->getLayers()->emplace_back(water,
                glm::vec3{chunk->xbase, initial_height, chunk->zbase}, initial_height, source_layer.liquid_id);
              target = &water->getLayers()->back();
            }
            target->setSubchunk(x, z, true);
          }
        }

      if (registered)
      {
        for (liquid_layer& target : *water->getLayers())
        {
          auto& vertices = target.getVertices();
          for (int z = 0; z < 9; ++z)
            for (int x = 0; x < 9; ++x)
            {
              glm::vec2 const point{chunk->xbase + x * UNITSIZE, chunk->zbase + z * UNITSIZE};
              glm::vec2 const source = inverseTransform(point, destination_pivot, options);
              ChunkCache const* source_chunk = sourceAt(source);
              if (!source_chunk)
                continue;
              auto source_layer = std::find_if(source_chunk->liquids.begin(), source_chunk->liquids.end(),
                [&](ChunkLiquidLayerCache const& layer) { return layer.liquid_id == target.liquidID(); });
              if (source_layer == source_chunk->liquids.end())
                continue;
              int sx = std::clamp(static_cast<int>(std::round((source.x - source_chunk->xbase) / UNITSIZE)), 0, 8);
              int sz = std::clamp(static_cast<int>(std::round((source.y - source_chunk->zbase) / UNITSIZE)), 0, 8);
              int const source_vertex = sz * 9 + sx;
              int const target_vertex = z * 9 + x;
              vertices[target_vertex].position.y = source_layer->heights[source_vertex] + options.height_offset;
              vertices[target_vertex].depth = source_layer->depths[source_vertex];
              vertices[target_vertex].uv = source_layer->uvs[source_vertex];
            }
          target.refresh();
        }
        std::erase_if(*water->getLayers(), [](liquid_layer const& layer) { return layer.empty(); });
        water->update_layers();
        water->tagUpdate();
        changed = true;
      }
    }
    if (hasFlag(options.components, ChunkCopyFlags::TEXTURES)
        && hasFlag(options.components, ChunkCopyFlags::ALPHAMAPS))
    {
      TextureSet* destination_set = chunk->getTextureSet();
      destination_set->apply_alpha_changes();

      std::vector<std::string> destination_names;
      std::vector<layer_info> destination_layers;
      destination_names.reserve(destination_set->num());
      destination_layers.reserve(destination_set->num());
      for (std::size_t layer = 0; layer < destination_set->num(); ++layer)
      {
        destination_names.push_back(destination_set->filename(layer));
        destination_layers.push_back(destination_set->getMCLYEntries()[layer]);
      }

      auto destination_weight = [destination_set](std::size_t layer, int pixel)
      {
        if (layer > 0)
          return static_cast<float>((*destination_set->getAlphamaps())[layer - 1]->getAlpha(pixel));
        float base = 255.f;
        for (std::size_t other = 1; other < destination_set->num(); ++other)
          base -= static_cast<float>((*destination_set->getAlphamaps())[other - 1]->getAlpha(pixel));
        return std::max(0.f, base);
      };

      std::unordered_map<std::string, double> totals;
      std::unordered_map<std::string, layer_info> metadata;
      bool any_source_texture = false;
      for (int z = 0; z < 64; ++z)
        for (int x = 0; x < 64; ++x)
        {
          int const pixel_index = z * 64 + x;
          glm::vec2 const pixel{chunk->xbase + (x + 0.5f) * CHUNKSIZE / 64.f,
                                chunk->zbase + (z + 0.5f) * CHUNKSIZE / 64.f};
          glm::vec2 const source = inverseTransform(pixel, destination_pivot, options);
          if (ChunkCache const* source_chunk = sourceAt(source))
          {
            int sx = std::clamp(static_cast<int>((source.x - source_chunk->xbase) / CHUNKSIZE * 64.f), 0, 63);
            int sz = std::clamp(static_cast<int>((source.y - source_chunk->zbase) / CHUNKSIZE * 64.f), 0, 63);
            int const source_pixel = sz * 64 + sx;
            for (std::size_t layer = 0; layer < source_chunk->textures.textures.size(); ++layer)
            {
              auto const& name = source_chunk->textures.textures[layer];
              totals[name] += source_chunk->textures.weights[layer][source_pixel];
              layer_info info = source_chunk->textures.layers[layer];
              info.flags = transformTextureAnimation(info.flags, options);
              metadata.try_emplace(name, info);
            }
            any_source_texture = true;
          }
          else
          {
            for (std::size_t layer = 0; layer < destination_names.size(); ++layer)
            {
              totals[destination_names[layer]] += destination_weight(layer, pixel_index);
              metadata.try_emplace(destination_names[layer], destination_layers[layer]);
            }
          }
        }

      if (any_source_texture && !totals.empty())
      {
        std::vector<std::pair<std::string, double>> ranked(totals.begin(), totals.end());
        std::sort(ranked.begin(), ranked.end(), [](auto const& lhs, auto const& rhs)
        {
          return lhs.second > rhs.second;
        });
        if (ranked.size() > 4)
          result.textures_dropped += static_cast<int>(ranked.size() - 4);
        ranked.resize(std::min<std::size_t>(4, ranked.size()));

        std::unordered_map<std::string, std::size_t> output_index;
        for (std::size_t i = 0; i < ranked.size(); ++i)
          output_index.emplace(ranked[i].first, i);

        auto new_weights = std::make_unique<tmp_edit_alpha_values>();
        for (auto& map : new_weights->map)
          map.fill(0.f);

        for (int z = 0; z < 64; ++z)
          for (int x = 0; x < 64; ++x)
          {
            int const pixel_index = z * 64 + x;
            std::array<float, 4> weights{};
            glm::vec2 const pixel{chunk->xbase + (x + 0.5f) * CHUNKSIZE / 64.f,
                                  chunk->zbase + (z + 0.5f) * CHUNKSIZE / 64.f};
            glm::vec2 const source = inverseTransform(pixel, destination_pivot, options);
            if (ChunkCache const* source_chunk = sourceAt(source))
            {
              int sx = std::clamp(static_cast<int>((source.x - source_chunk->xbase) / CHUNKSIZE * 64.f), 0, 63);
              int sz = std::clamp(static_cast<int>((source.y - source_chunk->zbase) / CHUNKSIZE * 64.f), 0, 63);
              int const source_pixel = sz * 64 + sx;
              for (std::size_t layer = 0; layer < source_chunk->textures.textures.size(); ++layer)
                if (auto it = output_index.find(source_chunk->textures.textures[layer]); it != output_index.end())
                  weights[it->second] += source_chunk->textures.weights[layer][source_pixel];
            }
            else
            {
              for (std::size_t layer = 0; layer < destination_names.size(); ++layer)
                if (auto it = output_index.find(destination_names[layer]); it != output_index.end())
                  weights[it->second] += destination_weight(layer, pixel_index);
            }
            float total = weights[0] + weights[1] + weights[2] + weights[3];
            if (total <= 0.f)
              weights[0] = total = 255.f;
            for (std::size_t layer = 0; layer < ranked.size(); ++layer)
              new_weights->map[layer][pixel_index] = weights[layer] * 255.f / total;
          }

        NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
        destination_set->getTextures()->clear();
        for (auto const& entry : ranked)
          destination_set->getTextures()->emplace_back(entry.first, Noggit::NoggitRenderContext::MAP_VIEW);
        destination_set->setNTextures(ranked.size());

        std::array<std::unique_ptr<Alphamap>, MAX_ALPHAMAPS> new_alphas;
        for (std::size_t layer = 1; layer < ranked.size(); ++layer)
          new_alphas[layer - 1] = std::make_unique<Alphamap>();
        destination_set->setAlphamaps(new_alphas);
        for (std::size_t layer = 0; layer < 4; ++layer)
          destination_set->getMCLYEntries()[layer] = layer_info{};
        for (std::size_t layer = 0; layer < ranked.size(); ++layer)
        {
          layer_info info = metadata[ranked[layer].first];
          if (!hasFlag(options.components, ChunkCopyFlags::GROUND_EFFECTS))
            info.effectID = 0xFFFFFFFF;
          destination_set->getMCLYEntries()[layer] = info;
        }
        destination_set->getTempAlphamaps() = std::move(new_weights);
        destination_set->apply_alpha_changes();
        destination_set->markDirty();
        chunk->registerChunkUpdate(ChunkUpdateFlags::GROUND_EFFECT);
        changed = true;
      }
    }
    if (hasFlag(options.components, ChunkCopyFlags::GROUND_EFFECT_EXCLUSION))
    {
      bool any = false;
      auto stencil = chunk->texture_set->_doodadStencil;
      for (int z = 0; z < 8; ++z)
        for (int x = 0; x < 8; ++x)
        {
          glm::vec2 const cell{chunk->xbase + (x + 0.5f) * CHUNKSIZE / 8.f,
                               chunk->zbase + (z + 0.5f) * CHUNKSIZE / 8.f};
          glm::vec2 const source = inverseTransform(cell, destination_pivot, options);
          ChunkCache const* source_chunk = sourceAt(source);
          if (!source_chunk)
            continue;
          int sx = std::clamp(static_cast<int>((source.x - source_chunk->xbase) / CHUNKSIZE * 8.f), 0, 7);
          int sz = std::clamp(static_cast<int>((source.y - source_chunk->zbase) / CHUNKSIZE * 8.f), 0, 7);
          bool const excluded = (source_chunk->ground_effect_exclusion[sz] & (1u << sx)) != 0;
          stencil[z] = excluded ? static_cast<std::uint8_t>(stencil[z] | (1u << x))
                                : static_cast<std::uint8_t>(stencil[z] & ~(1u << x));
          any = true;
        }
      if (any)
      {
        NOGGIT_CUR_ACTION->registerChunkDetailDoodadExclusionChange(chunk);
        chunk->texture_set->_doodadStencil = stencil;
        chunk->registerChunkUpdate(ChunkUpdateFlags::DETAILDOODADS_EXCLUSION);
        changed = true;
      }
    }

    if (changed)
    {
      ++result.chunks_changed;
      _world->mapIndex.setChanged(chunk->mt);
    }
  }

  if (options.automatic_seams && !terrain_changed.empty())
    sewTerrain(terrain_changed);
  recalcNormalsAroundTerrain(terrain_changed);

  if (hasFlag(options.components, ChunkCopyFlags::MODELS)
      || hasFlag(options.components, ChunkCopyFlags::WMOs))
  {
    std::vector<std::uint32_t> remove;
    auto collect = [&](SceneObject& object)
    {
      if (object.chunk_mover_preview)
        return;
      bool const is_wmo = object.which() == eWMO;
      if ((is_wmo && !hasFlag(options.components, ChunkCopyFlags::WMOs))
          || (!is_wmo && !hasFlag(options.components, ChunkCopyFlags::MODELS)))
        return;
      glm::vec2 const source = inverseTransform({object.pos.x, object.pos.z}, destination_pivot, options);
      if (sourceAt(source))
        remove.push_back(object.uid);
    };
    _world->getModelInstanceStorage().for_each_m2_instance([&](ModelInstance& object) { collect(object); });
    _world->getModelInstanceStorage().for_each_wmo_instance([&](WMOInstance& object) { collect(object); });
    _world->deleteInstances(remove, true);
    result.objects_removed += static_cast<int>(remove.size());
  }

  for (auto const& object : _cached_objects)
  {
    if ((object.type == ChunkManipulatorObjectTypes::WMO && !hasFlag(options.components, ChunkCopyFlags::WMOs))
        || (object.type == ChunkManipulatorObjectTypes::M2 && !hasFlag(options.components, ChunkCopyFlags::MODELS)))
      continue;
    glm::vec2 const transformed = destination_pivot
      + transformRelative({object.relative_pos.x, object.relative_pos.z}, options);
    glm::vec3 pos{transformed.x, object.relative_pos.y + options.height_offset, transformed.y};
    auto dir = object.dir;
    if (options.mirror_horizontal)
      dir.y = 180.f - dir.y;
    if (options.mirror_vertical)
      dir.y = -dir.y;
    dir.y += rotationDegrees(options);
    if (object.type == ChunkManipulatorObjectTypes::WMO)
    {
      auto* added = _world->addWMOAndGetInstance(object.file_key, pos, dir, object.scale, false);
      if (!added)
        continue;
      added->change_nameset(object.wmo_nameset);
      added->change_doodadset(object.wmo_doodadset);
      NOGGIT_CUR_ACTION->registerObjectAdded(added);
    }
    else
    {
      // Register the stored instance explicitly, just like WMOs. The generic
      // insertion path can register a temporary pre-insertion M2 when `action`
      // is true; keeping that implicit path here made chunk-mover undo depend
      // on storage internals instead of the UID actually returned to the mover.
      auto* added = _world->addM2AndGetInstance(
          object.file_key, pos, object.scale, dir, nullptr, true, false);
      if (!added)
        continue;
      NOGGIT_CUR_ACTION->registerObjectAdded(added);
    }
    ++result.objects_added;
  }

  NOGGIT_ACTION_MGR->endAction();
  // Show the real, seam-repaired terrain after paste. Reapplying the cached
  // height ghost here hid the repaired boundary with the unsown source preview.
  // Keep only the cyan footprint until the user moves to a new destination.
  if (_map_view)
    updatePreviewFootprint(destination, options, _preview_display_options);
  emit pasted(result);
  return result;
}

std::set<SelectedChunkIndex> const& ChunkClipboard::selectedChunks() const
{
  return _selected_chunks;
}

std::size_t ChunkClipboard::cachedChunkCount() const
{
  return _cached_chunks.size();
}

bool ChunkClipboard::hasCopy() const
{
  return !_cached_chunks.empty();
}

bool ChunkClipboard::isAdtAsset() const
{
  return _is_adt_asset;
}
