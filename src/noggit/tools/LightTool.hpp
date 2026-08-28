// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Tool.hpp>
#include <noggit/Sky.h>

#include <optional>
#include <cstddef>

namespace Noggit
{
    namespace Ui::Tools
    {
        class LightEditor;
    }

    class LightTool final : public Tool
    {
    public:
        LightTool(MapView* mapView);
        ~LightTool();

        [[nodiscard]]
        virtual char const* name() const override;

        [[nodiscard]]
        virtual editing_mode editingMode() const override;

        [[nodiscard]]
        virtual Ui::FontNoggit::Icons icon() const override;

        [[nodiscard]] unsigned int actionModality() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;

        void onTick(float deltaTime, TickParameters const& params) override;

        void onSelected() override;

        void onDeselected() override;

        void onMousePress(MousePressParameters const& params) override;
        void onMouseRelease(MouseReleaseParameters const& params) override;
        void onMouseMove(MouseMoveParameters const& params) override;
        void onFocusLost() override;
        void renderImGui(ImGuizmo::MODE mode, ImGuizmo::OPERATION operation) override;

    private:
        Ui::Tools::LightEditor* _lightEditor = nullptr;
        int _selectedSkyId = 0;
        int _browserLightId = 0;
        std::optional<Sky> _skyClipboard;
        float _keyX = 0.0f;
        float _keyZ = 0.0f;
        float _keyScale = 0.0f;
        float _mouseHorizontal = 0.0f;
        float _mouseVertical = 0.0f;
        bool _moveSky = false;
        bool _gizmoWasUsing = false;
        std::size_t _knownSkyCount = 0;
        bool _skyCountInitialized = false;

        void setupHotkeys();
        Sky* selectedSky();
        void selectSky(int sky_id, bool update_editor = true);
        void pickSky(QPoint const& mouse_position);
        void copySelectedSky();
        void pasteSky();
        void deleteSelectedSky();
        void transformSelected(glm::vec3 const& translation, float radius_multiplier = 1.0f);
        void persistSelected();
    };
}
