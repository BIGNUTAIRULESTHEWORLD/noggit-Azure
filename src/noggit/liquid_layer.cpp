// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/liquid_layer.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/Misc.h>
#include <opengl/scoped.hpp>
#include <ClientFile.hpp>

#include <util/sExtendableArray.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace
{
  inline glm::vec2 default_uv(int px, int pz)
  {
    return {static_cast<float>(px) / 4.f, static_cast<float>(pz) / 4.f};
  }
}

liquid_layer::liquid_layer(ChunkWater* chunk, glm::vec3 const& base, float height, int liquid_id)
  : _liquid_id(liquid_id)
  , _liquid_vertex_format(LVF_HEIGHT_DEPTH)
  , _minimum(height)
  , _maximum(height)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  if (!gLiquidTypeDB.CheckIfIdExists(_liquid_id))
    _liquid_id = LIQUID_WATER;

  create_vertices(height);

  changeLiquidID(_liquid_id);
  
  update_min_max();
}

liquid_layer::liquid_layer(ChunkWater* chunk, glm::vec3 const& base, mclq& liquid, int liquid_id)
  : _liquid_id(liquid_id)
  , _liquid_vertex_format(LVF_HEIGHT_DEPTH)
  , _minimum(liquid.min_height)
  , _maximum(liquid.max_height)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  if (!gLiquidTypeDB.CheckIfIdExists(_liquid_id))
    _liquid_id = LIQUID_WATER;

  changeLiquidID(_liquid_id);

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      misc::set_bit(_subchunks, x, z, !liquid.tiles[z * 8 + x].dont_render);
    }
  }

  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      const unsigned v_index = z * 9 + x;
      mclq_vertex const& v = liquid.vertices[v_index];

      liquid_vertex lv;

      // _liquid_vertex_format is set by changeLiquidID()
      if (_liquid_vertex_format == LVF_HEIGHT_UV)
      {
        lv.depth = 1.f;
        lv.uv = { static_cast<float>(v.magma.x) / 255.f, static_cast<float>(v.magma.y) / 255.f };
      }
      else
      {
        lv.depth = static_cast<float>(v.water.depth) / 255.f;
        lv.uv = default_uv(x, z);
      }

      // sometimes there's garbage data on unused tiles that mess things up
      lv.position = { pos.x + UNITSIZE * x, std::clamp(v.height, _minimum, _maximum), pos.z + UNITSIZE * z };


      _vertices[v_index] = lv;
    }
  }
  update_min_max();
}

liquid_layer::liquid_layer(ChunkWater* chunk
                           , BlizzardArchive::ClientFile& f
                           , std::size_t base_pos
                           , glm::vec3 const& base
                           , MH2O_Information const& info
                           , std::uint64_t infomask)
  : _liquid_id(info.liquid_id)
  , _liquid_vertex_format(info.liquid_vertex_format)
  , _minimum(info.minHeight)
  , _maximum(info.maxHeight)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  // check if liquid id is valid or some downported maps will crash
  if (!gLiquidTypeDB.CheckIfIdExists(_liquid_id))
    _liquid_id = LIQUID_WATER;

  int offset = 0;
  for (int z = 0; z < info.height; ++z)
  {
    for (int x = 0; x < info.width; ++x)
    {
      setSubchunk(x + info.xOffset, z + info.yOffset, (infomask >> offset) & 1);
      offset++;
    }
  }

  // default values
  create_vertices(_minimum);

  if (info.ofsHeightMap)
  {
    f.seek(base_pos + info.ofsHeightMap);

    if (liquid_format_has_height(_liquid_vertex_format))
    {

      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
            float h;
            f.read(&h, sizeof(float));

            _vertices[z * 9 + x].position.y = std::clamp(h, _minimum, _maximum);
        }
      }
    }

    if (liquid_format_has_uv(_liquid_vertex_format))
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          mh2o_uv uv;
          f.read(&uv, sizeof(mh2o_uv));
          _vertices[z * 9 + x].uv =
            { static_cast<float>(uv.x) / 255.f
            , static_cast<float>(uv.y) / 255.f
            };
        }
      }
    }

    if (liquid_format_has_depth(_liquid_vertex_format))
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          std::uint8_t depth;
          f.read(&depth, sizeof(std::uint8_t));
          _vertices[z * 9 + x].depth = static_cast<float>(depth) / 255.f;
        }
      }
    }
  }

  changeLiquidID(_liquid_id); // to update the liquid type

  update_min_max();
}

liquid_layer::liquid_layer(liquid_layer&& other) noexcept
  : _liquid_id(other._liquid_id)
  , _liquid_vertex_format(other._liquid_vertex_format)
  , _minimum(other._minimum)
  , _maximum(other._maximum)
  // , _center(other._center)
  , _subchunks(other._subchunks)
  , _vertices(other._vertices)
  // , _indices_by_lod(other._indices_by_lod)
  , _fatigue_enabled(other._fatigue_enabled)
  , _surface_token(other._surface_token)
  , pos(other.pos)
  , _chunk(other._chunk)
{
  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
}

liquid_layer::liquid_layer(liquid_layer const& other)
  : _liquid_id(other._liquid_id)
  , _liquid_vertex_format(other._liquid_vertex_format)
  , _minimum(other._minimum)
  , _maximum(other._maximum)
  , _subchunks(other._subchunks)
  , _vertices(other._vertices)
  // , _indices_by_lod(other._indices_by_lod)
  , _fatigue_enabled(other._fatigue_enabled)
  , _surface_token(other._surface_token)
  , pos(other.pos)
  , _chunk(other._chunk)
{
  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
}

liquid_layer& liquid_layer::operator= (liquid_layer&& other) noexcept
{
  std::swap(_liquid_id, other._liquid_id);
  std::swap(_liquid_vertex_format, other._liquid_vertex_format);
  std::swap(_minimum, other._minimum);
  std::swap(_maximum, other._maximum);
  std::swap(_subchunks, other._subchunks);
  std::swap(_vertices, other._vertices);
  std::swap(_fatigue_enabled, other._fatigue_enabled);
  std::swap(_surface_token, other._surface_token);
  std::swap(pos, other.pos);
  // std::swap(_indices_by_lod, other._indices_by_lod);
  std::swap(_chunk, other._chunk);

  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
  other.changeLiquidID(other._liquid_id);

  return *this;
}

liquid_layer& liquid_layer::operator=(liquid_layer const& other)
{

  _liquid_vertex_format = other._liquid_vertex_format;
  _minimum = other._minimum;
  _maximum = other._maximum;
  _subchunks = other._subchunks;
  _vertices = other._vertices;
  pos = other.pos;
  // _indices_by_lod = other._indices_by_lod;
  _fatigue_enabled = other._fatigue_enabled;
  _surface_token = other._surface_token;
  _chunk = other._chunk;

  // update liquid type and vertex format
  changeLiquidID(other._liquid_id);
  return *this;
}

void liquid_layer::create_vertices(float height)
{
    int index = 0;
    for (int z = 0; z < 9; ++z)
    {
        const float posZ = pos.z + UNITSIZE * z;
        for (int x = 0; x < 9; ++x, ++index)
        {
            _vertices[index] = liquid_vertex( glm::vec3(pos.x + UNITSIZE * x, height, posZ)
                , default_uv(x, z)
                , 1.f
            );
        }
    }
}

void liquid_layer::save(util::sExtendableArray& adt, int base_pos, int& info_pos, int& current_pos) const
{
  int min_x = 9, min_z = 9, max_x = 0, max_z = 0;
  bool filled = true;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (hasSubchunk(x, z))
      {
        min_x = std::min(x, min_x);
        min_z = std::min(z, min_z);
        max_x = std::max(x + 1, max_x);
        max_z = std::max(z + 1, max_z);
      }
      else
      {
        filled = false;
      }
    }
  }

  // The 3.3.5 client only understands the original MH2O vertex formats 0-2.
  // Keep the editor-only combined format in memory, but serialize the channels
  // appropriate for the liquid type. Depth-only is safe only for the special
  // zero-height representation used by the client.
  int serialized_vertex_format = _liquid_vertex_format;
  if (serialized_vertex_format == LVF_HEIGHT_DEPTH_UV)
  {
    serialized_vertex_format = (_liquid_type == liquid_basic_types_magma
                             || _liquid_type == liquid_basic_types_slime)
                             ? LVF_HEIGHT_UV : LVF_HEIGHT_DEPTH;
  }
  else if (serialized_vertex_format == LVF_DEPTH
           && (!misc::float_equals(_minimum, 0.f)
               || !misc::float_equals(_maximum, 0.f)))
  {
    serialized_vertex_format = LVF_HEIGHT_DEPTH;
  }

  MH2O_Information info;
  std::uint64_t mask = 0;

  info.liquid_id = _liquid_id;
  info.liquid_vertex_format = serialized_vertex_format;
  info.minHeight = _minimum;
  info.maxHeight = _maximum;
  info.xOffset = min_x;
  info.yOffset = min_z;
  info.width = max_x - min_x;
  info.height = max_z - min_z;

  if (filled)
  {
    info.ofsInfoMask = 0;
  }
  else
  {
    std::uint64_t value = 1;
    for (int z = info.yOffset; z < info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x < info.xOffset + info.width; ++x)
      {
        if (hasSubchunk(x, z))
        {
          mask |= value;
        }
        value <<= 1;
      }
    }

    if (mask > 0)
    {
      info.ofsInfoMask = current_pos - base_pos;
      adt.Insert(current_pos, 8, reinterpret_cast<char*>(&mask));
      current_pos += 8;
    }
  }

  int vertices_count = (info.width + 1) * (info.height + 1);
  info.ofsHeightMap = current_pos - base_pos;

  if (liquid_format_has_height(serialized_vertex_format))
  {
    adt.Extend(vertices_count * sizeof(float));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        memcpy(adt.GetPointer<char>(current_pos).get(), &_vertices[z * 9 + x].position.y, sizeof(float));
        current_pos += sizeof(float);
      }
    }
  }
  // no heightmap/depth data for fatigue chunks
  else if (_fatigue_enabled)
  {
      info.ofsHeightMap = 0;
  }

  if (liquid_format_has_uv(serialized_vertex_format))
  {
    adt.Extend(vertices_count * sizeof(mh2o_uv));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        mh2o_uv uv;
        uv.x = static_cast<std::uint16_t>(std::clamp(_vertices[z * 9 + x].uv.x * 255.f, 0.f, 65535.f));
        uv.y = static_cast<std::uint16_t>(std::clamp(_vertices[z * 9 + x].uv.y * 255.f, 0.f, 65535.f));

        memcpy(adt.GetPointer<char>(current_pos).get(), &uv, sizeof(mh2o_uv));
        current_pos += sizeof(mh2o_uv);
      }
    }
  }

  if (serialized_vertex_format == LVF_HEIGHT_DEPTH
      || (serialized_vertex_format == LVF_DEPTH && !_fatigue_enabled))
  {
    adt.Extend(vertices_count * sizeof(std::uint8_t));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
          std::uint8_t depth = static_cast<std::uint8_t>(std::min(_vertices[z * 9 + x].depth * 255.0f, 255.f));
        memcpy(adt.GetPointer<char>(current_pos).get(), &depth, sizeof(std::uint8_t));
        current_pos += sizeof(std::uint8_t);
      }
    }
  }

  memcpy(adt.GetPointer<char>(info_pos).get(), &info, sizeof(MH2O_Information));
  info_pos += sizeof(MH2O_Information);
}

void liquid_layer::changeLiquidID(int id)
{
  _liquid_id = id;

  try
  {
    DBCFile::Record lLiquidTypeRow = gLiquidTypeDB.getByID(_liquid_id);

    _liquid_type = lLiquidTypeRow.getInt(LiquidTypeDB::Type);

    switch (_liquid_type)
    {
    case liquid_basic_types_magma:
      _mclq_liquid_type = mclq_liquid_magma;
      if (!liquid_format_has_uv(_liquid_vertex_format))
        _liquid_vertex_format = liquid_format_has_depth(_liquid_vertex_format)
                              ? LVF_HEIGHT_DEPTH_UV : LVF_HEIGHT_UV;
      break;
    case liquid_basic_types_slime:
      _mclq_liquid_type = mclq_liquid_slime;
      if (!liquid_format_has_uv(_liquid_vertex_format))
        _liquid_vertex_format = liquid_format_has_depth(_liquid_vertex_format)
                              ? LVF_HEIGHT_DEPTH_UV : LVF_HEIGHT_UV;
      break;
    case liquid_basic_types_ocean: // ocean
      _mclq_liquid_type = mclq_liquid_ocean;
      break;
    default: // river
      _mclq_liquid_type = mclq_liquid_river;
      break;
    }
  }
  catch (LiquidTypeDB::NotFound)
  {
      assert(false);
      LogError << "Liquid type id " << _liquid_type << " not found in LiquidType dbc" << std::endl;
  }
}

void liquid_layer::crop(MapChunk* chunk)
{
  if (_maximum < chunk->getMinHeight())
  {
    _subchunks = 0;
  }
  else
  {
    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        if (hasSubchunk(x, z))
        {
          int water_index = 9 * z + x, terrain_index = 17 * z + x;

          if ( _vertices[water_index +  0].position.y < chunk->mVertices[terrain_index +  0].y
            && _vertices[water_index +  1].position.y < chunk->mVertices[terrain_index +  1].y
            && _vertices[water_index +  9].position.y < chunk->mVertices[terrain_index + 17].y
            && _vertices[water_index + 10].position.y < chunk->mVertices[terrain_index + 18].y
            )
          {
            setSubchunk(x, z, false);
          }
        }
      }
    }
  }

  update_min_max();
}

void liquid_layer::update_opacity(MapChunk* chunk, float factor)
{
  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      update_vertex_opacity(x, z, chunk, factor);
    }
  }
}

void liquid_layer::update_underground_vertices_depth(MapChunk* chunk)
{
  // set depth = 0 to liquid verts under ground. This is for LODs.
  {
    for (int z = 0; z < 9; ++z)
    {
      for (int x = 0; x < 9; ++x)
      {
        float diff = _vertices[z * 9 + x].position.y - chunk->mVertices[z * 17 + x].y;

        if (diff < 0.f)
        {
          _vertices[z * 9 + x].depth = 0.f;
        }
        else
        {
          if (x < 8 && z < 8 && !hasSubchunk(x, z))
          {
            _vertices[z * 9 + x].depth = 0.f;
            _vertices[z * 9 + x + 1].depth = 0.f;
            _vertices[(z + 1) * 9 + x].depth = 0.f;
            _vertices[(z + 1) * 9 + (x + 1)].depth = 0.f;
          }
        }
      }
    }
  }
}

std::array<liquid_layer::liquid_vertex, 9 * 9>& liquid_layer::getVertices()
{
  return _vertices;
}

std::array<liquid_layer::liquid_vertex, 9 * 9> const& liquid_layer::getVertices() const
{
  return _vertices;
}

float liquid_layer::min() const
{
  return _minimum;
}

float liquid_layer::max() const
{
  return _maximum;
}

int liquid_layer::liquidID() const
{
  return _liquid_id;
}

liquid_basic_types liquid_layer::liquidType() const
{
  return static_cast<liquid_basic_types>(_liquid_type);
}

int liquid_layer::vertexFormat() const
{
  return _liquid_vertex_format;
}

std::uint64_t liquid_layer::surfaceToken() const
{
  return _surface_token;
}

void liquid_layer::setSurfaceToken(std::uint64_t token)
{
  _surface_token = token;
}

int liquid_layer::mclq_liquid_type() const
{
  return _mclq_liquid_type;
}

bool liquid_layer::hasSubchunk(int x, int z, int size) const
{
  for (int pz = z; pz < z + size; ++pz)
  {
    for (int px = x; px < x + size; ++px)
    {
      if ((_subchunks >> (pz * 8 + px)) & 1)
      {
        return true;
      }
    }
  }
  return false;
}

void liquid_layer::setSubchunk(int x, int z, bool water)
{
  misc::set_bit(_subchunks, x, z, water);
}

std::uint64_t liquid_layer::getSubchunks() const
{
  return _subchunks;
}

bool liquid_layer::empty() const
{
  return !_subchunks;
}

bool liquid_layer::full() const
{
  return _subchunks == std::uint64_t(-1);
}

void liquid_layer::clear()
{
  _subchunks = std::uint64_t(0);
}

void liquid_layer::paintLiquid( glm::vec3 const& cursor_pos
                              , float radius
                              , bool add
                              , math::radians const& angle
                              , math::radians const& orientation
                              , bool lock
                              , glm::vec3 const& origin
                              , bool override_height
                              , MapChunk* chunk
                              , float opacity_factor
                              )
{
  glm::vec3 ref ( lock
                      ? origin
                      : glm::vec3 (cursor_pos.x, cursor_pos.y + 1.0f, cursor_pos.z)
                      );

  std::array<bool, 9 * 9> vertices_used_before{};
  if (add && !override_height)
  {
    for (int z = 0; z < 9; ++z)
      for (int x = 0; x < 9; ++x)
        vertices_used_before[z * 9 + x] = usesVertex(x, z);
  }

  int id = 0;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (misc::getShortestDist(cursor_pos, _vertices[id].position, UNITSIZE) <= radius)
      {
        if (add)
        {
          for (int index : {id, id + 1, id + 9, id + 10})
          {
            bool no_subchunk = !hasSubchunk(x, z);
            bool in_range = misc::dist(cursor_pos, _vertices[index].position) <= radius;
            bool const vertex_existed_before = vertices_used_before[index];

            // Coverage may initialize only vertices that did not belong to the
            // surface before this operation. Reshape mode intentionally edits
            // existing vertices inside the brush as well.
            if ((no_subchunk && (override_height || !vertex_existed_before))
                || (in_range && override_height))
            {
              _vertices[index].position.y = misc::angledHeight(ref, _vertices[index].position, angle, orientation);
            }
            if (override_height ? (no_subchunk || in_range)
                                : (no_subchunk && !vertex_existed_before))
            {
              update_vertex_opacity(index % 9, index / 9, chunk, opacity_factor);
            }
          }
        }
        setSubchunk(x, z, add);
      }

      id++;
    }
    // to go to the next row of subchunks
    id++;
  }

  update_min_max();
}

void liquid_layer::update_min_max()
{
  _minimum = std::numeric_limits<float>::max();
  _maximum = std::numeric_limits<float>::lowest();
  int x = 0, z = 0;

  for (liquid_vertex& v : _vertices)
  {
    if (hasSubchunk(std::min(x, 7), std::min(z, 7)))
    {
      _maximum = std::max(_maximum, v.position.y);
      _minimum = std::min(_minimum, v.position.y);
    }

    if (++x == 9)
    {
      z++;
      x = 0;
    }
  }

  // Vertex format 2 is the client's special zero-height representation. Any
  // other surface must carry an explicit height map even when it is flat.
  if (_liquid_vertex_format == LVF_DEPTH
      && (!misc::float_equals(0.f, _minimum) || !misc::float_equals(0.f, _maximum)))
  {
    _liquid_vertex_format = LVF_HEIGHT_DEPTH;
  }
  else if (_liquid_vertex_format == LVF_HEIGHT_DEPTH
           && misc::float_equals(0.f, _minimum) && misc::float_equals(0.f, _maximum))
  {
    _liquid_vertex_format = LVF_DEPTH;
  }

  _fatigue_enabled = check_fatigue();
  // recalc all atributes instead?
  // _chunk->update_layers();
}

void liquid_layer::copy_subchunk_height(int x, int z, liquid_layer const& from)
{
  int id = 9 * z + x;

  for (int index : {id, id + 1, id + 9, id + 10})
  {
    _vertices[index].position.y = from._vertices[index].position.y;
  }

  setSubchunk(x, z, true);
}

bool liquid_layer::usesVertex(int x, int z) const
{
  for (int dz = -1; dz <= 0; ++dz)
    for (int dx = -1; dx <= 0; ++dx)
    {
      int const tile_x = x + dx;
      int const tile_z = z + dz;
      if (tile_x >= 0 && tile_x < 8 && tile_z >= 0 && tile_z < 8
          && hasSubchunk(tile_x, tile_z))
        return true;
    }

  return false;
}

void liquid_layer::paintDepth(glm::vec3 const& cursor_pos, float radius, float depth)
{
  depth = std::clamp(depth, 0.f, 1.f);
  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      auto& vertex = _vertices[z * 9 + x];
      float const dx = vertex.position.x - cursor_pos.x;
      float const dz = vertex.position.z - cursor_pos.z;
      if (usesVertex(x, z) && dx * dx + dz * dz <= radius * radius)
        vertex.depth = depth;
    }
  }

  if (!liquid_format_has_depth(_liquid_vertex_format))
    _liquid_vertex_format = liquid_format_has_uv(_liquid_vertex_format)
                            ? LVF_HEIGHT_DEPTH_UV : LVF_HEIGHT_DEPTH;
}

void liquid_layer::projectUV(glm::vec3 const& cursor_pos, float radius, float scale,
                             math::radians rotation)
{
  scale = std::max(scale, 0.0001f);
  float const cosine = std::cos(rotation._);
  float const sine = std::sin(rotation._);

  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      auto& vertex = _vertices[z * 9 + x];
      float const dx = vertex.position.x - cursor_pos.x;
      float const dz = vertex.position.z - cursor_pos.z;
      if (!usesVertex(x, z) || dx * dx + dz * dz > radius * radius)
        continue;

      // World-aligned projection keeps UVs continuous across chunk boundaries.
      float const world_u = vertex.position.x / scale;
      float const world_v = vertex.position.z / scale;
      float projected_u = std::fmod(world_u * cosine - world_v * sine, 256.f);
      float projected_v = std::fmod(world_u * sine + world_v * cosine, 256.f);
      if (projected_u < 0.f) projected_u += 256.f;
      if (projected_v < 0.f) projected_v += 256.f;
      vertex.uv = {projected_u, projected_v};
    }
  }

  if (!liquid_format_has_uv(_liquid_vertex_format))
    _liquid_vertex_format = liquid_format_has_depth(_liquid_vertex_format)
                            ? LVF_HEIGHT_DEPTH_UV : LVF_HEIGHT_UV;
}

ChunkWater* liquid_layer::getChunk()
{
  return _chunk;
}

void liquid_layer::refresh()
{
  update_min_max();
}

bool liquid_layer::has_fatigue() const
{
  return _fatigue_enabled;
}

void liquid_layer::disableFatigueOptimization()
{
  _fatigue_enabled = false;
}

void liquid_layer::update_vertex_opacity(int x, int z, MapChunk* chunk, float factor)
{
  const int  index = z * 9 + x;
  float diff = _vertices[index].position.y - chunk->mVertices[z * 17 + x].y;
  _vertices[z * 9 + x].depth = diff < 0.0f ? 0.0f : (std::min(1.0f, std::max(0.0f, (diff + 1.0f) * factor)));
}

int liquid_layer::get_lod_level(glm::vec3 const& camera_pos) const
{
  glm::vec3 const& center_vertex (_vertices[5 * 9 + 4].position);
  // this doesn't look like it's using the right length function...
  // auto const dist ((center_vertex - camera_pos).length());
  float const dist = misc::dist(center_vertex, camera_pos);

  return dist < 1000.f ? 0
       : dist < 2000.f ? 1
       : dist < 4000.f ? 2
       : 3;
}
// if ocean and all subchunks are at max depth
bool liquid_layer::check_fatigue() const
{
    // only oceans have fatigue
    if (_liquid_type != liquid_basic_types_ocean)
    {
        return false;
    }

    for (int z = 0; z < 8; ++z)
    {
        for (int x = 0; x < 8; ++x)
        {
            if (!(hasSubchunk(x, z) && subchunk_at_max_depth(x, z)))
            {
                return false;
            }
        }
    }

    return true;
}

mclq liquid_layer::to_mclq(MH2O_Attributes& attributes) const
{
  mclq mclq_data;

  mclq_data.min_height = _minimum;
  mclq_data.max_height = _maximum;

  for (int i = 0; i < 8 * 8; ++i)
  {
    if (hasSubchunk(i % 8, i / 8))
    {
      mclq_data.tiles[i].liquid_type = _mclq_liquid_type & 0x7;
      mclq_data.tiles[i].dont_render = 0;
      mclq_data.tiles[i].fishable = (attributes.fishable >> i) & 1;
      mclq_data.tiles[i].fatigue = (attributes.fatigue >> i) & 1;
    }
    else
    {
      mclq_data.tiles[i].liquid_type = 7;
      mclq_data.tiles[i].dont_render = 1;
      mclq_data.tiles[i].fishable = 0;
      mclq_data.tiles[i].fatigue = 0;
    }
  }

  for (int i = 0; i < 9 * 9; ++i)
  {
    mclq_data.vertices[i].height = _vertices[i].position.y;

    // magma and slime
    if (_liquid_type == 2 || _liquid_type == 3)
    {
      mclq_data.vertices[i].magma.x = static_cast<std::uint16_t>(std::clamp(_vertices[i].uv.x * 255.f, 0.f, 65535.f));
      mclq_data.vertices[i].magma.y = static_cast<std::uint16_t>(std::clamp(_vertices[i].uv.y * 255.f, 0.f, 65535.f));
    }
    else
    {
      mclq_data.vertices[i].water.depth = static_cast<std::uint8_t>(std::clamp(_vertices[i].depth * 255.f, 0.f, 255.f));
    }
  }

  return mclq_data;
}

int liquid_layer::mclq_flag_ordering() const
{
  switch (_mclq_liquid_type)
  {
  case 6: return 2;  // lava
  case 3: return 3;  // slime
  case 1: return 1;  // ocean
  default: return 0; // river

  }
}

void liquid_layer::update_attributes(MH2O_Attributes& attributes)
{
    if (check_fatigue())
    {
        attributes.fishable = 0xFFFFFFFFFFFFFFFF;
        attributes.fatigue = 0xFFFFFFFFFFFFFFFF;

        _fatigue_enabled = true;
    }
    else
    {
        _fatigue_enabled = false;
        for (int z = 0; z < 8; ++z)
        {
            for (int x = 0; x < 8; ++x)
            {
                if (hasSubchunk(x, z))
                {
                    // Fishability is authored independently from liquid type.
                    // WoW uses the flag for water, ocean, magma, and slime cells.
                    misc::set_bit(attributes.fishable, x, z, true);

                    // only oceans have fatigue
                    // warning: not used by TrinityCore
                    if (_liquid_type == liquid_basic_types_ocean && subchunk_at_max_depth(x, z))
                    {
                        misc::set_bit(attributes.fatigue, x, z, true);
                    }
                }
            }
        }
    }
}

bool liquid_layer::subchunk_at_max_depth(int x, int z) const
{
    for (int id_z = z; id_z <= z + 1; ++id_z)
    {
        for (int id_x = x; id_x <= x + 1; ++id_x)
        {
            if (_vertices[id_x + 9 * id_z].depth < 1.f)
            {
                return false;
            }
        }
    }

    return true;
}

liquid_layer::liquid_vertex::liquid_vertex(glm::vec3 const& pos, glm::vec2 const& uv, float depth) : position(pos), uv(uv), depth(depth) {}
