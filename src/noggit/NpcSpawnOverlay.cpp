#include <noggit/NpcSpawnOverlay.hpp>

#include <blizzard-archive-library/include/Listfile.hpp>

#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/trigonometric.hpp>

namespace Noggit
{
  namespace
  {
    void loadTextures(std::map<std::size_t, scoped_blp_texture_reference>& destination,
                      std::map<std::size_t, std::string> const& source,
                      NoggitRenderContext context)
    {
      for (auto const& texture : source)
        destination.emplace(texture.first, scoped_blp_texture_reference(texture.second, context));
    }
  }

  NpcSpawnOverlay::Attachment::Attachment(NpcAttachmentAppearance const& appearance,
                                           NoggitRenderContext context)
    : attachment_id(appearance.attachment_id)
    , render_attachment_id(appearance.render_attachment_id
        ? appearance.render_attachment_id : appearance.attachment_id)
    , model(BlizzardArchive::Listfile::FileKey(appearance.model.model_path), context)
    , geosets(appearance.model.geosets)
  {
    model->wait_until_loaded();
    if (model->loading_failed() || model->skin_load_failed())
      throw std::runtime_error("NPC attachment model could not be loaded");
    loadTextures(replacement_textures, appearance.model.replacement_textures, context);
  }

  Rendering::ModelAppearanceOverride NpcSpawnOverlay::Attachment::renderAppearance() const
  {
    return {&geosets, &replacement_textures};
  }

  NpcSpawnOverlay::NpcSpawnOverlay(std::uint64_t guid, unsigned entry,
                                   glm::vec3 const& position, float yaw, float scale,
                                   NpcAppearance const& appearance, NoggitRenderContext context)
    : _guid(guid)
    , _entry(entry)
    , _body(BlizzardArchive::Listfile::FileKey(appearance.body.model_path), context)
    , _geosets(appearance.body.geosets)
  {
    _body.chunk_mover_preview = true;
    _body.model->wait_until_loaded();
    if (_body.model->loading_failed() || _body.model->skin_load_failed())
      throw std::runtime_error("NPC body model could not be loaded");
    setTransform(position, yaw, scale);
    loadTextures(_replacement_textures, appearance.body.replacement_textures, context);

    for (NpcAttachmentAppearance const& attachment : appearance.attachments)
    {
      try
      {
        _attachments.emplace_back(attachment, context);
      }
      catch (std::exception const&)
      {
        // Keep the body visible when one optional helmet or shoulder is missing.
      }
    }

    // Showing a ranged weapon alongside held weapons commonly intersects the
    // body or shield. Keep ranged-only NPCs visible by default.
    bool const has_held_weapon = std::any_of(_attachments.begin(), _attachments.end(),
      [](Attachment const& attachment)
      {
        return attachment.attachment_id == 0 || attachment.attachment_id == 1
            || attachment.attachment_id == 2;
      });
    if (has_held_weapon) _show_ranged_weapon = false;
  }

  void NpcSpawnOverlay::setWeaponVisibility(bool show_main_off_hand, bool show_ranged)
  {
    _show_main_off_hand = show_main_off_hand;
    _show_ranged_weapon = show_ranged;
  }

  void NpcSpawnOverlay::setIdentity(std::uint64_t guid, unsigned entry)
  {
    _guid = guid;
    _entry = entry;
  }

  void NpcSpawnOverlay::setTransform(glm::vec3 const& position, float yaw, float scale)
  {
    _anchor_position = position;
    _anchor_yaw = yaw;
    _body.pos = position;
    _body.dir = {0.0f, yaw, 0.0f};
    _body.scale = scale;
    _body.recalcExtents();
  }

  glm::mat4x4 NpcSpawnOverlay::anchorTransformMatrix() const
  {
    glm::mat4x4 matrix = glm::translate(glm::mat4x4(1.0f), _anchor_position);
    matrix *= glm::eulerAngleYZX(glm::radians(_anchor_yaw - 90.0f), 0.0f, 0.0f);
    return glm::scale(matrix, glm::vec3(_body.scale));
  }

  void NpcSpawnOverlay::setPreviewRoute(std::vector<PreviewWaypoint> points, bool enabled,
                                        std::optional<glm::vec3> start,
                                        bool show_waypoint_path,
                                        std::optional<glm::vec3> end)
  {
    _preview_route = std::move(points);
    _preview_route_start = start.value_or(_anchor_position);
    _preview_enabled = enabled;
    _show_waypoint_path = show_waypoint_path;
    _preview_route_end = end;
    _preview_start_time = -1;
    if (!enabled || _preview_route.empty())
      setTransform(_anchor_position, _anchor_yaw, _body.scale);
    else
    {
      _body.pos = _preview_route_start;
      _body.recalcExtents();
    }
  }

  void NpcSpawnOverlay::setMovementPreviewSettings(float wander_radius,
                                                     float walk_speed_multiplier,
                                                     float run_speed_multiplier)
  {
    _wander_radius = std::max(0.0f, wander_radius);
    _walk_speed_multiplier = std::max(0.01f, walk_speed_multiplier);
    _run_speed_multiplier = std::max(0.01f, run_speed_multiplier);
    _preview_start_time = -1;
  }

  void NpcSpawnOverlay::setPreviewAnimation(unsigned animation_id, bool override_movement,
                                             bool loop)
  {
    if (_requested_animation_id != animation_id || _loop_requested_animation != loop)
      _animation_start_time = -1;
    _requested_animation_id = animation_id;
    _override_movement_animation = override_movement;
    _loop_requested_animation = loop;
  }

  void NpcSpawnOverlay::restartPreviewAnimation()
  {
    _animation_start_time = -1;
  }

  void NpcSpawnOverlay::applyAnimation(unsigned animation_id, int animation_time, int now)
  {
    constexpr int transition_duration_ms = 220;
    if (_active_animation_id != animation_id)
    {
      _blend_from_animation_id = _active_animation_id;
      _blend_from_animation_start_time = _active_animation_time;
      _blend_from_animation_time = _active_animation_time;
      _blend_start_time = now;
      _animation_blend = 0.0f;
    }

    _active_animation_id = animation_id;
    _active_animation_time = animation_time;

    if (_blend_start_time < 0)
    {
      _animation_blend = 1.0f;
      return;
    }

    int const elapsed = std::max(0, now - _blend_start_time);
    _blend_from_animation_time = _blend_from_animation_start_time + elapsed;
    _animation_blend = std::min(1.0f,
      static_cast<float>(elapsed) / static_cast<float>(transition_duration_ms));
    if (_animation_blend >= 1.0f)
      _blend_start_time = -1;
  }

  void NpcSpawnOverlay::updatePreview(int animtime)
  {
    if (_animation_start_time < 0 || animtime < _animation_start_time)
      _animation_start_time = animtime;
    int const requested_time = animtime - _animation_start_time;
    std::uint32_t const requested_length = _requested_animation_id
      ? _body.model->animationLength(static_cast<std::uint16_t>(_requested_animation_id)) : 0u;
    bool const requested_active = _requested_animation_id
      && (_loop_requested_animation || !requested_length
          || requested_time < static_cast<int>(requested_length));

    if (!_preview_enabled || _preview_route.empty())
    {
      applyAnimation(requested_active ? _requested_animation_id : 0u,
                     requested_active ? requested_time : animtime, animtime);
      return;
    }
    if (_preview_start_time < 0 || animtime < _preview_start_time)
      _preview_start_time = animtime;

    glm::vec3 const start = _preview_route_start;
    std::int64_t total_ms = 0;
    glm::vec3 previous = start;
    for (PreviewWaypoint const& point : _preview_route)
    {
      float const speed = point.run ? 7.0f * _run_speed_multiplier
                                    : 2.5f * _walk_speed_multiplier;
      total_ms += static_cast<std::int64_t>(
        std::max(1.0f, glm::distance(previous, point.position) / speed * 1000.0f));
      total_ms += point.delay_ms;
      previous = point.position;
    }
    if (total_ms <= 0) return;

    std::int64_t elapsed = (static_cast<std::int64_t>(animtime) - _preview_start_time) % total_ms;
    previous = start;
    for (PreviewWaypoint const& point : _preview_route)
    {
      float const speed = point.run ? 7.0f * _run_speed_multiplier
                                    : 2.5f * _walk_speed_multiplier;
      std::int64_t const travel_ms = static_cast<std::int64_t>(
        std::max(1.0f, glm::distance(previous, point.position) / speed * 1000.0f));
      if (elapsed < travel_ms)
      {
        float const fraction = static_cast<float>(elapsed) / static_cast<float>(travel_ms);
        glm::vec3 const position = glm::mix(previous, point.position, fraction);
        glm::vec3 const direction = point.position - previous;
        float const yaw = glm::degrees(std::atan2(direction.x, direction.z));
        _body.pos = position;
        _body.dir = {0.0f, yaw, 0.0f};
        _body.recalcExtents();
        applyAnimation(_override_movement_animation && requested_active
                         ? _requested_animation_id : (point.run ? 5u : 4u),
                       _override_movement_animation && requested_active
                         ? requested_time : animtime,
                       animtime);
        return;
      }
      elapsed -= travel_ms;
      if (elapsed < point.delay_ms)
      {
        _body.pos = point.position;
        _body.recalcExtents();
        unsigned const arrival_animation = point.arrival_animation_id
          ? point.arrival_animation_id
          : (requested_active ? _requested_animation_id : 0u);
        applyAnimation(arrival_animation,
                       point.arrival_animation_id ? static_cast<int>(elapsed)
                         : (requested_active ? requested_time : animtime),
                       animtime);
        return;
      }
      elapsed -= point.delay_ms;
      previous = point.position;
    }
  }

  Rendering::ModelAppearanceOverride NpcSpawnOverlay::renderAppearance() const
  {
    return {&_geosets, &_replacement_textures};
  }
}
