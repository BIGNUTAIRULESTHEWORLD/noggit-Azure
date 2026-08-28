// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_BRUSHSTACK_HPP
#define NOGGIT_BRUSHSTACK_HPP

#include <ui_BrushStack.h>

#include <noggit/tool_enums.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/ui/tools/Stamp/MapStampAsset.hpp>

#include <QWidget>
#include <QJsonObject>

#include <glm/vec3.hpp>

#include <map>
#include <optional>
#include <utility>
#include <vector>

class MapView;
class MapChunk;
class World;

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QSpinBox;

namespace Noggit::Ui::Tools::UiCommon
{
  class ExtendedSlider;
}

namespace Noggit::Ui::Tools
{
  class BrushStackItem;

  class BrushStack : public QWidget
  {
  public:
    BrushStack(MapView* map_view, QWidget* parent = nullptr);

    void execute(glm::vec3 const& cursor_pos, World* world, float dt, bool mod_shift_down, bool mod_alt_down, bool mod_ctrl_down, bool is_under_map);

    void changeRadius(float change);
    void changeInnerRadius(float change);
    void changeSpeed(float change);
    void changeRotation(int change);
    void setRotation(int rotation);

    float getRadius();
    float getInnerRadius();
    float getSpeed();
    bool getBrushMode() const;;
    bool getRandomizeRotation() const;;
    BrushStackItem* getActiveBrushItem();;
    bool hasLoadedMapStamp() const;
    bool hasActiveMapStamp() const;
    [[nodiscard]] bool isMapStampProtectionVisible() const;
    [[nodiscard]] glm::vec3 mapStampProtectionOverlayCenter() const;
    [[nodiscard]] float mapStampProtectionOverlayRadius() const;
    [[nodiscard]] float mapStampPreviewRadius() const;
    [[nodiscard]] float mapStampInnerRadiusRatio() const;
    [[nodiscard]] bool isMapStampExclusionBrushEnabled() const;
    void deactivateMapStampExclusionBrush();
    [[nodiscard]] float mapStampExclusionBrushRadius() const;
    [[nodiscard]] BrushShape mapStampExclusionBrushShape() const;
    void changeMapStampExclusionBrushRadius(float change);
    bool executeMapStamp(glm::vec3 const& cursor_pos, World* world);
    void updateMapStampPreview();
    void updateMapStampTerrainPreview(glm::vec3 const& cursor_pos, World* world);
    void clearMapStampTerrainPreview();
    [[nodiscard]] bool isMapStampHeightDragEnabled() const;
    [[nodiscard]] bool isMapStampHeightDragActive() const;
    bool beginMapStampHeightDrag(glm::vec3 const& cursor_pos);
    void adjustMapStampHeightDrag(float vertical_pixels, bool fine_adjustment);
    bool commitMapStampHeightDrag(World* world);
    void endMapStampHeightDrag();
    [[nodiscard]] std::vector<std::vector<glm::vec3>> const& mapStampHeightPreviewLines() const;
    void lockMapStampPosition(glm::vec3 const& cursor_pos);
    void toggleMapStampPositionLock(glm::vec3 const& cursor_pos);
    [[nodiscard]] bool isMapStampPositionLocked() const;
    [[nodiscard]] glm::vec3 mapStampPosition(glm::vec3 const& cursor_pos) const;
    void paintMapStampProtection(glm::vec3 const& cursor_pos);
    [[nodiscard]] bool isMapStampPaintedSelectionEnabled() const;
    [[nodiscard]] float mapStampPaintedSelectionRadius() const;
    void paintMapStampSelection(glm::vec3 const& cursor_pos, bool erase);
    void endMapStampSelectionStroke();
    void deactivateMapStampPaintedSelection();

    QJsonObject toJSON();
    void fromJSON(QJsonObject const& json);

  private:

    void addAction(BrushStackItem* brush_stack_item);
    void setupMapStampUi();
    void refreshMapStampLibrary(QString const& active_path = {});
    bool loadMapStamp(QString const& path);
    [[nodiscard]] Stamp::MapStampProtectionSettings mapStampProtectionSettings() const;
    [[nodiscard]] Stamp::MapStampHeightMode mapStampHeightMode() const;
    [[nodiscard]] float mapStampRadius() const;
    [[nodiscard]] float mapStampHardness() const;
    void markMapStampTerrainPreviewDirty(bool textures_dirty = true);

    ::Ui::brushStack _ui;
    QWidget* _add_popup;
    QComboBox* _add_operation_combo;
    MapView* _map_view;
    QButtonGroup* _active_item_button_group;
    BrushStackItem* _active_item = nullptr;
    Stamp::MapStampAsset _map_stamp;
    QCheckBox* _map_stamp_enabled = nullptr;
    QWidget* _map_stamp_options = nullptr;
    QComboBox* _map_stamp_library = nullptr;
    QComboBox* _map_stamp_shape = nullptr;
    QWidget* _map_stamp_painted_controls = nullptr;
    QCheckBox* _map_stamp_painted_selection = nullptr;
    UiCommon::ExtendedSlider* _map_stamp_painted_radius = nullptr;
    QComboBox* _map_stamp_height_mode = nullptr;
    QCheckBox* _map_stamp_position_lock = nullptr;
    UiCommon::ExtendedSlider* _map_stamp_radius = nullptr;
    QDoubleSpinBox* _map_stamp_edge_blend = nullptr;
    QDoubleSpinBox* _map_stamp_height_scale = nullptr;
    QDoubleSpinBox* _map_stamp_height_offset = nullptr;
    QCheckBox* _map_stamp_height_drag = nullptr;
    QDoubleSpinBox* _map_stamp_opacity = nullptr;
    QSpinBox* _map_stamp_rotation = nullptr;
    QCheckBox* _map_stamp_randomize_rotation = nullptr;
    QCheckBox* _map_stamp_auto_protection = nullptr;
    QCheckBox* _map_stamp_show_protection = nullptr;
    QDoubleSpinBox* _map_stamp_protection_slope = nullptr;
    QDoubleSpinBox* _map_stamp_protection_relief = nullptr;
    QGroupBox* _map_stamp_exclusion_brush = nullptr;
    UiCommon::ExtendedSlider* _map_stamp_exclusion_radius = nullptr;
    QButtonGroup* _map_stamp_exclusion_shape = nullptr;
    QButtonGroup* _map_stamp_exclusion_operation = nullptr;
    QLabel* _map_stamp_status = nullptr;
    QImage _map_stamp_preview;
    glm::vec3 _map_stamp_locked_position{};
    bool _map_stamp_height_drag_active = false;
    bool _map_stamp_terrain_preview_dirty = true;
    bool _map_stamp_texture_preview_dirty = true;
    bool _map_stamp_terrain_preview_valid = false;
    glm::vec3 _map_stamp_terrain_preview_center{};
    std::vector<std::vector<glm::vec3>> _map_stamp_height_preview_lines;
    struct MapStampPreviewChunk
    {
      TileIndex tile_index{0, 0};
      unsigned x = 0;
      unsigned z = 0;
    };
    std::vector<MapStampPreviewChunk> _map_stamp_terrain_preview_chunks;
    glm::vec3 _map_stamp_protection_overlay_center{};
    float _map_stamp_protection_overlay_radius = 1.f;
    struct MapStampProtectionStroke
    {
      glm::vec3 center;
      float radius = 0.f;
      bool protect = true;
      BrushShape shape = BrushShape::CIRCLE;
    };
    std::vector<MapStampProtectionStroke> _map_stamp_protection_strokes;
    Stamp::MapStampPaintedCells _map_stamp_painted_cells;
    std::optional<glm::vec3> _map_stamp_painted_last_position;

  };
}

#endif //NOGGIT_BRUSHSTACK_HPP
