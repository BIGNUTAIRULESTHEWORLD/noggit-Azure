// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "ModelRender.hpp"
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/Misc.h>
#include <noggit/Particle.h>
#include <noggit/TextureManager.h>

#include <math/bounding_box.hpp>
#include <math/frustum.hpp>

#include <opengl/shader.hpp>

#include <external/tracy/Tracy.hpp>


using namespace Noggit::Rendering;

ModelRender::ModelRender(Model* model)
: _model(model)
{

}

void ModelRender::upload()
{
  _vertex_box_points = math::box_points(
      misc::transform_model_box_coords(_model->bounding_box_min)
      , misc::transform_model_box_coords(_model->bounding_box_max));

  for (std::string const& texture : _model->_textureFilenames)
    _model->_textures.emplace_back(texture, _model->_context);

  _buffers.upload();
  _vertex_arrays.upload();
  _bone_matrices_buf_tex = 0;

  if (_model->animBones)
  {
    gl.genTextures(1, &_bone_matrices_buf_tex);

    gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
    OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const binder(_bone_matrices_buffer);
    gl.bufferData(GL_TEXTURE_BUFFER, _model->bone_matrices.size() * sizeof(glm::mat4x4), nullptr, GL_STREAM_DRAW);
    gl.texBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, _bone_matrices_buffer);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_vertices_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, _model->_vertices.size() * sizeof(ModelVertex), _model->_vertices.data(), GL_STATIC_DRAW);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_box_vbo);
    gl.bufferData (GL_ARRAY_BUFFER, _vertex_box_points.size() * sizeof (glm::vec3), _vertex_box_points.data(), GL_STATIC_DRAW);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);
  gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, _model->_indices.size() * sizeof(uint16_t), _model->_indices.data(), GL_STATIC_DRAW);

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> box_indices_binder(_box_indices_buffer);
  gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, _box_indices.size() * sizeof(uint16_t), _box_indices.data(), GL_STATIC_DRAW);

  _model->_textureFilenames.clear();

  _uploaded = true;
}

void ModelRender::unload()
{
  _model->_textures.clear();
  _buffers.unload();
  _vertex_arrays.unload();

  if (_bone_matrices_buf_tex)
    gl.deleteTextures(1, &_bone_matrices_buf_tex);

  for (auto& particle : _model->_particles)
  {
    particle.unload();
  }

  for (auto& ribbon : _model->_ribbons)
  {
    ribbon.unload();
  }

  _uploaded = false;
  _vao_setup = false;
  _single_instance_vao_setup = false;
}

void ModelRender::draw(glm::mat4x4 const& model_view
    , ModelInstance& instance
    , OpenGL::Scoped::use_program& m2_shader
    , OpenGL::M2RenderState& model_render_state
    , math::frustum const& frustum
    , const float& cull_distance
    , const glm::vec3& camera
    , int animtime
    , display_mode display
    , bool no_cull
    , bool animate
    , ModelAppearanceOverride const* appearance
    , bool force_animation
    , float opacity
    , bool force_alpha_blend
    , bool force_no_depth_write
)
{
  if (!_model->finishedLoading() || _model->loading_failed())
  {
    return;
  }

  if (!no_cull && !instance.isInFrustum(frustum) && !instance.isInRenderDist(cull_distance, camera, display))
  {
    return;
  }

  if (!_uploaded)
  {
    upload();
  }

  if (!_single_instance_vao_setup)
  {
    setupSingleInstanceVAO(m2_shader);
  }

  // Single-instance users such as skyboxes move with the camera. The transform
  // must be refreshed every draw, not only on the frame that uploads the VAO.
  m2_shader.uniform("transform", instance.transformMatrix());
  m2_shader.uniform("model_opacity", glm::clamp(opacity, 0.0f, 1.0f));

  if (_model->animated && (animate || _model->_per_instance_animation)
      && (force_animation || !_model->anim_calculated || _model->_per_instance_animation))
  {
    _model->animate(model_view * instance.transformMatrix(), 0,
                    animate || force_animation ? animtime : 0);
    _model->anim_calculated = true;
  }

  OpenGL::Scoped::vao_binder const vao(_single_instance_vao);

  if (_model->animBones)
  {
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
    m2_shader.uniform("anim_bones", true);
  }
  else
  {
    m2_shader.uniform("anim_bones", false);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);

  for (ModelRenderPass& p : _render_passes)
  {
    if (p.prepareDraw(m2_shader, _model, model_render_state, appearance,
                      force_alpha_blend, force_no_depth_write))
    {
      gl.drawElements(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(p.index_start * sizeof(GLushort)));
      p.afterDraw();
    }
  }

  gl.disable(GL_BLEND);
  gl.enable(GL_CULL_FACE);
  gl.depthMask(GL_TRUE);
}

void ModelRender::draw(glm::mat4x4 const& model_view
    , std::vector<glm::mat4x4> const& instances
    , OpenGL::Scoped::use_program& m2_shader
    , OpenGL::M2RenderState& model_render_state
    , math::frustum const& frustum
    , const float cull_distance
    , glm::vec3 const& camera
    , int animtime
    , bool all_boxes
    , std::unordered_map<Model*, std::size_t>& model_boxes_to_draw
    , display_mode display
    , bool no_cull
    , bool animate
    , bool draw_fake_geometry_box
    , bool draw_animation_box
    , ModelAppearanceOverride const* appearance
    , int animation_id
    , bool force_animation
    , int blend_from_animation_id
    , int blend_from_animation_time
    , float animation_blend
)
{
  ZoneScopedN(NOGGIT_CURRENT_FUNCTION);

  {
    ZoneScopedN("Model::draw() : uploads")

    if (!_model->finishedLoading() || _model->loading_failed())
    {
      return;
    }

    if (!_uploaded)
    {
      upload();
    }

    if (!_vao_setup)
    {
      setupVAO(m2_shader);
    }
  }

  if (instances.empty())
  {
    return;
  }

  {
    ZoneScopedN("Model::draw() : drawing")

    m2_shader.uniform("model_opacity", 1.0f);
    m2_shader.uniform("bones_per_instance", 0);

    auto animate_model = [&](glm::mat4x4 const& animation_view)
    {
      int const selected_animation = _model->_animations_seq_per_id.contains(animation_id)
        ? animation_id : 0;
      if (blend_from_animation_id >= 0 && animation_blend < 1.0f)
      {
        int const selected_blend_animation =
          _model->_animations_seq_per_id.contains(blend_from_animation_id)
            ? blend_from_animation_id : 0;
        _model->animateBlended(animation_view, selected_blend_animation,
          blend_from_animation_time, selected_animation, animtime, animation_blend);
      }
      else
      {
        _model->animate(animation_view, selected_animation,
                        animate || force_animation ? animtime : 0);
      }
      _model->anim_calculated = true;
    };

    bool const per_instance_billboards = _model->animated && _model->_per_instance_animation;
    if (_model->animated && animate && !per_instance_billboards
        && (force_animation || !_model->anim_calculated))
      animate_model(model_view);

    // store the model count to draw the bounding boxes later
    if (all_boxes || _model->_hidden ) 
    {
      model_boxes_to_draw.emplace(_model, instances.size());

      if (draw_animation_box)
      {
      }

    }
    else if (draw_fake_geometry_box && _model->use_fake_geometry() /*|| _model->particles_only()*/)
    { // hackfix for rendering particle only objects bounds as they currently don't render
      model_boxes_to_draw.emplace(_model, instances.size());
    }

    /*
    if (draw_particles && (!_particles.empty() || !_ribbons.empty()))
    {
      models_with_particles.emplace(this, n_visible_instances);
    }
     */

    OpenGL::Scoped::vao_binder const _ (_vao);

    if (_model->animBones)
    {
      gl.activeTexture(GL_TEXTURE0);
      gl.bindTexture(GL_TEXTURE_BUFFER, _bone_matrices_buf_tex);
      m2_shader.uniform("anim_bones", true);
    }
    else
    {
      m2_shader.uniform("anim_bones", false);
    }

    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_indices_buffer);

    if (per_instance_billboards && _model->animBones
        && (blend_from_animation_id < 0 || animation_blend >= 1.0f))
    {
      // Evaluate each placement's billboard bones, then upload the matrices
      // together. The vertex shader selects its own instance's matrices.
      // This keeps the orientation correct without one buffer update and
      // one draw per placement.
      std::vector<glm::mat4x4> instance_bones;
      std::size_t const bones_per_instance = _model->bone_matrices.size();
      instance_bones.reserve(instances.size() * bones_per_instance);
      int const selected_animation = _model->_animations_seq_per_id.contains(animation_id)
        ? animation_id : 0;
      for (auto const& transform : instances)
      {
        _model->animate(model_view * transform, selected_animation,
                        animate || force_animation ? animtime : 0, false);
        instance_bones.insert(instance_bones.end(), _model->bone_matrices.begin(),
                              _model->bone_matrices.end());
      }
      _model->anim_calculated = true;
      {
        OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const bone_binder(_bone_matrices_buffer);
        gl.bufferData(GL_TEXTURE_BUFFER, instance_bones.size() * sizeof(glm::mat4x4),
                      instance_bones.data(), GL_STREAM_DRAW);
      }
      m2_shader.uniform("bones_per_instance", static_cast<int>(bones_per_instance));
      {
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_transform_buffer);
        gl.bufferData(GL_ARRAY_BUFFER, instances.size() * sizeof(glm::mat4x4),
                      instances.data(), GL_DYNAMIC_DRAW);
      }
      for (ModelRenderPass& p : _render_passes)
        if (p.prepareDraw(m2_shader, _model, model_render_state, appearance))
          gl.drawElementsInstanced(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT,
            reinterpret_cast<void*>(p.index_start * sizeof(GLushort)), static_cast<GLsizei>(instances.size()));
    }
    else if (per_instance_billboards)
    {
      // Keep the existing path for blended preview animations, where every
      // placement may have a different animation state.
      for (auto const& transform : instances)
      {
        animate_model(model_view * transform);
        {
          OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_transform_buffer);
          gl.bufferData(GL_ARRAY_BUFFER, sizeof(transform), &transform, GL_DYNAMIC_DRAW);
        }
        for (ModelRenderPass& p : _render_passes)
          if (p.prepareDraw(m2_shader, _model, model_render_state, appearance))
            gl.drawElementsInstanced(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT,
              reinterpret_cast<void*>(p.index_start * sizeof(GLushort)), 1);
      }
    }
    else
    {
      {
        OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder(_transform_buffer);
        gl.bufferData(GL_ARRAY_BUFFER, instances.size() * sizeof(glm::mat4x4), instances.data(), GL_DYNAMIC_DRAW);
      }
      for (ModelRenderPass& p : _render_passes)
        if (p.prepareDraw(m2_shader, _model, model_render_state, appearance))
          gl.drawElementsInstanced(GL_TRIANGLES, p.index_count, GL_UNSIGNED_SHORT,
            reinterpret_cast<void*>(p.index_start * sizeof(GLushort)), static_cast<GLsizei>(instances.size()));
    }
  }

}

void ModelRender::drawParticles(glm::mat4x4 const& model_view
    , OpenGL::Scoped::use_program& particles_shader
    , std::size_t instance_count
)
{
  for (auto& p : _model->_particles)
  {
    p.draw(model_view, particles_shader, _transform_buffer, static_cast<int>(instance_count));
  }
}

void ModelRender::drawRibbons( OpenGL::Scoped::use_program& ribbons_shader
    , std::size_t instance_count
)
{
  for (auto& r : _model->_ribbons)
  {
    r.draw(ribbons_shader, _transform_buffer, static_cast<int>(instance_count));
  }
}

void ModelRender::drawBox(OpenGL::Scoped::use_program& m2_box_shader, std::size_t box_count)
{

  OpenGL::Scoped::vao_binder const _ (_box_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
    m2_box_shader.attrib("transform", 0, 1);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_box_vbo);
    m2_box_shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);
  }

  OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> indices_binder(_box_indices_buffer);

  gl.drawElementsInstanced (GL_LINE_STRIP, static_cast<GLsizei>(_box_indices.size()), GL_UNSIGNED_SHORT, nullptr, static_cast<GLsizei>(box_count));
}

[[nodiscard]]
std::vector<ModelRenderPass> const& Noggit::Rendering::ModelRender::renderPasses() const
{
  return _render_passes;
}

void ModelRender::setupVAO(OpenGL::Scoped::use_program& m2_shader)
{
  OpenGL::Scoped::vao_binder const _(_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder (_vertices_buffer);
    m2_shader.attrib("pos",           3, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), 0);
    m2_shader.attribi("bones_weight", 4, GL_UNSIGNED_BYTE,   sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3)));
    m2_shader.attribi("bones_indices",4, GL_UNSIGNED_BYTE,  sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) + 4));
    m2_shader.attrib("normal",        3, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) + 8));
    m2_shader.attrib("texcoord1",     2, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) * 2 + 8));
    m2_shader.attrib("texcoord2",     2, GL_FLOAT, GL_FALSE, sizeof (ModelVertex), reinterpret_cast<void*> (sizeof (::glm::vec3) * 2 + 8 + sizeof(glm::vec2)));
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const transform_binder (_transform_buffer);
    gl.bufferData(GL_ARRAY_BUFFER, 10 * sizeof(::glm::mat4x4), nullptr, GL_DYNAMIC_DRAW);
    m2_shader.attrib("transform", 0, 1);
  }

  _vao_setup = true;
}

void ModelRender::setupSingleInstanceVAO(OpenGL::Scoped::use_program& m2_shader)
{
  OpenGL::Scoped::vao_binder const vao(_single_instance_vao);

  OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_vertices_buffer);
  m2_shader.attrib("pos",            3, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), 0);
  m2_shader.attribi("bones_weight",  4, GL_UNSIGNED_BYTE,   sizeof(ModelVertex), reinterpret_cast<void*>(sizeof(glm::vec3)));
  m2_shader.attribi("bones_indices", 4, GL_UNSIGNED_BYTE,   sizeof(ModelVertex), reinterpret_cast<void*>(sizeof(glm::vec3) + 4));
  m2_shader.attrib("normal",         3, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(sizeof(glm::vec3) + 8));
  m2_shader.attrib("texcoord1",      2, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(sizeof(glm::vec3) * 2 + 8));
  m2_shader.attrib("texcoord2",      2, GL_FLOAT, GL_FALSE, sizeof(ModelVertex), reinterpret_cast<void*>(sizeof(glm::vec3) * 2 + 8 + sizeof(glm::vec2)));

  _single_instance_vao_setup = true;
}


void ModelRender::fixShaderIdBlendOverride()
{
  for (auto& pass : _render_passes)
  {
    if (pass.shader_id & 0x8000)
    {
      continue;
    }

    int shader = 0;
    bool blend_mode_override = (_model->Flags & m2_flag_use_texture_combiner_combos);

    // fuckporting check
    if (pass.texture_coord_combo_index + pass.texture_count - 1 >= _model->_texture_unit_lookup.size())
    {
      LogDebug << "wrong texture coord combo index on fuckported model: " << _model->_file_key.stringRepr() << std::endl;
      // use default stuff
      pass.shader_id = 0;
      pass.texture_count = 1;

      continue;
    }

    if (!blend_mode_override)
    {
      uint16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index];

      if (_model->_render_flags[pass.renderflag_index].blend)
      {
        shader = 1;

        if (texture_unit_lookup == 0xFFFF)
        {
          shader |= 0x8;
        }
      }

      shader <<= 4;

      if (texture_unit_lookup == 1)
      {
        shader |= 0x4000;
      }
    }
    else
    {
      uint16_t runtime_shader_val[2] = { 0, 0 };

      for (int i = 0; i < pass.texture_count; ++i)
      {
        uint16_t override_blend = _model->blend_override[pass.shader_id + i];
        uint16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index + i];

        if (i == 0 && _model->_render_flags[pass.renderflag_index].blend == 0)
        {
          override_blend = 0;
        }

        runtime_shader_val[i] = override_blend;

        if (texture_unit_lookup == 0xFFFF)
        {
          runtime_shader_val[i] |= 0x8;
        }

        if (texture_unit_lookup == 1 && i + 1 == pass.texture_count)
        {
          shader |= 0x4000;
        }
      }

      shader |= (runtime_shader_val[1] & 0xFFFF) | ((runtime_shader_val[0] << 4) & 0xFFFF);
    }

    pass.shader_id = shader;
  }
}

void ModelRender::fixShaderIDLayer()
{
  int non_layered_count = 0;

  for (auto const& pass : _render_passes)
  {
    if (pass.material_layer <= 0)
    {
      non_layered_count++;
    }
  }

  if (non_layered_count < _render_passes.size())
  {
    std::vector<ModelRenderPass> passes;

    ModelRenderPass* first_pass = nullptr;
    bool need_reducing = false;
    uint16_t previous_render_flag = -1, some_flags = 0;

    for (auto& pass : _render_passes)
    {
      if (pass.renderflag_index == previous_render_flag)
      {
        need_reducing = true;
        continue;
      }

      previous_render_flag = pass.renderflag_index;

      uint8_t lower_bits = pass.shader_id & 0x7;

      if (pass.material_layer == 0)
      {
        if (pass.texture_count >= 1 && _model->_render_flags[pass.renderflag_index].blend == 0)
        {
          pass.shader_id &= 0xFF8F;
        }

        first_pass = &pass;
      }
      assert(first_pass);
      if (first_pass == nullptr)
        return;

      bool xor_unlit = ((_model->_render_flags[pass.renderflag_index].flags.unlit ^ _model->_render_flags[first_pass->renderflag_index].flags.unlit) & 1) == 0;

      if ((some_flags & 0xFF) == 1)
      {
        if ((_model->_render_flags[pass.renderflag_index].blend == 1 || _model->_render_flags[pass.renderflag_index].blend == 2)
            && pass.texture_count == 1
            && xor_unlit
            && pass.texture_combo_index == first_pass->texture_combo_index
            )
        {
          if (_model->_transparency_lookup[pass.transparency_combo_index] == _model->_transparency_lookup[first_pass->transparency_combo_index])
          {
            pass.shader_id = 0x8000;
            first_pass->shader_id = 0x8001;

            some_flags = (some_flags & 0xFF00) | 3;

            // current pass removed (not needed)
            continue;
          }
        }

        some_flags = (some_flags & 0xFF00);
      }

      int16_t texture_unit_lookup = _model->_texture_unit_lookup[pass.texture_coord_combo_index];

      if ((some_flags & 0xFF) < 2)
      {
        if ((_model->_render_flags[pass.renderflag_index].blend == 0) && (pass.texture_count == 2) && ((lower_bits == 4) || (lower_bits == 6)))
        {
          if (texture_unit_lookup == 0 && (_model->_texture_unit_lookup[pass.texture_coord_combo_index + 1] == -1))
          {
            some_flags = (some_flags & 0xFF00) | 1;
          }
        }
      }

      if ((some_flags >> 8) != 0)
      {
        if ((some_flags >> 8) == 1)
        {
          if (((_model->_render_flags[pass.renderflag_index].blend != 4) && (_model->_render_flags[pass.renderflag_index].blend != 6)) || (pass.texture_count != 1) || (texture_unit_lookup >= 0))
          {
            some_flags &= 0xFF00;
          }
          // tod
          else  if ((_model->_transparency_lookup.size() > pass.transparency_combo_index
          && _model->_transparency_lookup.size() > first_pass->transparency_combo_index)
          && _model->_transparency_lookup[pass.transparency_combo_index]
            == _model->_transparency_lookup[first_pass->transparency_combo_index])
          {
            pass.shader_id = 0x8000;
            first_pass->shader_id = _model->_render_flags[pass.renderflag_index].blend != 4 ? 0xE : 0x8002;

            some_flags = (some_flags & 0xFF) | (2 << 8);

            first_pass->texture_count = 2;

            first_pass->textures[1] = pass.texture_combo_index;
            first_pass->uv_animations[1] = pass.animation_combo_index;

            // current pass removed (merged with the previous one)
            continue;
          }
        }
        else
        {
          if ((some_flags >> 8) != 2)
          {
            continue;
          }

          if ( ((_model->_render_flags[pass.renderflag_index].blend != 2) && (_model->_render_flags[pass.renderflag_index].blend != 1))
               || (pass.texture_count != 1)
               || xor_unlit
               || ((pass.texture_combo_index & 0xff) != (first_pass->texture_combo_index & 0xff))
              )
          {
            some_flags &= 0xFF00;
          }
          else  if (_model->_transparency_lookup[pass.transparency_combo_index] == _model->_transparency_lookup[first_pass->transparency_combo_index])
          {
            // current pass ignored/removed
            pass.shader_id = 0x8000;
            first_pass->shader_id = ((first_pass->shader_id == 0x8002 ? 2 : 0) - 0x7FFF) & 0xFFFF;
            some_flags = (some_flags & 0xFF) | (3 << 8);
            continue;
          }
        }
        some_flags = (some_flags & 0xFF);
      }

      if ((_model->_render_flags[pass.renderflag_index].blend == 0) && (pass.texture_count == 1) && (texture_unit_lookup == 0))
      {
        some_flags = (some_flags & 0xFF) | (1 << 8);
      }

      // setup texture and anim lookup indices
      pass.textures[0] = pass.texture_combo_index;
      pass.textures[1] = pass.texture_count > 1 ? pass.texture_combo_index + 1 : 0;
      pass.uv_animations[0] = pass.animation_combo_index;
      pass.uv_animations[1] = pass.texture_count > 1 ? pass.animation_combo_index + 1 : 0;

      passes.push_back(pass);
    }

    if (need_reducing)
    {
      previous_render_flag = -1;
      for (int i = 0; i < passes.size(); ++i)
      {
        auto& pass = _render_passes[i];
        uint16_t renderflag_index = pass.renderflag_index;

        if (renderflag_index == previous_render_flag)
        {
          pass.shader_id = _render_passes[i - 1].shader_id;
          pass.texture_count = _render_passes[i - 1].texture_count;
          pass.texture_combo_index = _render_passes[i - 1].texture_combo_index;
          pass.texture_coord_combo_index = _render_passes[i - 1].texture_coord_combo_index;
        }
        else
        {
          previous_render_flag = renderflag_index;
        }
      }
    }

    _render_passes = passes;
  }
    // no layering, just setting some infos
  else
  {
    for (auto& pass : _render_passes)
    {
      pass.textures[0] = pass.texture_combo_index;
      pass.textures[1] = pass.texture_count > 1 ? pass.texture_combo_index + 1 : 0;
      pass.uv_animations[0] = pass.animation_combo_index;
      pass.uv_animations[1] = pass.texture_count > 1 ? pass.animation_combo_index + 1 : 0;
    }
  }
}

namespace
{

// https://wowdev.wiki/M2/.skin/WotLK_shader_selection
  std::optional<ModelPixelShader> GetPixelShader(uint16_t texture_count, uint16_t shader_id)
  {
    uint16_t texture1_fragment_mode = (shader_id >> 4) & 7;
    uint16_t texture2_fragment_mode = shader_id & 7;
    // uint16_t texture1_env_map = (shader_id >> 4) & 8;
    // uint16_t texture2_env_map = shader_id & 8;

    std::optional<ModelPixelShader> pixel_shader;

    if (texture_count == 1)
    {
      switch (texture1_fragment_mode)
      {
        case 0:
          pixel_shader = ModelPixelShader::Combiners_Opaque;
          break;
        case 2:
          pixel_shader = ModelPixelShader::Combiners_Decal;
          break;
        case 3:
          pixel_shader = ModelPixelShader::Combiners_Add;
          break;
        case 4:
          pixel_shader = ModelPixelShader::Combiners_Mod2x;
          break;
        case 5:
          pixel_shader = ModelPixelShader::Combiners_Fade;
          break;
        default:
          pixel_shader = ModelPixelShader::Combiners_Mod;
          break;
      }
    }
    else
    {
      if (!texture1_fragment_mode)
      {
        switch (texture2_fragment_mode)
        {
          case 0:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Opaque;
            break;
          case 3:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Add;
            break;
          case 4:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2x;
            break;
          case 6:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2xNA;
            break;
          case 7:
            pixel_shader = ModelPixelShader::Combiners_Opaque_AddNA;
            break;
          default:
            pixel_shader = ModelPixelShader::Combiners_Opaque_Mod;
            break;
        }
      }
      else if (texture1_fragment_mode == 1)
      {
        switch (texture2_fragment_mode)
        {
          case 0:
            pixel_shader = ModelPixelShader::Combiners_Mod_Opaque;
            break;
          case 3:
            pixel_shader = ModelPixelShader::Combiners_Mod_Add;
            break;
          case 4:
            pixel_shader = ModelPixelShader::Combiners_Mod_Mod2x;
            break;
          case 6:
            pixel_shader = ModelPixelShader::Combiners_Mod_Mod2xNA;
            break;
          case 7:
            pixel_shader = ModelPixelShader::Combiners_Mod_AddNA;
            break;
          default:
            pixel_shader = ModelPixelShader::Combiners_Mod_Mod;
            break;
        }
      }
      else if (texture1_fragment_mode == 3)
      {
        if (texture2_fragment_mode == 1)
        {
          pixel_shader = ModelPixelShader::Combiners_Add_Mod;
        }
      }
      else if (texture1_fragment_mode == 4 && texture2_fragment_mode == 4)
      {
        pixel_shader = ModelPixelShader::Combiners_Mod2x_Mod2x;
      }
      else if (texture2_fragment_mode == 1)
      {
        pixel_shader = ModelPixelShader::Combiners_Mod_Mod2x;
      }
    }


    return pixel_shader;
  }

  std::optional<ModelPixelShader> M2GetPixelShaderID (uint16_t texture_count, uint16_t shader_id)
  {
    std::optional<ModelPixelShader> pixel_shader;

    if (!(shader_id & 0x8000))
    {
      pixel_shader = GetPixelShader(texture_count, shader_id);

      if (!pixel_shader)
      {
        pixel_shader = GetPixelShader(texture_count, 0x11);
      }
    }
    else
    {
      switch (shader_id & 0x7FFF)
      {
        case 1:
          pixel_shader = ModelPixelShader::Combiners_Opaque_Mod2xNA_Alpha;
          break;
        case 2:
          pixel_shader = ModelPixelShader::Combiners_Opaque_AddAlpha;
          break;
        case 3:
          pixel_shader = ModelPixelShader::Combiners_Opaque_AddAlpha_Alpha;
          break;
      }
    }

    return pixel_shader;
  }
}

void ModelRender::computePixelShaderIDs()
{
  for (auto& pass : _render_passes)
  {
    pass.pixel_shader = M2GetPixelShaderID(pass.texture_count, pass.shader_id);
  }
}

void ModelRender::initRenderPasses(ModelView const* view, ModelTexUnit const* tex_unit, ModelGeoset const* model_geosets)
{
  _render_passes.reserve(view->n_texture_unit);
  for (size_t j = 0; j<view->n_texture_unit; j++)
  {
    size_t geoset = tex_unit[j].submesh;

    ModelRenderPass pass(tex_unit[j], _model);
    pass.ordering_thingy = model_geosets[geoset].BoundingBox[0].x;

    pass.index_start = model_geosets[geoset].istart;
    pass.index_count = model_geosets[geoset].icount;
    pass.vertex_start = model_geosets[geoset].vstart;
    pass.vertex_end = pass.vertex_start + model_geosets[geoset].vcount;

    _render_passes.push_back(std::move(pass));
  }


  fixShaderIdBlendOverride();
  fixShaderIDLayer();
  computePixelShaderIDs();


  for (auto& pass : _render_passes)
  {
    pass.initUVTypes(_model);
  }

  // transparent parts come later
  std::sort(_render_passes.begin(), _render_passes.end());
}

void ModelRender::updateBoneMatrices()
{
  {
    OpenGL::Scoped::buffer_binder<GL_TEXTURE_BUFFER> const binder (_bone_matrices_buffer);
    gl.bufferSubData(GL_TEXTURE_BUFFER, 0, _model->bone_matrices.size() * sizeof(glm::mat4x4), _model->bone_matrices.data());
  }
}

// ModelRenderPass


ModelRenderPass::ModelRenderPass(ModelTexUnit const& tex_unit, Model* m)
    : ModelTexUnit(tex_unit)
    , blend_mode(m->_render_flags[renderflag_index].blend)
{
}

bool ModelRenderPass::prepareDraw(OpenGL::Scoped::use_program& m2_shader, Model *m,
                                  OpenGL::M2RenderState& model_render_state,
                                  ModelAppearanceOverride const* appearance,
                                  bool force_alpha_blend,
                                  bool force_no_depth_write)
{
  auto const& visible_geosets = appearance && appearance->geosets
    ? *appearance->geosets : m->showGeosets;
  if (invalid_texture_binding || submesh >= visible_geosets.size()
      || !visible_geosets[submesh] || !pixel_shader)
  {
    return false;
  }

  // COLOUR
  // Get the colour and transparency and check that we should even render
  glm::vec4 mesh_color = glm::vec4(1.0f, 1.0f, 1.0f, m->trans); // ??
  glm::vec4 emissive_color = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);

  auto const& renderflag(m->_render_flags[renderflag_index]);

  // emissive colors
  if (color_index != -1 && m->_colors[color_index].color.uses(0))
  {
    ::glm::vec3 c (m->_colors[color_index].color.getValue (0, m->_anim_time, m->_global_animtime));
    if (m->_colors[color_index].opacity.uses (m->_current_anim_seq))
    {
      mesh_color.w = m->_colors[color_index].opacity.getValue (m->_current_anim_seq, m->_anim_time, m->_global_animtime);
    }

    mesh_color.x = c.x; mesh_color.y = c.y; mesh_color.z = c.z;

    emissive_color = glm::vec4(c.x,c.y,c.z, mesh_color.w);
  }

  // opacity
  if (transparency_combo_index != 0xFFFF && transparency_combo_index < m->_transparency_lookup.size())
  {
    auto& transparency (m->_transparency[m->_transparency_lookup[transparency_combo_index]].trans);
    if (transparency.uses (0))
    {
      mesh_color.w = mesh_color.w * transparency.getValue(0, m->_anim_time, m->_global_animtime);
    }
  }

  // exit and return false before affecting the opengl render state
  if (!((mesh_color.w > 0) && (color_index == -1 || emissive_color.w > 0)))
  {
    return false;
  }


  M2Blend const source_blend = static_cast<M2Blend>(renderflag.blend);
  M2Blend const effective_blend = force_alpha_blend
      && (source_blend == M2Blend::Opaque || source_blend == M2Blend::Alpha_Key)
    ? M2Blend::Alpha : source_blend;

  if (model_render_state.blend != static_cast<std::uint16_t>(effective_blend))
  {
    switch (effective_blend)
    {
      default:
      case M2Blend::Opaque:
      case M2Blend::Alpha_Key:
        gl.disable(GL_BLEND);
        break;
      case M2Blend::Alpha:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
      case M2Blend::No_Add_Alpha:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_ONE, GL_ONE);
        break;
      case M2Blend::Add:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
      case M2Blend::Mod:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_ZERO);
        break;
      case M2Blend::Mod2x:
        gl.enable(GL_BLEND);
        gl.blendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        break;
    }

    model_render_state.blend = static_cast<std::uint16_t>(effective_blend);
  }
  // Keep shader material semantics tied to the source blend mode even when an
  // opaque skybox pass temporarily uses alpha blending for a transition.
  m2_shader.uniform("blend_mode", static_cast<int>(source_blend));

  if (model_render_state.backface_cull != !renderflag.flags.two_sided)
  {
    if (renderflag.flags.two_sided)
    {
      gl.disable(GL_CULL_FACE);
    }
    else
    {
      gl.enable(GL_CULL_FACE);
    }

    model_render_state.backface_cull = !renderflag.flags.two_sided;
  }

  if (model_render_state.z_buffered != renderflag.flags.z_buffered)
  {
    if (renderflag.flags.z_buffered)
    {
      gl.depthMask(GL_FALSE);
    }
    else
    {
      gl.depthMask(GL_TRUE);
    }

    model_render_state.z_buffered = renderflag.flags.z_buffered;
  }

  if (force_no_depth_write)
  {
    gl.depthMask(GL_FALSE);
  }

  if (model_render_state.unfogged != renderflag.flags.unfogged)
  {
    m2_shader.uniform("unfogged", (int)renderflag.flags.unfogged);
    model_render_state.unfogged = renderflag.flags.unfogged;
  }

  if (model_render_state.unlit != renderflag.flags.unlit)
  {
    m2_shader.uniform("unlit", (int)renderflag.flags.unlit);
    model_render_state.unlit = renderflag.flags.unlit;
  }

  if (texture_count > 1)
  {
    if (!bindTexture(1, m, model_render_state, m2_shader, appearance))
      return false;
  }

  if (!bindTexture(0, m, model_render_state, m2_shader, appearance))
    return false;

  GLint tu1 = static_cast<GLint>(tu_lookups[0]), tu2 = static_cast<GLint>(tu_lookups[1]);

  if (model_render_state.tex_unit_lookups[0] != tu1)
  {
    m2_shader.uniform("tex_unit_lookup_1", tu1);
    model_render_state.tex_unit_lookups[0] = tu1;
  }

  if (model_render_state.tex_unit_lookups[1] != tu2)
  {
    m2_shader.uniform("tex_unit_lookup_2", tu2);
    model_render_state.tex_unit_lookups[1] = tu2;
  }

  int16_t tex_anim_lookup = m->_texture_animation_lookups[uv_animations[0]];
  static const glm::mat4x4 unit(glm::mat4x4(1));

  if (tex_anim_lookup != -1)
  {
    m2_shader.uniform("tex_matrix_1", m->_texture_animations[tex_anim_lookup].mat);
    if (texture_count > 1)
    {
      tex_anim_lookup = m->_texture_animation_lookups[uv_animations[1]];
      if (tex_anim_lookup != -1)
      {
        m2_shader.uniform("tex_matrix_2", m->_texture_animations[tex_anim_lookup].mat);
      }
      else
      {
        m2_shader.uniform("tex_matrix_2", unit);
      }
    }
  }
  else
  {
    m2_shader.uniform("tex_matrix_1", unit);
    m2_shader.uniform("tex_matrix_2", unit);
  }


  GLint ps = static_cast<GLint>(pixel_shader.value());
  if (model_render_state.pixel_shader != ps)
  {
    m2_shader.uniform("pixel_shader", ps);
    model_render_state.pixel_shader = ps;
  }

  m2_shader.uniform("mesh_color", mesh_color);

  return true;
}

void ModelRenderPass::afterDraw()
{
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

bool ModelRenderPass::bindTexture(size_t index, Model* m,
                                  OpenGL::M2RenderState& model_render_state,
                                  OpenGL::Scoped::use_program& m2_shader,
                                  ModelAppearanceOverride const* appearance)
{
  auto fail = [&](char const* reason)
  {
    if (!invalid_texture_binding)
    {
      LogError << "model: disabling render pass with invalid texture binding ("
               << reason << ") " << m->file_key().stringRepr() << std::endl;
    }
    invalid_texture_binding = true;
    return false;
  };

  if (invalid_texture_binding)
    return false;
  if (index >= 2)
    return fail("texture slot out of range");

  std::size_t const lookup_index = textures[index];
  if (lookup_index >= m->_texture_lookup.size())
    return fail("texture lookup out of range");

  uint16_t const tex = m->_texture_lookup[lookup_index];
  if (tex >= m->_specialTextures.size())
    return fail("texture index out of range");

  int special_texture = m->_specialTextures[tex];
  if (appearance && appearance->replacement_textures
      && tex < m->_replaceableTextureTypes.size())
  {
    int const original_type = m->_replaceableTextureTypes[tex];
    if (original_type >= 0
        && appearance->replacement_textures->contains(static_cast<std::size_t>(original_type)))
      special_texture = original_type;
  }

  if (special_texture == -1)
  {
    if (tex >= m->_textures.size())
      return fail("texture reference out of range");

    auto& texture = m->_textures[tex];
    texture->upload();
    GLuint tex_array = texture->texture_array();
    int tex_index = texture->array_index();

    gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + index + 1));
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex_array);
    /*
    if (model_render_state.tex_arrays[index] != tex_array)
    {
      gl.activeTexture(GL_TEXTURE0 + index + 1);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex_array);
      model_render_state.tex_arrays[index] = tex_array;
    }


    if (model_render_state.tex_indices[index] != tex_index)
    {
      m2_shader.uniform(index ? "tex2_index" : "tex1_index", tex_index);
      model_render_state.tex_indices[index] = tex_index;
    }

          */

    m2_shader.uniform(index ? "tex2_index" : "tex1_index", tex_index);
    model_render_state.tex_indices[index] = tex_index;

    return true;
  }
  else
  {
    if (special_texture < 0)
      return fail("special texture index is invalid");

    auto const& replacements = appearance && appearance->replacement_textures
      ? *appearance->replacement_textures : m->_replaceTextures;
    auto const replacement = replacements.find(static_cast<std::size_t>(special_texture));
    if (replacement == replacements.end())
      return fail("special texture is unavailable");

    auto& texture = replacement->second;
    texture->upload();
    GLuint tex_array = texture->texture_array();
    int tex_index = texture->array_index();

    gl.activeTexture(static_cast<GLenum>(GL_TEXTURE0 + index + 1));
    gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex_array);

    /*
    if (model_render_state.tex_arrays[index] != tex_array)
    {
      gl.activeTexture(GL_TEXTURE0 + index + 1);
      gl.bindTexture(GL_TEXTURE_2D_ARRAY, tex_array);
      model_render_state.tex_arrays[index] = tex_array;
    }

    if (model_render_state.tex_indices[index] != tex_index)
    {
      m2_shader.uniform(index ? "tex2_index" : "tex1_index", tex_index);
      model_render_state.tex_indices[index] = tex_index;
    }

          */

    m2_shader.uniform(index ? "tex2_index" : "tex1_index", tex_index);
    model_render_state.tex_indices[index] = tex_index;

    return true;
  }
}

void ModelRenderPass::initUVTypes(Model* m)
{
  tu_lookups[0] = texture_unit_lookup::none;
  tu_lookups[1] = texture_unit_lookup::none;

  if (m->_texture_unit_lookup.size() < texture_coord_combo_index + texture_count)
  {
    LogError << "model: texture_coord_combo_index out of range " << m->file_key().stringRepr() << std::endl;

    for (int i = 0; i < texture_count; ++i)
    {
      switch (i)
      {
        case 0: tu_lookups[i] = texture_unit_lookup::t1; break;
        case 1: tu_lookups[i] = texture_unit_lookup::t2; break;
      }
    }

    return;

    //throw std::out_of_range("model: texture_coord_combo_index out of range " + m->filename);
  }

  for (int i = 0; i < texture_count; ++i)
  {
    switch (m->_texture_unit_lookup[texture_coord_combo_index + i])
    {
      case (int16_t)(-1): tu_lookups[i] = texture_unit_lookup::environment; break;
      case 0: tu_lookups[i] = texture_unit_lookup::t1; break;
      case 1: tu_lookups[i] = texture_unit_lookup::t2; break;
    }
  }
}

bool ModelRenderPass::operator< (const ModelRenderPass& m) const
{
  if (priority_plane < m.priority_plane)
  {
    return true;
  }
  else if (priority_plane > m.priority_plane)
  {
    return false;
  }
  else
  {
    return blend_mode == m.blend_mode ? (ordering_thingy < m.ordering_thingy) : blend_mode < m.blend_mode;
  }
}
