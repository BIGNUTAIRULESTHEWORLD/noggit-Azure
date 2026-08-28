// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Tool.hpp>

namespace Noggit
{
    namespace Ui::Tools
    {
        class BrushStack;
    }

    class StampTool final : public Tool
    {
    public:
        StampTool(MapView* mapView);
        ~StampTool();

        [[nodiscard]]
        virtual char const* name() const override;

        [[nodiscard]]
        virtual editing_mode editingMode() const override;

        [[nodiscard]]
        virtual Ui::FontNoggit::Icons icon() const override;

        [[nodiscard]]
        unsigned int actionModality() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;

        [[nodiscard]]
        ToolDrawParameters drawParameters() const override;

        void onSelected() override;

        void onDeselected() override;

        void onTick(float deltaTime, TickParameters const& params) override;

        void onMousePress(MousePressParameters const& params) override;

        void onMouseRelease(MouseReleaseParameters const& params) override;

        void onMouseMove(MouseMoveParameters const& params) override;

        void onMouseWheel(MouseWheelParameters const& params) override;

        void onFocusLost() override;

    private:
        Ui::Tools::BrushStack* _stampTool = nullptr;
        bool _stamp_rotation_drag_active = false;
        float _stamp_rotation_drag_remainder = 0.f;

        void endStampRotationDrag();
        void randomizeStampRotation();
    };
}
