// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "StampTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/Input.hpp>
#include <noggit/MapView.h>
#include <noggit/ui/tools/BrushStack/BrushStack.hpp>
#include <noggit/ui/tools/BrushStack/BrushStackItem.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/World.h>

#include <QWheelEvent>

#include <random>

namespace Noggit
{
    StampTool::StampTool(MapView* mapView)
        : Tool{ mapView }
    {
        addHotkey("mapStampLockCursor"_hash, Hotkey{
            .onPress = [this, mapView]
            {
                if (_stampTool)
                    _stampTool->lockMapStampPosition(mapView->cursorPosition());
            },
            .condition = [this, mapView]
            {
                return _stampTool && _stampTool->hasActiveMapStamp()
                    && mapView->get_editing_mode() == editing_mode::stamp;
            },
        });

        addHotkey("mapStampToggleLock"_hash, Hotkey{
            .onPress = [this, mapView]
            {
                if (_stampTool)
                    _stampTool->toggleMapStampPositionLock(mapView->cursorPosition());
            },
            .condition = [this, mapView]
            {
                return _stampTool && _stampTool->hasActiveMapStamp()
                    && mapView->get_editing_mode() == editing_mode::stamp;
            },
        });

        addHotkey("stampRotateDrag"_hash, Hotkey{
            .onPress = [this]
            {
                if (_stamp_rotation_drag_active)
                    return;

                _stamp_rotation_drag_active = true;
                _stamp_rotation_drag_remainder = 0.f;
                auto* action = NOGGIT_ACTION_MGR->beginAction(
                    this->mapView(), Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                    Noggit::ActionModalityControllers::eROTATE);
                action->setBlockCursor(true);
                action->tagAdress(reinterpret_cast<std::uintptr_t>(this));
            },
            .onRelease = [this] { endStampRotationDrag(); },
            .condition = [this, mapView]
            {
                if (_stamp_rotation_drag_active)
                    return true;
                if (!_stampTool || mapView->get_editing_mode() != editing_mode::stamp
                    || NOGGIT_CUR_ACTION || _stampTool->isMapStampHeightDragActive()
                    || _stampTool->isMapStampPaintedSelectionEnabled()
                    || _stampTool->isMapStampExclusionBrushEnabled())
                {
                    return false;
                }

                auto* const active_item = _stampTool->getActiveBrushItem();
                return _stampTool->hasActiveMapStamp()
                    || (active_item && active_item->isMaskEnabled());
            },
        });
    }

    StampTool::~StampTool()
    {
        delete _stampTool;
    }

    char const* StampTool::name() const
    {
        return "Stamp Mode";
    }

    editing_mode StampTool::editingMode() const
    {
        return editing_mode::stamp;
    }

    Ui::FontNoggit::Icons StampTool::icon() const
    {
        return Ui::FontNoggit::TOOL_STAMP;
    }

    unsigned int StampTool::actionModality() const
    {
        return _stamp_rotation_drag_active
            ? Noggit::ActionModalityControllers::eROTATE
            : Noggit::ActionModalityControllers::eNONE;
    }

    void StampTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _stampTool = new Noggit::Ui::Tools::BrushStack(mapView(), mapView());
        toolPanel->registerTool(this, _stampTool);

        QObject::connect(NOGGIT_ACTION_MGR, &ActionManager::historyNavigated, _stampTool,
            [this]
            {
                _stampTool->endMapStampHeightDrag();
                _stampTool->clearMapStampTerrainPreview();
            });

        QObject::connect(mapView(), &MapView::trySetBrushTexture, [=](QImage* brush, QWidget* sender) {
            auto mv = mapView();
            auto item = _stampTool->getActiveBrushItem();

            if (mv->get_editing_mode() != editing_mode::stamp
                || (!_stampTool->hasActiveMapStamp()
                    && !_stampTool->isMapStampExclusionBrushEnabled()
                    && item && item->getTool() == sender))
            {
                mv->setBrushTexture(brush);
            }
            });
    }

    ToolDrawParameters StampTool::drawParameters() const
    {
        bool const painted_selection = _stampTool->isMapStampPaintedSelectionEnabled();
        bool const exclusion_brush = _stampTool->isMapStampExclusionBrushEnabled();
        auto* const active_item = _stampTool->getActiveBrushItem();
        bool const active_map_stamp = _stampTool->hasActiveMapStamp();
        bool const image_mask = active_item && active_item->isMaskEnabled();
        CursorType const cursor_type = painted_selection ? CursorType::CIRCLE : (exclusion_brush
            ? (_stampTool->mapStampExclusionBrushShape() == BrushShape::SQUARE
                ? CursorType::SQUARE : CursorType::CIRCLE)
            : (active_map_stamp || image_mask
                ? CursorType::STAMP
                : (active_item && active_item->brushShape() == BrushShape::SQUARE
                    ? CursorType::SQUARE : CursorType::CIRCLE)));
        return
        {
            .radius = painted_selection ? _stampTool->mapStampPaintedSelectionRadius()
                : (exclusion_brush ? _stampTool->mapStampExclusionBrushRadius()
                : (active_map_stamp ? _stampTool->mapStampPreviewRadius()
                                    : _stampTool->getRadius())),
            .inner_radius = painted_selection ? .75f : (exclusion_brush ? .75f
                                            : (active_map_stamp
                                                ? _stampTool->mapStampInnerRadiusRatio()
                                                : _stampTool->getInnerRadius())),
            .cursor_position_override = _stampTool->mapStampPosition({}),
            .use_cursor_position_override = !painted_selection && !exclusion_brush
                && _stampTool->isMapStampPositionLocked(),
            .show_stamp_protection = !painted_selection && (active_map_stamp || exclusion_brush)
                && _stampTool->isMapStampProtectionVisible(),
            .stamp_protection_center = _stampTool->mapStampProtectionOverlayCenter(),
            .stamp_protection_radius = _stampTool->mapStampProtectionOverlayRadius(),
            .cursor_type = cursor_type,
            .cursor_color = painted_selection ? glm::vec4{.08f, .35f, 1.f, 1.f}
                : (exclusion_brush ? glm::vec4{1.f, .56f, .69f, 1.f}
                                   : glm::vec4{1.f, 1.f, 1.f, 1.f}),
            .show_painted_stamp_selection = painted_selection,
            .stamp_height_preview_lines = _stampTool->mapStampHeightPreviewLines(),
        };
    }

    void StampTool::onSelected()
    {
        if (_stampTool->hasActiveMapStamp())
        {
            _stampTool->deactivateMapStampExclusionBrush();
            _stampTool->updateMapStampPreview();
        }
        else if (_stampTool->getActiveBrushItem() && _stampTool->getActiveBrushItem()->isEnabled())
        {
            _stampTool->getActiveBrushItem()->updateMask();
        }
    }

    void StampTool::onDeselected()
    {
        endStampRotationDrag();
        _stampTool->endMapStampHeightDrag();
        _stampTool->clearMapStampTerrainPreview();
        _stampTool->deactivateMapStampExclusionBrush();
        _stampTool->deactivateMapStampPaintedSelection();
    }

    void StampTool::onTick(float deltaTime, TickParameters const& params)
    {
        auto mv = mapView();
        auto world = mv->getWorld();
        _stampTool->updateMapStampTerrainPreview(mv->cursorPosition(), world);

        if (_stamp_rotation_drag_active)
            return;

        if (_stampTool->isMapStampPaintedSelectionEnabled())
        {
            if (params.left_mouse && !params.underMap
                && params.displayMode == display_mode::in_3D
                && !params.mod_shift_down && !params.mod_alt_down)
            {
                _stampTool->paintMapStampSelection(mv->cursorPosition(), params.mod_ctrl_down);
            }
            return;
        }

        if ((!mapView()->getWorld()->has_selection()
             && !_stampTool->isMapStampPositionLocked()) || !params.left_mouse)
        {
            return;
        }

        if (_stampTool->isMapStampExclusionBrushEnabled())
        {
            if (params.displayMode == display_mode::in_3D && !params.mod_shift_down
                && !params.mod_alt_down && !params.mod_ctrl_down)
            {
                _stampTool->paintMapStampProtection(mv->cursorPosition());
            }
            return;
        }

        if (_stampTool->hasActiveMapStamp())
        {
            if (params.displayMode == display_mode::in_3D && params.mod_shift_down
                && !params.mod_alt_down && !params.mod_ctrl_down
                && !_stampTool->isMapStampHeightDragActive())
            {
                auto action = NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eNO_FLAG,
                    Noggit::ActionModalityControllers::eSHIFT
                    | Noggit::ActionModalityControllers::eLMB);
                auto const tag = reinterpret_cast<std::uintptr_t>(_stampTool);
                if (!action->checkAdressTag(tag))
                {
                    action->setBlockCursor(true);
                    if (_stampTool->executeMapStamp(mv->cursorPosition(), world))
                    {
                        action->tagAdress(tag);
                        action->setPostCallback([this] { randomizeStampRotation(); });
                    }
                }
            }
            return;
        }

        for (auto& selection : world->current_selection())
        {
            if (selection.index() != eEntry_MapChunk
                || params.displayMode != display_mode::in_3D
                || !(params.mod_shift_down || params.mod_ctrl_down || params.mod_alt_down)
                || !_stampTool->getBrushMode())
            {
                continue;
            }

            auto action = NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eNO_FLAG,
                Noggit::ActionModalityControllers::eSHIFT
                | Noggit::ActionModalityControllers::eLMB);

            if (!_stampTool->getBrushMode())
                action->setBlockCursor(true);

            _stampTool->execute(mv->cursorPosition(),
                world, deltaTime,
                params.mod_shift_down,
                params.mod_alt_down,
                params.mod_ctrl_down,
                world->isUnderMap(mv->cursorPosition()));
        }
    }

    void StampTool::onMousePress(MousePressParameters const& params)
    {
        if (_stamp_rotation_drag_active)
            return;
        if (_stampTool->isMapStampPaintedSelectionEnabled())
            return;
        if (params.button == Qt::RightButton && _stampTool->isMapStampHeightDragActive())
        {
            _stampTool->endMapStampHeightDrag();
            return;
        }

        if (params.button == Qt::LeftButton && !params.mod_shift_down
            && !params.mod_alt_down && !params.mod_space_down)
        {
            _stampTool->beginMapStampHeightDrag(mapView()->cursorPosition());
        }
    }

    void StampTool::onMouseRelease(MouseReleaseParameters const& params)
    {
        if (params.button == Qt::LeftButton)
            _stampTool->endMapStampSelectionStroke();
        if (params.button != Qt::LeftButton || !_stampTool->isMapStampHeightDragActive())
            return;

        auto* action = NOGGIT_ACTION_MGR->beginAction(
            mapView(), Noggit::ActionFlags::eNO_FLAG,
            Noggit::ActionModalityControllers::eNONE);
        action->setBlockCursor(true);
        if (_stampTool->commitMapStampHeightDrag(mapView()->getWorld()))
            action->setPostCallback([this] { randomizeStampRotation(); });
        NOGGIT_ACTION_MGR->endAction();
    }

    void StampTool::onMouseMove(MouseMoveParameters const& params)
    {
        if (_stamp_rotation_drag_active)
        {
            if (params.displayMode == display_mode::in_3D
                && !_stampTool->isMapStampPaintedSelectionEnabled()
                && !_stampTool->isMapStampExclusionBrushEnabled())
            {
                float const sensitivity = params.mod_ctrl_down ? .125f : (10.f / XSENS);
                _stamp_rotation_drag_remainder -=
                    static_cast<float>(params.relative_movement.dx()) * sensitivity;
                int const whole_degrees = static_cast<int>(_stamp_rotation_drag_remainder);
                if (whole_degrees != 0)
                {
                    _stampTool->changeRotation(whole_degrees);
                    _stamp_rotation_drag_remainder -= whole_degrees;
                }
            }
            return;
        }

        if (_stampTool->isMapStampHeightDragActive())
        {
            if (params.left_mouse && params.displayMode == display_mode::in_3D
                && !params.mod_shift_down && !params.mod_alt_down && !params.mod_space_down)
            {
                _stampTool->adjustMapStampHeightDrag(
                    -static_cast<float>(params.relative_movement.dy()), params.mod_ctrl_down);
            }
            return;
        }

        auto mv = mapView();
        if (params.right_mouse)
        {
            if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
            {
                _stampTool->changeInnerRadius(params.relative_movement.dx() / 300.0f);
            }

            if (params.mod_space_down)
            {
                auto action = NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eDO_NOT_WRITE_HISTORY,
                    Noggit::ActionModalityControllers::eRMB
                    | Noggit::ActionModalityControllers::eSPACE);

                _stampTool->changeRotation(-params.relative_movement.dx() / XSENS * 10.f);
                action->setBlockCursor(true);
            }
        }

        if (params.left_mouse)
        {
            if (params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
            {
                if (_stampTool->isMapStampPaintedSelectionEnabled())
                    return;
                else if (_stampTool->isMapStampExclusionBrushEnabled())
                    _stampTool->changeMapStampExclusionBrushRadius(
                        params.relative_movement.dx() / XSENS);
                else
                    _stampTool->changeRadius(params.relative_movement.dx() / XSENS);
            }

            if (params.mod_space_down)
            {
                _stampTool->changeSpeed(params.relative_movement.dx() / XSENS);
            }

            if (params.mod_shift_down)
            {
                if(params.displayMode == display_mode::in_3D && !_stampTool->getBrushMode()
                    && !_stampTool->hasActiveMapStamp())
                {
                    auto action = NOGGIT_ACTION_MGR->beginAction(mv, Noggit::ActionFlags::eNO_FLAG,
                        Noggit::ActionModalityControllers::eSHIFT
                        | Noggit::ActionModalityControllers::eLMB);

                    action->setPostCallback([this] { randomizeStampRotation(); });
                    action->setBlockCursor(true);

                    _stampTool->execute(mv->cursorPosition()
                        , mv->getWorld()
                        , params.relative_movement.dx() / 30.0f
                        , params.mod_shift_down
                        , params.mod_alt_down
                        , params.mod_ctrl_down
                        , false);
                }
            }
        }
    }

    void StampTool::onMouseWheel(MouseWheelParameters const& params)
    {
        if (_stamp_rotation_drag_active || !_stampTool->hasActiveMapStamp()
            || _stampTool->isMapStampExclusionBrushEnabled()
            || params.mod_shift_down || params.mod_alt_down || params.mod_space_down)
        {
            return;
        }

        int const delta = params.event.angleDelta().y();
        if (delta == 0)
            return;

        int const step = params.mod_ctrl_down ? 1 : 15;
        _stampTool->changeRotation(delta > 0 ? step : -step);
        params.event.accept();
    }

    void StampTool::onFocusLost()
    {
        endStampRotationDrag();
        _stampTool->endMapStampHeightDrag();
        _stampTool->endMapStampSelectionStroke();
    }

    void StampTool::endStampRotationDrag()
    {
        if (!_stamp_rotation_drag_active)
            return;

        _stamp_rotation_drag_active = false;
        _stamp_rotation_drag_remainder = 0.f;
        auto* const action = NOGGIT_CUR_ACTION;
        auto const tag = reinterpret_cast<std::uintptr_t>(this);
        if (action && action->checkAdressTag(tag))
            NOGGIT_ACTION_MGR->endAction();
    }

    void StampTool::randomizeStampRotation()
    {
        if (!_stampTool->getRandomizeRotation())
            return;

        unsigned int ms = static_cast<unsigned>(QDateTime::currentMSecsSinceEpoch());
        std::mt19937 gen(ms);
        std::uniform_int_distribution<> uid(0, 359);

        _stampTool->setRotation(uid(gen));
    }
}
