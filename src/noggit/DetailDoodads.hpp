// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/ContextObject.hpp>
#include <noggit/ModelManager.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <array>
#include <string>
#include <vector>

class MapChunk;

namespace Noggit
{
  struct DetailDoodadPreview
  {
    bool enabled = false;
    std::string texture;
    std::array<std::string, 4> filenames;
    std::array<std::int32_t, 4> weights{};
    std::uint32_t amount = 0;
  };

  struct DetailDoodadPlacement
  {
    std::uint16_t model_index;
    bool terrain_align;   // GroundEffectDoodad flag 0x1
    glm::vec3 pos;        // world space
    float rot;            // [0, 2pi]
    float scale;          // [0.67, 1.33]
    glm::vec3 normal;     // terrain facet normal, written to every vertex
    std::uint32_t color;  // RGBA8: MCCV with the MCSH shadow term baked in
    std::uint16_t facet_idx; // 4 * cell + sub_tri; keys the client's per-batch rotation matrix cache
  };

  // Client-matching ground effect doodad placements for one chunk, cached and
  // regenerated when the chunk is edited, the ground effect DBCs change or the
  // density setting moves.
  struct ChunkDetailDoodads
  {
    std::uint32_t chunk_stamp = 0;
    std::uint32_t dbc_stamp = 0;
    int density = -1;
    std::uint32_t revision = 0; // globally unique per regeneration; keys the GL batch cache
    std::vector<scoped_model_reference> models;
    std::vector<DetailDoodadPlacement> placements;
  };

  namespace DetailDoodads
  {
    // bumped when ground effect DBC records are edited so cached placements rebuild
    std::uint32_t dbcStamp();
    void bumpDbcStamp();

    // Reproduces CMapChunk::CreateDetailDoodads (Wow.exe 12340 @ 0x7D3390):
    // per-chunk deterministic seed, cell picks, stride-13 weighted doodad
    // table, the jitter/slope/plane-snap math and the MCCV/MCSH colour terms,
    // so the editor shows the same layout the client generates from the saved
    // data.
    void generate(MapChunk* chunk, int density, NoggitRenderContext context,
                  ChunkDetailDoodads& out, DetailDoodadPreview const* preview = nullptr);
  }
}
