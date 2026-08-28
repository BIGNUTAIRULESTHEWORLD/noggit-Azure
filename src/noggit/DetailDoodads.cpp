// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DetailDoodads.hpp>

#include <noggit/BlizzardRandomizer.hpp>
#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/texture_set.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <unordered_map>

namespace
{
  // one cell of the chunk's 8x8 doodad grid: the client's UNITSIZE,
  // TILESIZE / 128 (533.33333 / 128 yards)
  constexpr float CELL = 4.1666665f;
  constexpr float HALF_CELL = 2.0833333f;

  // the 4-triangle cell fan through the centre vertex (CFacet::Set @ 0x7912C0)
  constexpr int VTX_A[4] = { 17, 0, 18, 1 };
  constexpr int VTX_B[4] = { 0, 1, 17, 18 };
  constexpr int CRN_A[4] = { 3, 0, 2, 1 };
  constexpr int CRN_B[4] = { 0, 1, 3, 2 };
  constexpr float CORNER[4][2] = { { 0.f, 0.f }, { 0.f, -CELL }, { -CELL, -CELL }, { -CELL, 0.f } };

  struct Facet
  {
    float a, b, c, d; // unit plane normal + offset
  };

  // Axes follow the client convention: x = the x17 vertex row (noggit z),
  // y = the in-row column (noggit x). Heights are absolute, which only shifts
  // the plane's d, so the plane evaluates to absolute height.
  Facet build_facet(MapChunk* chunk, int row, int col, int t)
  {
    int const base = 17 * row + col;
    float const base_x = -CELL * row;
    float const base_y = -CELL * col;

    glm::vec3 const C{ base_x - HALF_CELL, base_y - HALF_CELL, chunk->mVertices[base + 9].y };
    glm::vec3 const A{ base_x + CORNER[CRN_A[t]][0], base_y + CORNER[CRN_A[t]][1], chunk->mVertices[base + VTX_A[t]].y };
    glm::vec3 const B{ base_x + CORNER[CRN_B[t]][0], base_y + CORNER[CRN_B[t]][1], chunk->mVertices[base + VTX_B[t]].y };

    glm::vec3 const u = B - C;
    glm::vec3 const v = A - C;
    glm::vec3 n{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    n = glm::normalize(n);
    return { n.x, n.y, n.z, -glm::dot(n, C) };
  }

  std::atomic<std::uint32_t> dbc_stamp_counter{ 1 };

  // globally monotonic so a rebuilt cache never repeats a revision another
  // chunk (or a recycled chunk address) already handed to the GL batch cache
  std::atomic<std::uint32_t> revision_counter{ 0 };
}

std::uint32_t Noggit::DetailDoodads::dbcStamp()
{
  return dbc_stamp_counter.load();
}

void Noggit::DetailDoodads::bumpDbcStamp()
{
  ++dbc_stamp_counter;
}

void Noggit::DetailDoodads::generate(MapChunk* chunk, int density, NoggitRenderContext context,
                                     ChunkDetailDoodads& out, DetailDoodadPreview const* preview)
{
  out.chunk_stamp = chunk->detailDoodadStamp();
  out.dbc_stamp = dbcStamp();
  out.density = density;
  out.revision = ++revision_counter;
  out.models.clear();
  out.placements.clear();

  TextureSet* texture_set = chunk->getTextureSet();
  if (!texture_set->num())
  {
    return;
  }

  // untouched chunks keep the doodadMapping stored in the ADT, which is what
  // the client renders from; only alpha edits force noggit's recompute
  if (chunk->doodadMappingNeedsUpdate())
  {
    texture_set->updateDoodadMapping();
    chunk->clearDoodadMappingNeedsUpdate();
  }

  std::uint16_t const* mapping = texture_set->getDoodadMappingBase();
  std::uint8_t const* stencil = texture_set->getDoodadStencilBase();

  // client seed: cOffset.x | (cOffset.y << 16) with the global chunk index on
  // client axes. CMapChunk::Create (0x7C64B0) derives topLeftCoords.x from
  // cOffset.y and topLeftCoords.y from cOffset.x, so cOffset.x is the world Y
  // axis (noggit x / px) and cOffset.y is the world X axis (noggit z / py).
  std::uint32_t const coffset_x = static_cast<std::uint32_t>(chunk->mt->index.x) * 16 + chunk->px;
  std::uint32_t const coffset_y = static_cast<std::uint32_t>(chunk->mt->index.z) * 16 + chunk->py;

  Noggit::BlizzardRandomizer rnd(coffset_x | (coffset_y << 16));

  // pass 1: cell picks, random with replacement; duplicates spawn again
  int const D = std::clamp(density, 16, 256);
  std::uint8_t pick_col[256];
  std::uint8_t pick_row[256];

  for (int i = 0; i < D; ++i)
  {
    pick_col[i] = rnd.shuffle() & 7; // first random: column -> noggit x
    pick_row[i] = rnd.shuffle() & 7; // second random: row -> noggit z
  }

  // the client's facet prebuild consumes no randoms, so building lazily keeps
  // the random sequence bit-identical
  Facet facets[64][4];
  bool facet_built[64][4] = {};

  struct EffectData
  {
    bool valid = false;
    std::int32_t tbl[16] = {};
    std::uint32_t amount = 0;
  };
  std::unordered_map<unsigned int, EffectData> effects;

  EffectData preview_effect;
  if (preview && preview->enabled && !preview->texture.empty())
  {
    int write_pos = 0;
    int total = 0;
    for (int k = 0; k < 4; ++k)
    {
      std::int32_t weight = preview->weights[k];
      if (weight <= 0)
      {
        continue;
      }
      total += weight;
      while (weight--)
      {
        // Negative ids address the preview filenames without creating DBC records.
        preview_effect.tbl[write_pos & 15] = -(k + 1);
        write_pos += 13;
      }
    }
    while (total < 16)
    {
      preview_effect.tbl[write_pos & 15] = -((total & 3) + 1);
      ++total;
      write_pos += 13;
    }
    preview_effect.amount = preview->amount ? preview->amount : 8;
    preview_effect.valid = true;
  }

  auto resolve_effect = [&](unsigned int effect_id) -> EffectData const&
  {
    auto it = effects.find(effect_id);
    if (it != effects.end())
    {
      return it->second;
    }

    EffectData& data = effects[effect_id];
    if (!effect_id || effect_id == 0xFFFFFFFF || !gGroundEffectTextureDB.CheckIfIdExists(effect_id))
    {
      return data;
    }

    DBCFile::Record record = gGroundEffectTextureDB.getByID(effect_id);

    std::int32_t ids[4];
    std::int32_t weights[4];
    for (int k = 0; k < 4; ++k)
    {
      ids[k] = record.getInt(GroundEffectTextureDB::Doodads + k);
      weights[k] = record.getInt(GroundEffectTextureDB::Weights + k);
    }

    // 16-slot table, stride 13 (coprime with 16 -> scatters runs); the last 16
    // writes win, the padding cycles the id slots including zeroes
    int w = 0;
    int total = 0;
    for (int k = 0; k < 4; ++k)
    {
      std::int32_t weight = weights[k];
      if (weight <= 0)
      {
        continue;
      }
      total += weight;
      while (weight--)
      {
        data.tbl[w & 15] = ids[k];
        w += 13;
      }
    }
    while (total < 16)
    {
      data.tbl[w & 15] = ids[total & 3];
      ++total;
      w += 13;
    }

    data.amount = record.getUInt(GroundEffectTextureDB::Amount);
    if (!data.amount)
    {
      data.amount = 8;
    }
    data.valid = true;
    return data;
  };

  struct DoodadModel
  {
    int index = -1;
    bool align = false;
  };
  std::unordered_map<std::int32_t, DoodadModel> doodad_models;

  auto resolve_doodad = [&](std::int32_t id) -> DoodadModel const&
  {
    auto it = doodad_models.find(id);
    if (it != doodad_models.end())
    {
      return it->second;
    }

    DoodadModel& dm = doodad_models[id];
    if (id < 0 && preview)
    {
      int const slot = -id - 1;
      if (slot < 0 || slot >= 4 || preview->filenames[slot].empty())
      {
        return dm;
      }

      std::string path = "world/nodxt/detail/" + preview->filenames[slot];
      dm.index = static_cast<int>(out.models.size());
      out.models.emplace_back(path, context);
      return dm;
    }
    if (!gGroundEffectDoodadDB.CheckIfIdExists(id))
    {
      return dm;
    }

    DBCFile::Record record = gGroundEffectDoodadDB.getByID(id);

    std::string path = "world/nodxt/detail/";
    path += record.getString(GroundEffectDoodadDB::Filename);
    if (path.size() > 4)
    {
      std::string ext = path.substr(path.size() - 4);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".mdx" || ext == ".mdl")
      {
        path.replace(path.size() - 4, 4, ".m2");
      }
    }

    dm.align = record.getUInt(GroundEffectDoodadDB::Flags) & 1;
    dm.index = static_cast<int>(out.models.size());
    out.models.emplace_back(path, context);
    return dm;
  };

  // pass 2: per pick, masks -> layer -> effect record -> spawn attempts
  for (int i = 0; i < D; ++i)
  {
    int const col = pick_col[i];
    int const row = pick_row[i];

    if ((stencil[row] >> col) & 1) // bit set = doodads disabled on this unit
    {
      continue;
    }
    if (chunk->holes & (1 << (4 * (row >> 1) + (col >> 1))))
    {
      continue;
    }

    unsigned int const layer = (mapping[row] >> (2 * col)) & 3;
    // like the client, no bound check against the layer count: unused entries
    // of the fixed layer array carry effect id 0 and get skipped below
    bool const use_preview = preview_effect.valid
      && layer < static_cast<unsigned int>(texture_set->num())
      && texture_set->filename(layer) == preview->texture;
    unsigned int const effect_id = texture_set->getEffectForLayer(layer);

    EffectData const& effect = use_preview ? preview_effect : resolve_effect(effect_id);
    if (!effect.valid)
    {
      continue;
    }

    for (std::uint32_t j = 0; j < effect.amount; ++j)
    {
      // both randoms are always consumed, even for attempts rejected below
      float const r1 = rnd.signedUnit();
      float const r2 = rnd.signedUnit();
      float const o_col = r1 * HALF_CELL + HALF_CELL; // [0, CELL] along noggit x
      float const o_row = r2 * HALF_CELL + HALF_CELL; // [0, CELL] along noggit z

      // the doodad pick is deterministic, not random
      std::int32_t const doodad_id = effect.tbl[(static_cast<std::uint8_t>(i) + static_cast<std::uint8_t>(j)) & 15];
      if (!doodad_id)
      {
        continue;
      }

      int const t = (o_col > o_row ? 1 : 0) + ((o_col + o_row > CELL) ? 2 : 0);
      int const cell = col + 8 * row;
      if (!facet_built[cell][t])
      {
        facets[cell][t] = build_facet(chunk, row, col, t);
        facet_built[cell][t] = true;
      }
      Facet const& f = facets[cell][t];

      if (f.c < 0.4f) // steeper than ~66 degrees
      {
        continue;
      }

      float const local_x = -(o_row + row * CELL);
      float const local_y = -(o_col + col * CELL);
      float const height = -((local_x * f.a + local_y * f.b + f.d) / f.c);

      float const rot = (rnd.signedUnit() + 1.0f) * 3.1415927f; // [0, 2pi]
      float const scale = rnd.signedUnit() * 0.33f + 1.0f;      // [0.67, 1.33]

      // all randoms of this attempt are consumed; unresolvable ids drop here
      DoodadModel const& dm = resolve_doodad(doodad_id);
      if (dm.index < 0)
      {
        continue;
      }

      // MCCV over the facet: the same jitter randoms drive the barycentric
      // weights, so colour and position stay correlated like the client
      float bary_w, edge_t;
      if (std::fabs(r2) >= std::fabs(r1))
      {
        bary_w = std::fabs(r2);
        edge_t = 0.5f - r1 * 0.5f;
      }
      else
      {
        bary_w = std::fabs(r1);
        edge_t = 0.5f - r2 * 0.5f;
      }
      if (o_row - o_col < 0.0f)
      {
        edge_t = 1.0f - edge_t;
      }

      int const base = 17 * row + col;
      glm::vec3 const& cC = chunk->mccv[base + 9];
      glm::vec3 const& cA = chunk->mccv[base + VTX_A[t]];
      glm::vec3 const& cB = chunk->mccv[base + VTX_B[t]];
      glm::vec3 rgb = cC + bary_w * (cA - cC) + (bary_w * edge_t) * (cB - cA);
      rgb = glm::min(rgb, glm::vec3(1.0f));

      // MCSH: a shadowed doodad is darkened to 70% (lit is 65254/65536);
      // 1.92 maps yards to shadow texels, 64 texels / (8 * CELL) yards
      float shade = 0.99570f;
      int sx = std::clamp(static_cast<int>(std::floor((o_col + col * CELL) * 1.92f)), 0, 63);
      int sy = std::clamp(static_cast<int>(std::floor((o_row + row * CELL) * 1.92f)), 0, 63);
      if (chunk->_shadow_map[sy * 64 + sx])
      {
        shade = 0.70f;
      }
      rgb *= shade;

      std::uint32_t const color = (static_cast<std::uint32_t>(rgb.r * 255.f))
                                | (static_cast<std::uint32_t>(rgb.g * 255.f) << 8)
                                | (static_cast<std::uint32_t>(rgb.b * 255.f) << 16)
                                | 0xFF000000u;

      DetailDoodadPlacement placement;
      placement.model_index = static_cast<std::uint16_t>(dm.index);
      placement.terrain_align = dm.align;
      placement.pos = { chunk->xbase + (o_col + col * CELL)
                      , height
                      , chunk->zbase + (o_row + row * CELL) };
      placement.rot = rot;
      placement.scale = scale;
      // facet normal converted from client axes to noggit axes
      placement.normal = { -f.b, f.c, -f.a };
      placement.color = color;
      placement.facet_idx = static_cast<std::uint16_t>(4 * cell + t);

      out.placements.push_back(placement);
    }
  }
}
