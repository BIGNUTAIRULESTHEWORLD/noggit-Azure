// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Tool.hpp>
#include <noggit/BoolToggleProperty.hpp>
#include <noggit/RoadStyle.hpp>

#include <map>
#include <optional>
#include <utility>
#include <vector>

class QDockWidget;

namespace Noggit
{
    namespace Ui
    {
        class texturing_tool;
        struct tileset_chooser;
        class texture_picker;
        class texture_palette_small;
    }

    class TexturingTool final : public Tool
    {
    public:
        TexturingTool(MapView* mapView);
        ~TexturingTool();
        void unload() override;

        [[nodiscard]]
        char const* name() const override;

        [[nodiscard]]
        editing_mode editingMode() const override;

        [[nodiscard]]
        Ui::FontNoggit::Icons icon() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;

        void registerMenuItems(QMenu* menu) override;

        [[nodiscard]]
        ToolDrawParameters drawParameters() const override;

        void onSelected() override;

        void onDeselected() override;

        void onTick(float deltaTime, TickParameters const& params) override;

        void onMousePress(MousePressParameters const& params) override;

        void onMouseRelease(MouseReleaseParameters const& params) override;

        void onMouseMove(MouseMoveParameters const& params) override;

        void onMouseWheel(MouseWheelParameters const& params) override;

        void hidePopups() override;

    private:
        enum class road_session_state
        {
            idle,
            selecting_reference,
            reference_ready,
            routing
        };

        Ui::texturing_tool* _texturingTool = nullptr;
        QDockWidget* _textureBrowserDock = nullptr;
        Ui::tileset_chooser* _textureBrowser = nullptr;
        Ui::texture_picker* _texturePicker = nullptr;
        Ui::texture_palette_small* _texturePalette = nullptr;
        QDockWidget* _texturePaletteDock = nullptr;
        QDockWidget* _texturePickerDock = nullptr;
        bool _texturePickerNeedUpdate = false;
        // onTick runs every frame the button is held; warn once per stroke
        bool _ge_brush_warning_shown = false;
        bool _texture_stroke_active = false;
        road_session_state _road_session = road_session_state::idle;
        bool _road_reference_stroke_active = false;
        bool _road_reference_erase_stroke = false;
        float _road_reference_effective_radius = 10.0f;
        std::optional<glm::vec3> _road_reference_last_brush_position;
        std::map<std::pair<int, int>, float> _road_reference_mask;
        std::vector<std::vector<glm::vec3>> _road_reference_mask_lines;
        std::optional<sampled_road_style> _road_style;
        std::vector<glm::vec3> _road_reference_centerline;
        std::vector<glm::vec3> _road_reference_left_edge;
        std::vector<glm::vec3> _road_reference_right_edge;
        std::vector<glm::vec3> _road_control_points;
        std::vector<glm::vec3> _road_preview_centerline;
        std::vector<glm::vec3> _road_preview_left_edge;
        std::vector<glm::vec3> _road_preview_right_edge;
        bool _road_preview_blocked = false;
        Noggit::BoolToggleProperty _show_texture_browser_window = { false };
        Noggit::BoolToggleProperty _show_texture_palette_window = { false };

        void randomizeTexturingRotation();
        bool sampleRoadAtCursor();
        bool sampleRoadAt(glm::vec3 const& click,
                          std::vector<glm::vec3> const* reference_centerline = nullptr);
        void beginRoadReferenceSelection();
        void paintRoadReferenceMask(glm::vec3 const& point, bool erase);
        void rebuildRoadReferenceMaskPreview();
        bool extractRoadReferenceCenterline();
        void acceptRoadReferenceSelection();
        void cancelRoadSession();
        void setRoadStart(glm::vec3 const& point);
        void addRoadControlPoint(glm::vec3 const& point);
        void rebuildRoadPreview(glm::vec3 const& live_endpoint, bool include_live_endpoint);
        void clearRoadPreview(bool clear_style);
        void commitRoadPreview();

        void setupTextureBrowser(MapView* mv);
        void setupTexturePalette(MapView* mv);
        void setupTexturePicker(MapView* mv);
    };
}
