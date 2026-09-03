// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Tool.hpp>
#include <noggit/rendering/Primitives.hpp>

#include <blizzard-archive-library/include/Listfile.hpp>

#include <optional>
#include <map>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPoint;
class QWidget;
class SceneObject;

namespace Noggit
{
  class FenceTool final : public Tool
  {
  public:
    explicit FenceTool(MapView* map_view);
    ~FenceTool() override;

    [[nodiscard]] char const* name() const override;
    [[nodiscard]] editing_mode editingMode() const override;
    [[nodiscard]] Ui::FontNoggit::Icons icon() const override;

    void setupUi(Ui::Tools::ToolPanel* tool_panel) override;
    [[nodiscard]] ToolDrawParameters drawParameters() const override;
    void onSelected() override;
    void onDeselected() override;
    void onMousePress(MousePressParameters const& params) override;
    void onMouseRelease(MouseReleaseParameters const& params) override;
    void onMouseMove(MouseMoveParameters const& params) override;
    void postRender() override;
    void unload() override;

  private:
    enum class FenceAxis
    {
      local_x,
      local_z
    };

    enum class PickRole
    {
      none,
      section,
      post
    };

    enum class PieceKind
    {
      section,
      post
    };

    struct PatternSource
    {
      std::optional<BlizzardArchive::Listfile::FileKey> file_key;
      std::string name;
      float scale = 1.0f;
      glm::vec3 rotation{};
      float height_offset = 0.0f;
      float inferred_length = 1.0f;
      glm::vec3 bounds_min{-0.5f, -0.5f, -0.5f};
      glm::vec3 bounds_max{0.5f, 0.5f, 0.5f};
      FenceAxis inferred_axis = FenceAxis::local_x;
    };

    struct FencePlacement
    {
      glm::vec3 position{};
      glm::vec3 direction{1.0f, 0.0f, 0.0f};
      glm::vec3 rotation{};
      PieceKind kind = PieceKind::section;
      float scale = 1.0f;
    };

    struct CapturedPatternObject
    {
      float distance = 0.0f;
      float lateral_offset = 0.0f;
      float height_offset = 0.0f;
      float scale = 1.0f;
      glm::vec3 rotation{};
      PieceKind kind = PieceKind::section;
      std::size_t group_index = 0;
      glm::vec2 group_local_offset{};
    };

    struct CapturedPatternGroup
    {
      float distance = 0.0f;
      float lateral_offset = 0.0f;
      float minimum_local_distance = 0.0f;
      float maximum_local_distance = 0.0f;
    };

    QWidget* _panel = nullptr;
    QLabel* _pattern_label = nullptr;
    QLabel* _path_label = nullptr;
    QLabel* _status_label = nullptr;
    QComboBox* _side_combo = nullptr;
    QComboBox* _axis_combo = nullptr;
    QDoubleSpinBox* _offset_spin = nullptr;
    QDoubleSpinBox* _spacing_spin = nullptr;
    QDoubleSpinBox* _yaw_offset_spin = nullptr;
    QDoubleSpinBox* _height_offset_spin = nullptr;
    QDoubleSpinBox* _post_height_offset_spin = nullptr;
    QDoubleSpinBox* _capture_width_spin = nullptr;
    QDoubleSpinBox* _corridor_nudge_spin = nullptr;
    QDoubleSpinBox* _width_scale_spin = nullptr;
    QComboBox* _alignment_combo = nullptr;
    QCheckBox* _align_slope_check = nullptr;
    QCheckBox* _reverse_pattern_check = nullptr;

    PickRole _pick_role = PickRole::none;
    PatternSource _section_source;
    PatternSource _post_source;
    bool _capturing_pattern_area = false;
    bool _pattern_locked = false;
    bool _painting_path = false;
    bool _reference_stroke_active = false;
    bool _reference_erase_stroke = false;
    bool _captured_full_corridor = false;
    std::optional<glm::vec3> _reference_last_brush_position;
    std::map<std::pair<int, int>, float> _reference_mask;
    std::vector<std::vector<glm::vec3>> _reference_mask_lines;
    std::vector<glm::vec3> _reference_centerline;
    float _captured_pattern_length = 0.0f;
    std::vector<CapturedPatternObject> _captured_pattern;
    std::vector<CapturedPatternGroup> _captured_pattern_groups;

    std::vector<glm::vec3> _control_points;
    std::vector<glm::vec3> _path_preview;
    std::vector<glm::vec3> _left_preview;
    std::vector<glm::vec3> _right_preview;
    std::vector<glm::vec3> _live_guide;
    std::vector<glm::vec3> _placement_segments;
    std::vector<glm::vec3> _post_segments;
    std::vector<FencePlacement> _placements;

    Rendering::Primitives::Line _line_renderer;

    bool captureSelectedPattern(bool quiet_if_missing = false);
    bool captureSource(SceneObject* object, PickRole role);
    bool pickSourceAt(QPoint const& mouse_position);
    std::optional<glm::vec3> terrainPointAt(QPoint const& mouse_position);
    void beginPatternAreaCapture();
    void paintPatternReference(glm::vec3 const& point, bool erase);
    void rebuildPatternReferencePreview();
    bool extractPatternReferenceCenterline();
    bool capturePatternArea();
    void clearCapturedPattern();
    void refreshPatternLabel();
    FenceAxis resolvedAxis() const;
    void addControlPoint(glm::vec3 point);
    void removeLastControlPoint();
    void clearPath();
    void rebuildPreview();
    void rebuildPlacementPreview();
    std::vector<glm::vec3> buildOffsetPath(float side_sign);
    void appendPlacements(std::vector<glm::vec3> const& path);
    void commitFence();
    void updatePanelStatus(QString const& message = {});
  };
}
