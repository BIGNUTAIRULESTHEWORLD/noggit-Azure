// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/DetailDoodadRender.hpp>

#include <noggit/MapChunk.h>
#include <noggit/Model.h>
#include <noggit/rendering/ModelRender.hpp>
#include <noggit/TextureManager.h>

#include <opengl/context.hpp>
#include <opengl/context.inl>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace Noggit::Rendering
{
  namespace
  {
    struct DDVertex
    {
      glm::vec3 pos;
      glm::vec3 normal;
      std::uint32_t color;
      glm::vec2 uv;
    };

    // the client computes doodad rotations on its own axes; conjugate its
    // matrices into noggit space instead of re-deriving them, so the math
    // below can stay a verbatim port of the binary.
    // client world direction -> noggit world direction: (x, y, z) -> (-y, z, -x)
    glm::mat3 const client_to_noggit{ { 0.f, 0.f, -1.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
    // noggit model space -> client model space, undoes Model.cpp's
    // fixCoordSystem (x, y, z) -> (x, z, -y) applied to the loaded vertices
    glm::mat3 const noggit_model_to_client{ { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, -1.f, 0.f } };
  }

  void DetailDoodadRender::deleteBuffers(ChunkGL& gl_data)
  {
    if (gl_data.vao)
    {
      gl.deleteVertexArray(1, &gl_data.vao);
      gl.deleteBuffers(1, &gl_data.vbo);
      gl.deleteBuffers(1, &gl_data.ibo);
      gl_data.vao = gl_data.vbo = gl_data.ibo = 0;
    }
    gl_data.batches.clear();
    gl_data.ready = false;
  }

  bool DetailDoodadRender::build(ChunkGL& gl_data, ChunkDetailDoodads* cache)
  {
    // wait until every referenced model settled; failed ones drop out
    for (auto const& model : cache->models)
    {
      if (!model->finishedLoading() && !model->loading_failed())
      {
        return false;
      }
    }

    struct ModelGeo
    {
      bool ok = false;
      Model* model = nullptr;
      unsigned int texture_id = 0;
      std::uint16_t vertex_start = 0;
      std::uint16_t vertex_count = 0;
      std::uint16_t index_start = 0;
      std::uint16_t index_count = 0;
    };

    std::vector<ModelGeo> geo(cache->models.size());
    for (std::size_t i = 0; i < cache->models.size(); ++i)
    {
      Model* model = cache->models[i].get();

      if (model->loading_failed()
        || model->skin_load_failed()
        || model->renderer()->renderPasses().empty()
        || model->vertexData().empty())
      {
        continue;
      }

      // detail doodads never go through the regular M2 draw path, so the
      // lazy first-draw upload that fills Model::_textures hasn't happened
      if (!model->renderer()->uploaded())
      {
        model->renderer()->upload();
      }

      auto const& passes = model->renderer()->renderPasses();

      // the client only draws submesh 0 with the first batch's texture
      auto const pass = std::min_element(passes.begin(), passes.end()
        , [](auto const& a, auto const& b) { return a.index_start < b.index_start; });

      if (!pass->index_count || pass->textures[0] >= model->textureLookup().size())
      {
        continue;
      }

      // resolved like ModelRenderPass::bindTexture
      std::uint16_t const tex = model->textureLookup()[pass->textures[0]];
      if (tex >= model->textureRefs().size())
      {
        continue;
      }

      ModelGeo& g = geo[i];
      g.model = model;
      g.texture_id = tex;
      g.vertex_start = pass->vertex_start;
      g.vertex_count = pass->vertex_end - pass->vertex_start;
      g.index_start = pass->index_start;
      g.index_count = pass->index_count;
      g.ok = g.vertex_count && g.index_count;
    }

    // CDetailDoodadInst::AddDoodad (0x7B31E0): 4 texture-keyed batches per
    // chunk, each budgeted min(density * 64, 4096) verts/indices (strict <);
    // a doodad no batch can take is silently dropped, so a 5th distinct
    // texture on a chunk never renders
    struct SimBatch
    {
      void const* tex = nullptr;
      std::uint32_t vtx = 0;
      std::uint32_t idx = 0;
      std::size_t geo_index = 0;
      std::vector<std::uint32_t> members;
    };
    SimBatch sim[4];
    std::uint32_t const budget = static_cast<std::uint32_t>(
        std::min(std::clamp(cache->density, 16, 256) * 64, 4096));

    for (std::size_t pi = 0; pi < cache->placements.size(); ++pi)
    {
      ModelGeo const& g = geo[cache->placements[pi].model_index];
      if (!g.ok)
      {
        continue;
      }

      void const* key = g.model->textureRefs()[g.texture_id].get();

      int use = -1;
      for (int b = 0; b < 4; ++b)
      {
        if (sim[b].tex == key
          && sim[b].vtx + g.vertex_count < budget
          && sim[b].idx + g.index_count < budget)
        {
          use = b;
          break;
        }
      }
      if (use < 0)
      {
        for (int b = 0; b < 4; ++b)
        {
          if (!sim[b].tex)
          {
            sim[b].tex = key;
            sim[b].geo_index = cache->placements[pi].model_index;
            use = b;
            break;
          }
        }
      }
      if (use < 0)
      {
        continue;
      }

      sim[use].vtx += g.vertex_count;
      sim[use].idx += g.index_count;
      sim[use].members.push_back(static_cast<std::uint32_t>(pi));
    }

    std::vector<DDVertex> vertices;
    std::vector<std::uint32_t> indices;
    gl_data.batches.clear();

    for (SimBatch const& sb : sim)
    {
      if (sb.members.empty())
      {
        continue;
      }

      std::size_t const batch_index_offset = indices.size();

      // CDetailDoodad::BuildVertexBuffer (0x7B1B50) keeps 4 rotation
      // matrices per batch with a dirty mask keyed on facetIdx & 0xFC (cell)
      // and facetIdx & 3 (sub-triangle): in a run of aligned instances on
      // one cell only the first to hit each sub-triangle builds a matrix
      // from its own angle, the rest reuse it. A genuine client quirk that
      // identical facing requires; upright instances neither use nor
      // invalidate the cache.
      glm::mat3 cached_rot[4];
      std::uint16_t cached_cell = 0xFFFF;
      std::uint8_t cached_mask = 0;

      for (std::uint32_t pi : sb.members)
      {
        auto const& p = cache->placements[pi];
        ModelGeo const& g = geo[p.model_index];
        auto const& model_vertices = g.model->vertexData();
        auto const& model_indices = g.model->indexData();

        glm::mat3 rot3;
        if (p.terrain_align)
        {
          std::uint16_t const cell = p.facet_idx & 0xFC;
          int const slot = p.facet_idx & 3;
          if (cell != cached_cell)
          {
            cached_cell = cell;
            cached_mask = 0;
          }
          if (!(cached_mask & (1 << slot)))
          {
            // the client's deterministic terrain-aligned basis
            // (BuildRotationMatrix @0x7B1000) from the facet normal
            // n = (a,b,c) on client axes; rows 0/1 span the terrain plane
            // and get spun about the normal (row 2) by the instance angle
            float const a = -p.normal.z;
            float const b = -p.normal.x;
            float const c = p.normal.y;
            float const L = std::sqrt(b * b + c * c);
            float const iL = 1.0f / L;
            glm::vec3 const r0{ -L, a * b * iL, a * c * iL };
            glm::vec3 const r1{ 0.f, c * iL, -b * iL };
            glm::vec3 const r2{ a, b, c };
            float const cs = std::cos(p.rot);
            float const sn = std::sin(p.rot);
            // the client transform is v' = v.x*row0 + v.y*row1 + v.z*row2,
            // i.e. the rows as glm columns
            cached_rot[slot] = client_to_noggit
                             * glm::mat3(cs * r0 + sn * r1, cs * r1 - sn * r0, r2)
                             * noggit_model_to_client;
            cached_mask |= static_cast<std::uint8_t>(1 << slot);
          }
          rot3 = cached_rot[slot];
        }
        else
        {
          // upright: a plain spin about the client's up axis, per instance
          float const cs = std::cos(p.rot);
          float const sn = std::sin(p.rot);
          rot3 = client_to_noggit
               * glm::mat3(glm::vec3(cs, sn, 0.f), glm::vec3(-sn, cs, 0.f), glm::vec3(0.f, 0.f, 1.f))
               * noggit_model_to_client;
        }

        std::uint32_t const base_vertex = static_cast<std::uint32_t>(vertices.size());

        for (std::uint16_t v = 0; v < g.vertex_count; ++v)
        {
          auto const& mv = model_vertices[g.vertex_start + v];
          DDVertex out;
          out.pos = rot3 * mv.position * p.scale + p.pos;
          out.normal = p.normal;
          out.color = p.color;
          out.uv = mv.texcoords[0];
          vertices.push_back(out);
        }

        for (std::uint16_t idx = 0; idx < g.index_count; ++idx)
        {
          indices.push_back(base_vertex + (model_indices[g.index_start + idx] - g.vertex_start));
        }
      }

      std::size_t const count = indices.size() - batch_index_offset;
      if (count)
      {
        ModelGeo const& bg = geo[sb.geo_index];
        gl_data.batches.push_back({ bg.model, bg.texture_id
                                  , static_cast<int>(count)
                                  , batch_index_offset * sizeof(std::uint32_t) });
      }
    }

    if (!indices.empty())
    {
      if (!gl_data.vao)
      {
        gl.genVertexArrays(1, &gl_data.vao);
        gl.genBuffers(1, &gl_data.vbo);
        gl.genBuffers(1, &gl_data.ibo);
      }

      gl.bufferData<GL_ARRAY_BUFFER, DDVertex>(gl_data.vbo, vertices, GL_STATIC_DRAW);
      gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint32_t>(gl_data.ibo, indices, GL_STATIC_DRAW);
    }

    gl_data.revision = cache->revision;
    gl_data.ready = true;
    return true;
  }

  void DetailDoodadRender::drawChunk(OpenGL::Scoped::use_program& shader, MapChunk* chunk, ChunkDetailDoodads* cache, int frame)
  {
    ChunkGL& gl_data = _chunks[chunk];
    gl_data.last_frame = frame;

    if (gl_data.revision != cache->revision || !gl_data.ready)
    {
      if (!build(gl_data, cache))
      {
        return; // models still loading, retry next frame
      }
    }

    if (gl_data.batches.empty())
    {
      return;
    }

    OpenGL::Scoped::vao_binder const _ (gl_data.vao);

    {
      OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (gl_data.vbo);
      shader.attrib("position", 3, GL_FLOAT, GL_FALSE, sizeof(DDVertex), reinterpret_cast<void*>(offsetof(DDVertex, pos)));
      shader.attrib("normal", 3, GL_FLOAT, GL_FALSE, sizeof(DDVertex), reinterpret_cast<void*>(offsetof(DDVertex, normal)));
      shader.attrib("color", 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DDVertex), reinterpret_cast<void*>(offsetof(DDVertex, color)));
      shader.attrib("uv", 2, GL_FLOAT, GL_FALSE, sizeof(DDVertex), reinterpret_cast<void*>(offsetof(DDVertex, uv)));
    }

    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const indices_binder (gl_data.ibo);

    for (auto const& batch : gl_data.batches)
    {
      auto const& texture = batch.model->textureRefs()[batch.texture_id];
      texture->upload();
      gl.activeTexture(GL_TEXTURE0);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, texture->texture_array());
      shader.uniform("tex_index", texture->array_index());

      gl.drawElements(GL_TRIANGLES, batch.index_count, GL_UNSIGNED_INT
        , reinterpret_cast<void*>(batch.index_offset_bytes));
    }
  }

  void DetailDoodadRender::endFrame(int frame)
  {
    for (auto it = _chunks.begin(); it != _chunks.end();)
    {
      // dropped out of range or unloaded a while ago: free the buffers.
      // only GL handles are touched, the chunk pointer is never dereferenced
      if (frame - it->second.last_frame > 120 || frame < it->second.last_frame)
      {
        deleteBuffers(it->second);
        it = _chunks.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  void DetailDoodadRender::unload()
  {
    for (auto& pair : _chunks)
    {
      deleteBuffers(pair.second);
    }
    _chunks.clear();
  }
}
