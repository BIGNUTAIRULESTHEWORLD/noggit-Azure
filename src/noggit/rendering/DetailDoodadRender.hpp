// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/DetailDoodads.hpp>

#include <opengl/scoped.hpp>

#include <unordered_map>
#include <vector>

class MapChunk;
class Model;

namespace Noggit::Rendering
{
  // Client-style detail doodad rendering: one merged static buffer per chunk
  // with rotation/scale baked in, the terrain facet normal in the normal slot
  // and MCCV/MCSH in the vertex colour, batched per texture like
  // CDetailDoodad's 4-batch model (without the 4-texture cap).
  class DetailDoodadRender
  {
  public:
    // builds/rebuilds the chunk's batches when its placements changed, then
    // draws them; a chunk whose models are still loading draws next frame
    void drawChunk(OpenGL::Scoped::use_program& shader, MapChunk* chunk, ChunkDetailDoodads* cache, int frame);

    // deletes buffers of chunks that stopped being drawn
    void endFrame(int frame);

    void unload();

  private:
    struct Batch
    {
      Model* model; // texture lookup at draw; guarded by the revision check
      unsigned int texture_id;
      int index_count;
      std::size_t index_offset_bytes;
    };

    struct ChunkGL
    {
      std::uint32_t revision = 0xFFFFFFFF;
      bool ready = false;
      unsigned int vao = 0;
      unsigned int vbo = 0;
      unsigned int ibo = 0;
      std::vector<Batch> batches;
      int last_frame = 0;
    };

    void deleteBuffers(ChunkGL& gl_data);
    bool build(ChunkGL& gl_data, ChunkDetailDoodads* cache);

    std::unordered_map<MapChunk*, ChunkGL> _chunks;
  };
}
