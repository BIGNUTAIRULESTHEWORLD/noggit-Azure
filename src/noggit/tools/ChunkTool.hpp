// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <noggit/Tool.hpp>

namespace Noggit
{
    namespace Ui::Tools::ChunkManipulator
    {
        class ChunkManipulatorPanel;
        class ChunkClipboard;
    }

    class ChunkTool final : public Tool
    {
    public:
        ChunkTool(MapView* mapView);
        ~ChunkTool();

        [[nodiscard]]
        virtual char const* name() const override;

        [[nodiscard]]
        virtual editing_mode editingMode() const override;

        [[nodiscard]]
        virtual Ui::FontNoggit::Icons icon() const override;

        void setupUi(Ui::Tools::ToolPanel* toolPanel) override;
        [[nodiscard]] ToolDrawParameters drawParameters() const override;
        [[nodiscard]] float brushRadius() const override;
        void onSelected() override;
        void onDeselected() override;
        void onTick(float deltaTime, TickParameters const& params) override;
        void onMousePress(MousePressParameters const& params) override;
        void onMouseRelease(MouseReleaseParameters const& params) override;
        void onMouseMove(MouseMoveParameters const& params) override;
        void onMouseWheel(MouseWheelParameters const& params) override;
        void unload() override;

    private:
        void refreshClipboard();
        void clearSelection();
        void queuePreviewUpdate();
        void updatePreviewImmediately();
        void pasteAtCursor();
        bool updateAdtLoadingState();

        Ui::Tools::ChunkManipulator::ChunkManipulatorPanel* _chunkManipulator = nullptr;
        Ui::Tools::ChunkManipulator::ChunkClipboard* _clipboard = nullptr;
        bool _preview_suppressed = false;
        bool _selection_dirty = false;
        bool _adt_loading = false;
    };
}
