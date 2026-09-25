// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "AreaTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/MapChunk.h>
#include <noggit/MapView.h>
#include <noggit/Input.hpp>
#include <noggit/ui/ZoneIDBrowser.h>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/World.h>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>

namespace Noggit
{
    AreaTool::AreaTool(MapView* mapView)
        : Tool{ mapView }
    {
        addHotkey("setAreaId"_hash, {
            .onPress = [=] {
              if (_selectedAreaId != -1)
              {
                NOGGIT_ACTION_MGR->beginAction(mapView, Noggit::ActionFlags::eCHUNKS_AREAID);
                mapView->getWorld()->setAreaID(mapView->getCamera()->position, _selectedAreaId, true);
                NOGGIT_ACTION_MGR->endAction();
              }},
            .condition = [=] { return mapView->get_editing_mode() == editing_mode::areaid && !NOGGIT_CUR_ACTION; }
            });
    }

    AreaTool::~AreaTool()
    {
    }

    char const* AreaTool::name() const
    {
        return "Area Designator";
    }

    editing_mode AreaTool::editingMode() const
    {
        return editing_mode::areaid;
    }

    Ui::FontNoggit::Icons AreaTool::icon() const
    {
        return Ui::FontNoggit::TOOL_AREA_DESIGNATOR;
    }

    void AreaTool::setupUi(Ui::Tools::ToolPanel* toolPanel)
    {
        _areaTool = new Noggit::Ui::zone_id_browser(mapView());
        toolPanel->registerTool(this, _areaTool);

        _areaTool->setMapID(mapView()->getWorld()->getMapID());
        QObject::connect(_areaTool, &Noggit::Ui::zone_id_browser::selected
            , _areaTool, [this](int area_id) { setSelectedAreaId(area_id); }
        );

        auto* selected_area_row = new QWidget(_areaTool);
        auto* selected_area_layout = new QHBoxLayout(selected_area_row);
        selected_area_layout->setContentsMargins(0, 0, 0, 0);
        _selectedAreaLabel = new QLabel("Selected Area ID: None", selected_area_row);
        _selectedAreaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        _copyAreaIdButton = new QPushButton("Copy ID", selected_area_row);
        _copyAreaIdButton->setEnabled(false);
        selected_area_layout->addWidget(_selectedAreaLabel);
        selected_area_layout->addWidget(_copyAreaIdButton);
        _areaTool->layout()->addWidget(selected_area_row);
        QObject::connect(_copyAreaIdButton, &QPushButton::clicked, _areaTool,
            [this]
            {
                if (_selectedAreaId >= 0)
                    QApplication::clipboard()->setText(QString::number(_selectedAreaId));
            });

        auto* set_current_adt_button = new QPushButton("Set Area ID on current ADT", _areaTool);
        _areaTool->layout()->addWidget(set_current_adt_button);
        QObject::connect(set_current_adt_button, &QPushButton::clicked, _areaTool,
            [this]
            {
                if (hotkeyCondition("setAreaId"_hash))
                    onHotkeyPress("setAreaId"_hash);
            });
    }

    ToolDrawParameters AreaTool::drawParameters() const
    {
        return
        {
            .radius = _areaTool->brushRadius(),
            .cursor_type = CursorType::SQUARE,
        };
    }

    void AreaTool::setSelectedAreaId(int areaId)
    {
        _selectedAreaId = areaId;
        _selectedAreaLabel->setText(areaId >= 0
            ? QString("Selected Area ID: %1").arg(areaId)
            : QString("Selected Area ID: None"));
        _copyAreaIdButton->setEnabled(areaId >= 0);
    }

    void AreaTool::registerMenuItems(QMenu* menu)
    {
        addMenuTitle(menu, "Area Designator");
        addMenuItem(menu, "Set Area ID", [=] {
            if (_selectedAreaId == -1)
            {
                return;
            }

            NOGGIT_ACTION_MGR->beginAction(mapView(), Noggit::ActionFlags::eCHUNKS_AREAID);
            mapView()->getWorld()->setAreaID(mapView()->getCamera()->position, _selectedAreaId, true);
            NOGGIT_ACTION_MGR->endAction();
            });
    }

    void AreaTool::onSelected()
    {
        mapView()->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_areaid_overlay = true;
    }

    void AreaTool::onDeselected()
    {
        mapView()->getWorld()->renderer()->getTerrainParamsUniformBlock()->draw_areaid_overlay = false;
    }

    void AreaTool::onTick(float deltaTime, TickParameters const& params)
    {
        if (_selectedAreaId == -1
            || !mapView()->getWorld()->has_selection()
            || params.underMap
            || !params.left_mouse
            || !params.mod_shift_down)
        {
            return;
        }

        NOGGIT_ACTION_MGR->beginAction(mapView(), Noggit::ActionFlags::eCHUNKS_AREAID,
            Noggit::ActionModalityControllers::eSHIFT
            | Noggit::ActionModalityControllers::eLMB);
        mapView()->getWorld()->setAreaID(mapView()->cursorPosition(), _selectedAreaId,
                                        false, _areaTool->brushRadius());
    }

    void AreaTool::onMouseMove(MouseMoveParameters const& params)
    {
        if (params.left_mouse && params.mod_alt_down && !params.mod_shift_down && !params.mod_ctrl_down)
        {
            _areaTool->changeRadius(params.relative_movement.dx() / XSENS);
        }
    }

    void AreaTool::onMousePress(MousePressParameters const& params)
    {
      if (params.button != Qt::LeftButton)
      {
        return;
      }

      if (params.mod_ctrl_down)
      {
        mapView()->doSelection(true);

        for (auto&& selection : mapView()->getWorld()->current_selection())
        {
          MapChunk* chnk(std::get<selected_chunk_type>(selection).chunk);
          int newID = chnk->getAreaID();
          setSelectedAreaId(newID);
          _areaTool->setZoneID(newID);
        }
        return;
      }
    }
}
