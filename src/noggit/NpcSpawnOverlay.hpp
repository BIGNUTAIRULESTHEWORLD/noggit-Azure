#pragma once

#include <noggit/ModelInstance.h>
#include <noggit/NpcAppearance.hpp>
#include <noggit/rendering/ModelRender.hpp>
#include <noggit/scoped_blp_texture_reference.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace Noggit
{
  class NpcSpawnOverlay
  {
  public:
    static constexpr float view_distance = 250.0f;

    struct PreviewWaypoint
    {
      glm::vec3 position{};
      unsigned delay_ms = 0;
      bool run = false;
      bool show_marker = true;
      unsigned arrival_animation_id = 0;
    };

    struct Attachment
    {
      Attachment(NpcAttachmentAppearance const& appearance, NoggitRenderContext context);

      unsigned attachment_id;
      unsigned render_attachment_id;
      scoped_model_reference model;
      std::vector<bool> geosets;
      std::map<std::size_t, scoped_blp_texture_reference> replacement_textures;

      Rendering::ModelAppearanceOverride renderAppearance() const;
    };

    NpcSpawnOverlay(std::uint64_t guid, unsigned entry, glm::vec3 const& position,
                    float yaw, float scale, NpcAppearance const& appearance,
                    NoggitRenderContext context);

    std::uint64_t guid() const { return _guid; }
    unsigned entry() const { return _entry; }
    void setIdentity(std::uint64_t guid, unsigned entry);
    void setTransform(glm::vec3 const& position, float yaw, float scale);
    glm::vec3 const& anchorPosition() const { return _anchor_position; }
    float anchorYaw() const { return _anchor_yaw; }
    glm::mat4x4 anchorTransformMatrix() const;
    float scale() const { return _body.scale; }
    void setPreviewRoute(std::vector<PreviewWaypoint> points, bool enabled,
                         std::optional<glm::vec3> start = std::nullopt,
                         bool show_waypoint_path = false,
                         std::optional<glm::vec3> end = std::nullopt);
    void setMovementPreviewSettings(float wander_radius, float walk_speed_multiplier,
                                    float run_speed_multiplier);
    std::vector<PreviewWaypoint> const& previewRoute() const { return _preview_route; }
    glm::vec3 const& previewRouteStart() const { return _preview_route_start; }
    std::optional<glm::vec3> const& previewRouteEnd() const { return _preview_route_end; }
    bool showsWaypointPath() const { return _show_waypoint_path; }
    float wanderRadius() const { return _wander_radius; }
    void setPreviewAnimation(unsigned animation_id, bool override_movement = true,
                             bool loop = true);
    void restartPreviewAnimation();
    void updatePreview(int animtime);
    unsigned previewAnimation() const { return _active_animation_id; }
    int previewAnimationTime() const { return _active_animation_time; }
    unsigned previewBlendFromAnimation() const { return _blend_from_animation_id; }
    int previewBlendFromAnimationTime() const { return _blend_from_animation_time; }
    float previewAnimationBlend() const { return _animation_blend; }
    ModelInstance& body() { return _body; }
    ModelInstance const& body() const { return _body; }
    std::vector<Attachment>& attachments() { return _attachments; }
    void setWeaponVisibility(bool show_main_off_hand, bool show_ranged);
    bool showsMainOffHand() const { return _show_main_off_hand; }
    bool showsRangedWeapon() const { return _show_ranged_weapon; }
    Rendering::ModelAppearanceOverride renderAppearance() const;

  private:
    std::uint64_t _guid;
    unsigned _entry;
    ModelInstance _body;
    glm::vec3 _anchor_position{};
    float _anchor_yaw = 0.0f;
    std::vector<PreviewWaypoint> _preview_route;
    glm::vec3 _preview_route_start{};
    std::optional<glm::vec3> _preview_route_end;
    bool _preview_enabled = false;
    bool _show_waypoint_path = false;
    float _wander_radius = 0.0f;
    float _walk_speed_multiplier = 1.0f;
    float _run_speed_multiplier = 1.14286f;
    int _preview_start_time = -1;
    unsigned _requested_animation_id = 0;
    unsigned _active_animation_id = 0;
    bool _override_movement_animation = true;
    bool _loop_requested_animation = true;
    int _animation_start_time = -1;
    int _active_animation_time = 0;
    unsigned _blend_from_animation_id = 0;
    int _blend_from_animation_start_time = 0;
    int _blend_from_animation_time = 0;
    int _blend_start_time = -1;
    float _animation_blend = 1.0f;
    std::vector<bool> _geosets;
    std::map<std::size_t, scoped_blp_texture_reference> _replacement_textures;
    std::vector<Attachment> _attachments;
    bool _show_main_off_hand = true;
    bool _show_ranged_weapon = true;

    void applyAnimation(unsigned animation_id, int animation_time, int now);
  };
}
