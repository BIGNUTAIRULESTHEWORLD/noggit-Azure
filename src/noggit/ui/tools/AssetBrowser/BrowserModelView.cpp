#include "BrowserModelView.hpp"

#include <noggit/ContextObject.hpp>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/ModelManager.h>
#include <noggit/TextureManager.h>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/WMOInstance.h>

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QSettings>
#include <QSurface>
#include <QWheelEvent>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

using namespace Noggit::Ui::Tools::AssetBrowser;

namespace
{
  // Selection changes happen outside paintGL. Return to the editor's context
  // after loading a preview model so later editor work does not run in this widget.
  struct RestoreCurrentContext
  {
    explicit RestoreCurrentContext(bool enabled)
      : enabled(enabled)
      , previous(enabled ? QOpenGLContext::currentContext() : nullptr)
      , surface(previous ? previous->surface() : nullptr)
    {}

    bool enabled;
    QOpenGLContext* previous;
    QSurface* surface;

    ~RestoreCurrentContext()
    {
      if (!enabled) return;
      if (previous && surface && QOpenGLContext::currentContext() != previous)
        previous->makeCurrent(surface);
      else if (!previous && QOpenGLContext::currentContext())
        QOpenGLContext::currentContext()->doneCurrent();
    }
  };
}

ModelViewer::ModelViewer(QWidget* parent, Noggit::NoggitRenderContext context,
                         int offscreen_width, int offscreen_height)
 : PreviewRenderer(offscreen_width, offscreen_height, context, parent)
 , look(false)
 , mousedir(-1.0f)
{
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking (true);
  // The NPC browser no longer embeds an interactive client-model viewport.
  // Its ModelViewer is exclusively a thumbnail renderer, so model resources
  // must be created and destroyed in the same offscreen GL context used by
  // renderToPixmap(). VAOs are not shared between OpenGL contexts.
  _offscreen_mode = context == Noggit::NoggitRenderContext::NPC_BROWSER
    || context == Noggit::NoggitRenderContext::NPC_SPAWN_CACHE;

  _startup_time.start();
  moving = strafing = updown = lookat = turn = 0.0f;
  mousedir = -1.0f;

  _move_sensitivity = _settings->value("assetBrowser/move_sensitivity", 15.0f).toFloat() / 30.0f;

  int _fps_limit = _settings->value("fps_limit", 60).toInt();
  int _fps_calcul = (int)((1.f / (float)_fps_limit) * 1000.f);

  _update_every_event_loop.start (_fps_calcul);
  connect (&_update_every_event_loop, &QTimer::timeout, [this]
      { 
          _needs_redraw = true;
          update();
      });
}

void ModelViewer::initializeGL()
{

  OpenGL::context::scoped_setter const _ (::gl, context());
  gl.viewport(0.0f, 0.0f, width(), height());
  gl.clearColor (0.5f, 0.5f, 0.5f, 1.0f);
  emit resized();
}

void ModelViewer::paintGL()
{
  if (!_needs_redraw)
      return;
  else
      _needs_redraw = false;

  const qreal now(_startup_time.elapsed() / 1000.0);

  OpenGL::context::scoped_setter const _ (::gl, context());
  makeCurrent();

  gl.clear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  tick(now - _last_update);

  _last_update = now;

  draw();

}

void ModelViewer::resizeGL(int w, int h)
{
  OpenGL::context::scoped_setter const _ (::gl, context());
  gl.viewport(0.0f, 0.0f, w, h);
  emit resized();
  _needs_redraw = true;
}

void ModelViewer::tick(float dt)
{
  PreviewRenderer::tick(dt);

  if (turn)
  {
    _camera.add_to_yaw(math::degrees(turn));
  }
  if (lookat)
  {
    _camera.add_to_pitch(math::degrees(lookat));
  }
  if (moving)
  {
    _camera.move_forward(moving, dt);
  }
  if (strafing)
  {
    _camera.move_horizontal(strafing, dt);
  }
  if (updown)
  {
    _camera.move_vertical(updown, dt);
  }

}

blp_texture* ModelViewer::prefetchCreatureTexture(std::string const& path)
{
  if (_context != Noggit::NoggitRenderContext::NPC_SPAWN_CACHE || path.empty())
    return nullptr;
  auto found = _deferred_creature_textures.find(path);
  if (found == _deferred_creature_textures.end())
  {
    if (_deferred_creature_textures.size() >= 256)
    {
      auto const ready = std::find_if(_deferred_creature_textures.begin(),
        _deferred_creature_textures.end(), [](auto const& entry)
        {
          return entry.second->get()->finishedLoading();
        });
      if (ready != _deferred_creature_textures.end())
        _deferred_creature_textures.erase(ready);
    }
    found = _deferred_creature_textures.emplace(path,
      std::make_unique<scoped_blp_texture_reference>(path, _context)).first;
  }
  return found->second->get();
}

void ModelViewer::setModel(std::string const& filename)
{
  auto load_model = [this, &filename]
  {
    _npc_attachment_ids.clear();
    _npc_attachment_render_ids.clear();
    _creature_assets_pending = false;
    if (_context == Noggit::NoggitRenderContext::NPC_SPAWN_CACHE)
      prefetchCreatureTexture("tileset/generic/black.blp");
    PreviewRenderer::setModel(filename);
    if ((_context == Noggit::NoggitRenderContext::NPC_BROWSER
         || _context == Noggit::NoggitRenderContext::NPC_SPAWN_CACHE
         || _context == Noggit::NoggitRenderContext::NPC_CREATOR)
        && !_model_instances.empty())
    {
      // Model resources can be shared by multiple display IDs. Clear the last
      // selection's replacements before applying this display's textures.
      Model* model = _model_instances.front().model.get();
      std::fill(model->showGeosets.begin(), model->showGeosets.end(), true);
      for (auto& replacement : model->_replaceTextures)
        replacement.second = scoped_blp_texture_reference("tileset/generic/black.blp", _context);
    }
  };

  if (_offscreen_mode)
  {
    RestoreCurrentContext const restore_context(true);
    if (!offscreenContext().makeCurrent(&offscreenSurface()))
      throw std::runtime_error("could not activate the NPC thumbnail OpenGL context");
    OpenGL::context::scoped_setter const context_set(::gl, &offscreenContext());
    load_model();
  }
  else
  {
    RestoreCurrentContext const restore_context(
      _context == Noggit::NoggitRenderContext::NPC_CREATOR);
    OpenGL::context::scoped_setter const context_set(::gl, context());
    makeCurrent();
    load_model();
  }
  emit model_set(filename);
  _last_selected_model = filename;
}

bool ModelViewer::setCreatureTexture(std::size_t type, std::string const& filename)
{
  if ((_context != Noggit::NoggitRenderContext::NPC_BROWSER
       && _context != Noggit::NoggitRenderContext::NPC_SPAWN_CACHE
       && _context != Noggit::NoggitRenderContext::NPC_CREATOR)
      || _model_instances.empty() || filename.empty())
    return false;

  Model* model = _model_instances.front().model.get();
  if (!model || std::find(model->_specialTextures.begin(), model->_specialTextures.end(),
                          static_cast<int>(type)) == model->_specialTextures.end())
    return false;

  if (_context == Noggit::NoggitRenderContext::NPC_SPAWN_CACHE)
  {
    blp_texture* const texture = prefetchCreatureTexture(filename);
    if (!texture) return false;
    if (!texture->finishedLoading())
    {
      _creature_assets_pending = true;
      return false;
    }
    if (texture->loading_failed()) return false;
  }

  RestoreCurrentContext const restore_context(true);
  if (_offscreen_mode)
  {
    if (!offscreenContext().makeCurrent(&offscreenSurface())) return false;
  }
  else makeCurrent();
  OpenGL::context::scoped_setter const context_set(
    ::gl, _offscreen_mode ? &offscreenContext() : context());
  model->_replaceTextures.insert_or_assign(
      type, scoped_blp_texture_reference(filename, _context));
  update();
  return true;
}

void ModelViewer::setCreatureGeosets(std::map<unsigned, unsigned> const& variants,
                                    bool character_model, bool show_scalp)
{
  if ((_context != Noggit::NoggitRenderContext::NPC_BROWSER
       && _context != Noggit::NoggitRenderContext::NPC_SPAWN_CACHE
       && _context != Noggit::NoggitRenderContext::NPC_CREATOR)
      || _model_instances.empty())
    return;

  Model* model = _model_instances.front().model.get();
  if (!model) return;

  // CreatureDisplayInfoExtra supplies character customization and equipment
  // geosets. Ordinary creature models do not use this character layout; keep
  // their model defaults instead of treating empty packed fields as "hide".
  if (!character_model) return;

  // Default character geosets from Noggit RED's initCreatureData. In
  // particular, 100/200/300 and several higher-numbered groups use variant
  // zero, while the head, torso, feet, and hand groups default to variant one.
  static constexpr std::array<unsigned, 29> defaults = {
    0, 0, 0, 0, 1, 1, 0, 2, 1, 1,
    1, 1, 1, 1, 0, 1, 0, 0, 1, 0,
    1, 1, 1, 1, 0, 0, 0, 1, 1
  };
  auto const& ids = model->geosetIds();
  auto const robe = variants.find(13);
  bool const wearing_robe = robe != variants.end() && robe->second > defaults[13]
    && std::find(ids.begin(), ids.end(), 1300 + robe->second) != ids.end();
  for (std::size_t i = 0; i < ids.size() && i < model->showGeosets.size(); ++i)
  {
    unsigned const id = ids[i];
    if (id == 0) // The character body is not a hair variant.
    {
      model->showGeosets[i] = true;
      continue;
    }
    unsigned const group = id / 100;
    unsigned const variant = id % 100;
    if (group >= defaults.size()) continue;

    // A robe replaces the leg silhouette. Knee, trouser, and boot meshes can
    // otherwise protrude through its skirt, even with the correct robe geoset.
    if (wearing_robe && (group == 5 || group == 9 || group == 11))
    {
      model->showGeosets[i] = false;
      continue;
    }

    // A bald hairstyle can request geoset 0 (no hair) while CharHairGeosets'
    // Showscalp flag requires geoset 1 to cover the top of the head.
    if (group == 0 && variant == 1 && show_scalp)
    {
      model->showGeosets[i] = true;
      continue;
    }

    unsigned desired = defaults[group];
    if (auto const selected = variants.find(group); selected != variants.end())
      desired = selected->second;

    bool const exact_exists = std::find(ids.begin(), ids.end(), group * 100 + desired) != ids.end();
    if (!exact_exists)
    {
      // Some client variants are absent on particular races. Use that
      // group's reference default when it exists, never an unrelated shape.
      desired = defaults[group];
    }
    model->showGeosets[i] = variant == desired;
  }
  update();
}

bool ModelViewer::setCreatureAttachment(unsigned attachment_id,
                                        std::string const& model_path,
                                        std::string const& texture_path,
                                        unsigned render_attachment_id)
{
  if (!render_attachment_id)
    render_attachment_id = attachment_id == 12 ? 1 : attachment_id;
  if ((_context != Noggit::NoggitRenderContext::NPC_BROWSER
       && _context != Noggit::NoggitRenderContext::NPC_SPAWN_CACHE
       && _context != Noggit::NoggitRenderContext::NPC_CREATOR)
      || _model_instances.empty() || !_model_instances.front().model.get()
      || model_path.empty()
      || !_model_instances.front().model->hasAttachment(render_attachment_id))
    return false;

  if (_context == Noggit::NoggitRenderContext::NPC_SPAWN_CACHE)
  {
    blp_texture* const texture = prefetchCreatureTexture(
      texture_path.empty() ? "tileset/generic/black.blp" : texture_path);
    bool const texture_pending = texture && !texture->finishedLoading();
    auto found = _deferred_attachment_models.find(model_path);
    if (found == _deferred_attachment_models.end())
    {
      if (_deferred_attachment_models.size() >= 128)
      {
        auto const ready = std::find_if(_deferred_attachment_models.begin(),
          _deferred_attachment_models.end(), [](auto const& entry)
          {
            return entry.second->get()->finishedLoading();
          });
        if (ready != _deferred_attachment_models.end())
          _deferred_attachment_models.erase(ready);
      }
      found = _deferred_attachment_models.emplace(model_path,
        std::make_unique<scoped_model_reference>(
          BlizzardArchive::Listfile::FileKey(model_path), _context)).first;
    }
    Model* const prefetched = found->second->get();
    if (!prefetched->finishedLoading() || texture_pending)
    {
      _creature_assets_pending = true;
      return false;
    }
    if (prefetched->loading_failed() || prefetched->skin_load_failed())
      return false;
  }

  RestoreCurrentContext const restore_context(true);
  if (_offscreen_mode)
  {
    if (!offscreenContext().makeCurrent(&offscreenSurface())) return false;
  }
  else makeCurrent();
  OpenGL::context::scoped_setter const context_set(
    ::gl, _offscreen_mode ? &offscreenContext() : context());
  auto& attachment = _model_instances.emplace_back(model_path, _context);
  try
  {
    attachment.model->wait_until_loaded();
    if (attachment.model->loading_failed() || attachment.model->skin_load_failed())
    {
      _model_instances.pop_back();
      return false;
    }
    if (std::find(attachment.model->_specialTextures.begin(), attachment.model->_specialTextures.end(), 2)
        != attachment.model->_specialTextures.end())
      attachment.model->_replaceTextures.insert_or_assign(
          2, scoped_blp_texture_reference(
              texture_path.empty() ? "tileset/generic/black.blp" : texture_path, _context));
  }
  catch (...)
  {
    _model_instances.pop_back();
    return false;
  }
  _npc_attachment_ids.push_back(attachment_id);
  _npc_attachment_render_ids.push_back(render_attachment_id);
  update();
  return true;
}

void ModelViewer::clearCreatureAttachments()
{
  if ((_context != Noggit::NoggitRenderContext::NPC_BROWSER
       && _context != Noggit::NoggitRenderContext::NPC_SPAWN_CACHE
       && _context != Noggit::NoggitRenderContext::NPC_CREATOR)
      || _model_instances.size() <= 1)
    return;

  RestoreCurrentContext const restore_context(true);
  if (_offscreen_mode)
  {
    if (!offscreenContext().makeCurrent(&offscreenSurface())) return;
  }
  else makeCurrent();
  OpenGL::context::scoped_setter const context_set(
    ::gl, _offscreen_mode ? &offscreenContext() : context());
  _model_instances.erase(_model_instances.begin() + 1, _model_instances.end());
  _npc_attachment_ids.clear();
  _npc_attachment_render_ids.clear();
  update();
}

std::optional<Noggit::NpcAppearance> ModelViewer::creatureAppearance() const
{
  if ((_context != Noggit::NoggitRenderContext::NPC_BROWSER
       && _context != Noggit::NoggitRenderContext::NPC_SPAWN_CACHE)
      || _model_instances.empty())
    return {};

  auto snapshot_model = [](ModelInstance const& instance)
  {
    Noggit::NpcModelAppearance appearance;
    Model* model = instance.model.get();
    if (!model) return appearance;
    appearance.model_path = model->file_key().stringRepr();
    appearance.geosets = model->showGeosets;
    for (auto const& replacement : model->_replaceTextures)
      if (replacement.second.get())
        appearance.replacement_textures.emplace(
          replacement.first, replacement.second->file_key().stringRepr());
    return appearance;
  };

  Noggit::NpcAppearance appearance;
  appearance.body = snapshot_model(_model_instances.front());
  for (std::size_t index = 1;
       index < _model_instances.size() && index - 1 < _npc_attachment_ids.size(); ++index)
  {
    Noggit::NpcAttachmentAppearance attachment;
    attachment.attachment_id = _npc_attachment_ids[index - 1];
    attachment.render_attachment_id = _npc_attachment_render_ids[index - 1];
    attachment.model = snapshot_model(_model_instances[index]);
    if (!attachment.model.model_path.empty())
      appearance.attachments.push_back(std::move(attachment));
  }
  return appearance.body.model_path.empty()
    ? std::optional<Noggit::NpcAppearance>{} : std::move(appearance);
}

std::optional<glm::mat4x4> ModelViewer::modelInstanceTransform(std::size_t index) const
{
  if ((_context == Noggit::NoggitRenderContext::NPC_BROWSER
        || _context == Noggit::NoggitRenderContext::NPC_SPAWN_CACHE
       || _context == Noggit::NoggitRenderContext::NPC_CREATOR)
      && index > 0
      && index - 1 < _npc_attachment_render_ids.size())
  {
    auto const& character = _model_instances.front();
    return character.model->attachmentTransform(
        _npc_attachment_render_ids[index - 1], character.transformMatrix());
  }
  return PreviewRenderer::modelInstanceTransform(index);
}

void Noggit::Ui::Tools::AssetBrowser::ModelViewer::setMoveSensitivity(float s)
{
  _move_sensitivity = s / 30.0f;
}

float Noggit::Ui::Tools::AssetBrowser::ModelViewer::getMoveSensitivity() const
{
  return _move_sensitivity;
}

float ModelViewer::aspect_ratio() const
{
  return float (width()) / float (height());
}

void ModelViewer::mouseMoveEvent(QMouseEvent* event)
{
  QLineF const relative_movement (_last_mouse_pos, event->pos());

  if (look)
  {
    float const speed = _context == Noggit::NoggitRenderContext::NPC_BROWSER
      ? _move_sensitivity / 0.5f : 1.0f;
    glm::vec3 orbit_center{};
    float orbit_distance = 0.0f;
    if (_context == Noggit::NoggitRenderContext::NPC_CREATOR
        && !_model_instances.empty())
    {
      auto const bounds = calcSceneExtents();
      orbit_center = (bounds[0] + bounds[1]) * 0.5f;
      orbit_distance = glm::distance(_camera.position, orbit_center);
    }
    _camera.add_to_yaw(math::degrees(relative_movement.dx() * speed / 20.0f));
    _camera.add_to_pitch(math::degrees(mousedir * relative_movement.dy() * speed / 20.0f));
    if (orbit_distance > 0.0f)
      _camera.position = orbit_center - _camera.direction() * orbit_distance;
  }

  _last_mouse_pos = event->pos();
}

void ModelViewer::mousePressEvent(QMouseEvent* event)
{
  if (event->button() == Qt::RightButton
      || (_context == Noggit::NoggitRenderContext::NPC_CREATOR
          && event->button() == Qt::LeftButton))
  {
    _last_mouse_pos = event->pos();
    look = true;
  }
}

void ModelViewer::mouseReleaseEvent(QMouseEvent* event)
{
  if (event->button() == Qt::RightButton
      || (_context == Noggit::NoggitRenderContext::NPC_CREATOR
          && event->button() == Qt::LeftButton))
    look = false;
}

void ModelViewer::wheelEvent(QWheelEvent* event)
{
  if (_context == Noggit::NoggitRenderContext::NPC_CREATOR)
  {
    if (!_model_instances.empty() && event->angleDelta().y() != 0)
    {
      auto const bounds = calcSceneExtents();
      glm::vec3 const center = (bounds[0] + bounds[1]) * 0.5f;
      float const radius = std::max(glm::distance(center, bounds[0]), 0.1f);
      float const distance = glm::distance(_camera.position, center);
      float const next = std::clamp(
        distance - std::copysign(radius * 0.2f, float(event->angleDelta().y())),
        radius * 0.6f, radius * 10.0f);
      _camera.position = center - _camera.direction() * next;
    }
    update();
    event->accept();
    return;
  }
  if (_context == Noggit::NoggitRenderContext::NPC_BROWSER)
  {
    int const steps = event->angleDelta().y();
    if (steps == 0) { event->accept(); return; }
    _move_sensitivity = std::clamp(
        _move_sensitivity * std::pow(1.2f, steps / 120.0f), 0.025f, 2.0f);
    auto rescale = [this](float& input)
    {
      if (input != 0.0f) input = std::copysign(_move_sensitivity, input);
    };
    rescale(moving);
    rescale(strafing);
    rescale(updown);
    rescale(lookat);
    rescale(turn);
    emit sensitivity_changed();
    event->accept();
    return;
  }
  if (event->angleDelta().y() > 0)
  {
    _move_sensitivity = std::min(_move_sensitivity + 0.5f / 30.0f, 1.0f);
  }
  else
  {
    _move_sensitivity = std::max(_move_sensitivity - 0.5f / 30.0f, 1.0f / 30.0f);
  }

  emit sensitivity_changed();
}

void ModelViewer::keyReleaseEvent(QKeyEvent* event)
{
    checkInputsSettings();

  if (event->key() == _inputs[0] || event->key() == _inputs[1])
  {
    moving = 0.0f;
  }

  if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)
  {
    lookat = 0.0f;
  }

  if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left)
  {
    turn  = 0.0f;
  }

  if (event->key() == _inputs[2] || event->key() == _inputs[3])
  {
    strafing  = 0.0f;
  }

  if (event->key() == _inputs[4] || event->key() == _inputs[5])
  {
    updown  = 0.0f;
  }

}

void ModelViewer::focusOutEvent(QFocusEvent* event)
{
  moving = 0.0f;
  lookat = 0.0f;
  turn = 0.0f;
  strafing = 0.0f;
  updown = 0.0f;
}

void ModelViewer::keyPressEvent(QKeyEvent* event)
{
  event->accept();

  checkInputsSettings();

  if (event->key() == _inputs[0])
  {
    moving = _move_sensitivity;
  }
  if (event->key() == _inputs[1])
  {
    moving = -_move_sensitivity;
  }

  if (event->key() == Qt::Key_Up)
  {
    lookat = _move_sensitivity;
  }
  if (event->key() == Qt::Key_Down)
  {
    lookat = -_move_sensitivity;
  }

  if (event->key() == Qt::Key_Right)
  {
    turn = _move_sensitivity;
  }
  if (event->key() == Qt::Key_Left)
  {
    turn = -_move_sensitivity;
  }

  if (event->key() == _inputs[2])
  {
    strafing = _move_sensitivity;
  }
  if (event->key() == _inputs[3])
  {
    strafing = -_move_sensitivity;
  }

  if (event->key() == _inputs[4])
  {
    updown = _move_sensitivity;
  }
  if (event->key() == _inputs[5])
  {
    updown = -_move_sensitivity;
  }
}

QStringList ModelViewer::getDoodadSetNames(const std::string& filename)
{
  QStringList names;

  for (auto& wmo_instance : _wmo_instances)
  {
    if (wmo_instance.wmo->file_key().filepath() != filename)
    {
      continue;
    }

    for (auto& doodad_set : wmo_instance.wmo->doodadsets)
    {
      names.append(doodad_set.name);
    }

    break;
  }

  return std::move(names);
}

void ModelViewer::setActiveDoodadSet(const std::string& filename, const std::string& doodadset_name)
{
  for (auto& wmo_instance : _wmo_instances)
  {
    if (wmo_instance.wmo->file_key().filepath() != filename)
    {
      continue;
    }

    int counter = 0;
    for (auto& doodad_set : wmo_instance.wmo->doodadsets)
    {
      wmo_instance.change_doodadset(counter);
      counter++;
    }

    break;
  }
}

std::string& Noggit::Ui::Tools::AssetBrowser::ModelViewer::getLastSelectedModel()
{
  return _last_selected_model;
}

bool Noggit::Ui::Tools::AssetBrowser::ModelViewer::hasHeightForWidth() const
{
  return true;
}

int Noggit::Ui::Tools::AssetBrowser::ModelViewer::heightForWidth(int w) const
{
  return w;
}

ModelViewer::~ModelViewer()
{
  disconnect(_gl_guard_connection);
}

void ModelViewer::checkInputsSettings()
{
    QString _locale = _settings->value("keyboard_locale", "QWERTY").toString();

    // default is QWERTY
    _inputs = std::array<Qt::Key, 6>{Qt::Key_W, Qt::Key_S, Qt::Key_D, Qt::Key_A, Qt::Key_Q, Qt::Key_E};

    if (_locale == "AZERTY")
    {
        _inputs = std::array<Qt::Key, 6>{Qt::Key_Z, Qt::Key_S, Qt::Key_D, Qt::Key_Q, Qt::Key_A, Qt::Key_E};
    }
}
