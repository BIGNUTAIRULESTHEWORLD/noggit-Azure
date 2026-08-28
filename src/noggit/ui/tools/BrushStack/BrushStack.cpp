// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "BrushStack.hpp"
#include "BrushStackItem.hpp"
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/rendering/WorldRender.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/ui/FlattenTool.hpp>
#include <noggit/ui/ShaderTool.hpp>
#include <noggit/ui/TerrainTool.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/tools/UiCommon/ExtendedSlider.hpp>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRadialGradient>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

using namespace Noggit::Ui::Tools;


BrushStack::BrushStack(MapView* map_view, QWidget* parent)
: QWidget(parent)
, _map_view(map_view)
{
  _ui.setupUi(this);
  layout()->setAlignment(Qt::AlignTop);
  setMinimumWidth(250);
  _ui.brushRotation->setMaximum(359);
  _ui.brushRotation->setToolTip(
      "Stamp rotation in degrees. Hold R and move the mouse horizontally to rotate in the "
      "viewport; hold Ctrl after R for fine control. Space+right-drag remains available.");
  _ui.randomizeRotation->setToolTip(
      "Chooses a new absolute rotation for the next stamp after each successful placement.");
  // setMaximumWidth(250);

  _ui.radiusSlider->setTabletSupportEnabled(false);
  _ui.innerRadiusSlider->setTabletSupportEnabled(false);
  _ui.speedSlider->setTabletSupportEnabled(false);

  _add_popup = new QWidget(this);
  auto _add_popup_layout = new QVBoxLayout(_add_popup);
  _add_operation_combo = new QComboBox(_add_popup);
  _add_operation_combo->addItems({"Raise | Lower",
                                        "Flatten | Blur",
                                        "Texture",
                                        "Shader"});

  _add_popup_layout->addWidget(_add_operation_combo);

  auto okay_button = new QPushButton(_add_popup);
  okay_button->setText("Okay");
  _add_popup_layout->addWidget(okay_button);

  _active_item_button_group = new QButtonGroup(this);

  setupMapStampUi();


  connect(okay_button, &QPushButton::clicked,
          [=]()
          {
            auto brush_stack_item = new BrushStackItem(this);
            _ui.brushList->layout()->addWidget(brush_stack_item);

            switch (_add_operation_combo->currentIndex())
            {
              case eTools::eRaiseLower:
                brush_stack_item->setTool(new Noggit::Ui::TerrainTool(_map_view, this, true));
                break;
              case eTools::eFlattenBlur:
                brush_stack_item->setTool(new Noggit::Ui::flatten_blur_tool(this));
                break;
              case eTools::eTexturing:
                brush_stack_item->setTool(new Noggit::Ui::texturing_tool(&_map_view->getCamera()->position, _map_view, nullptr, this));
                break;
              case eTools::eShader:
                brush_stack_item->setTool(new Noggit::Ui::ShaderTool(_map_view, this));
                break;
            }

            addAction(brush_stack_item);
          });

  _add_popup->updateGeometry();
  _add_popup->adjustSize();
  _add_popup->update();
  _add_popup->repaint();
  _add_popup->setVisible(false);

  connect(_ui.addBrushButton, &QPushButton::clicked,
          [=]()
          {
            QPoint new_pos = mapToGlobal(
              QPoint(_ui.addBrushButton->pos().x() - _add_popup->width() - 12,
                     _ui.addBrushButton->pos().y()));

            _add_popup->setGeometry(new_pos.x(),
                                    new_pos.y(),
                                    _add_popup->width(),
                                    _add_popup->height());

            _add_popup->setWindowFlags(Qt::Popup);
            _add_popup->show();
          });

  connect(_ui.clearBrushesButton, &QPushButton::clicked,
          [=]()
          {
            QLayoutItem* item;
            while( (item = _ui.brushList->layout()->takeAt(0)) != nullptr)
            {
              _active_item_button_group->removeButton(static_cast<BrushStackItem*>(item->widget())->getActiveButton());

              item->widget()->deleteLater();
              delete item;
            }

            _active_item = nullptr;
          });

  connect(_ui.radiusSlider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged,
          [this](double value)
          {
            for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
            {
              BrushStackItem* item {reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())};

              if (!item->isRadiusAffecting())
                continue;

              reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())->setRadius(value);
            }
            markMapStampTerrainPreviewDirty();
          });

  connect(_ui.innerRadiusSlider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged,
          [this](double value)
          {
            for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
            {
              BrushStackItem* item {reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())};

              if (!item->isInnerRadiusAffecting())
                continue;

              reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())->setInnerRadius(value);
            }
            markMapStampTerrainPreviewDirty();
          });

  connect(_ui.speedSlider, &Noggit::Ui::Tools::UiCommon::ExtendedSlider::valueChanged,
          [this](double value)
          {
            for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
            {
              BrushStackItem* item {reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())};

              if (!item->isSpeedAffecting())
                continue;

              reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())->setSpeed(value);
            }
          });


  connect(_ui.brushRotation, &QDial::valueChanged,
          [this](int value)
          {
            if (_map_stamp_rotation && _map_stamp_rotation->value() != value)
            {
              QSignalBlocker const blocker(_map_stamp_rotation);
              _map_stamp_rotation->setValue(value);
            }
            for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
            {
              BrushStackItem* item {reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())};

              if (!item->isMaskRotationAffecting())
                continue;

              reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())->setMaskRotation(value);
            }
            if (hasActiveMapStamp())
              updateMapStampPreview();
            markMapStampTerrainPreviewDirty();
          });


  connect(_ui.sculptRadio, &QRadioButton::clicked,
          [this](bool checked)
          {
            for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
            {
              reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())->setBrushMode(checked);
            }
          });

  connect(_ui.stampRadio, &QRadioButton::clicked,
          [this](bool checked)
          {
            for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
            {
              reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())->setBrushMode(!checked);
            }
          });

}

void BrushStack::setupMapStampUi()
{
  auto* group = new QGroupBox("Stamp workflow", this);
  auto* group_layout = new QVBoxLayout(group);
  group_layout->setContentsMargins(8, 8, 8, 8);
  group_layout->setSpacing(6);

  _map_stamp_enabled = new QCheckBox("Map stamp mode", group);
  _map_stamp_enabled->setToolTip(
      "Switches between captured terrain stamps and the legacy brush-stack editor.");
  group_layout->addWidget(_map_stamp_enabled);

  _map_stamp_options = new QWidget(group);
  auto* options_layout = new QVBoxLayout(_map_stamp_options);
  options_layout->setContentsMargins(0, 0, 0, 0);
  options_layout->setSpacing(6);

  auto* library_row = new QHBoxLayout();
  _map_stamp_library = new QComboBox(_map_stamp_options);
  _map_stamp_library->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  _map_stamp_library->setMinimumContentsLength(10);
  auto* remove = new QPushButton("Delete", _map_stamp_options);
  library_row->addWidget(_map_stamp_library, 1);
  library_row->addWidget(remove);
  options_layout->addLayout(library_row);

  auto* placement_form = new QFormLayout();
  placement_form->setContentsMargins(0, 0, 0, 0);
  placement_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  _map_stamp_radius = new UiCommon::ExtendedSlider(_map_stamp_options);
  _map_stamp_radius->setPrefix("");
  _map_stamp_radius->setRange(0.1, 1000.0);
  _map_stamp_radius->setDecimals(2);
  _map_stamp_radius->setValue(_ui.radiusSlider->value());
  _map_stamp_radius->setTabletSupportEnabled(false);
  _map_stamp_radius->setToolTip(
      "Exact and Mountain modes: the preserved feature radius with an outside blend skirt. "
      "Terrain Conform: the total footprint radius.");
  placement_form->addRow("Size", _map_stamp_radius);

  _map_stamp_height_mode = new QComboBox(_map_stamp_options);
  _map_stamp_height_mode->addItems({"Exact feature", "Mountain blend", "Terrain conform"});
  _map_stamp_height_mode->setToolTip(
      "Exact feature snaps the captured base to the terrain beneath the cursor, preserves core "
      "height differences, and blends outward from the grounded boundary. Mountain blend adds only "
      "positive mountain relief and merges overlapping features without digging. Terrain conform "
      "transfers signed relief and may intentionally raise or lower terrain.");
  placement_form->addRow("Height mode", _map_stamp_height_mode);

  _map_stamp_height_scale = new QDoubleSpinBox(_map_stamp_options);
  _map_stamp_height_scale->setRange(-10.0, 10.0);
  _map_stamp_height_scale->setDecimals(2);
  _map_stamp_height_scale->setSingleStep(.1);
  _map_stamp_height_scale->setValue(1.0);
  _map_stamp_height_scale->setToolTip(
      "Multiplies the captured height profile. Elevation moves the whole stamp vertically.");
  placement_form->addRow("Height scale", _map_stamp_height_scale);

  _map_stamp_opacity = new QDoubleSpinBox(_map_stamp_options);
  _map_stamp_opacity->setRange(.01, 1.0);
  _map_stamp_opacity->setDecimals(2);
  _map_stamp_opacity->setSingleStep(.05);
  _map_stamp_opacity->setValue(1.0);
  _map_stamp_opacity->setToolTip(
      "Overall stamp strength. Keep at 1.0 to preserve exact-feature heights in the core.");
  placement_form->addRow("Overall opacity", _map_stamp_opacity);

  auto* elevation_row = new QWidget(_map_stamp_options);
  auto* elevation_layout = new QHBoxLayout(elevation_row);
  elevation_layout->setContentsMargins(0, 0, 0, 0);
  _map_stamp_height_offset = new QDoubleSpinBox(elevation_row);
  _map_stamp_height_offset->setRange(-10000.0, 10000.0);
  _map_stamp_height_offset->setDecimals(2);
  _map_stamp_height_offset->setSingleStep(.1);
  _map_stamp_height_offset->setToolTip(
      "Moves the entire captured terrain profile up or down in world units.");
  auto* reset_elevation = new QPushButton("Reset", elevation_row);
  elevation_layout->addWidget(_map_stamp_height_offset, 1);
  elevation_layout->addWidget(reset_elevation);
  placement_form->addRow("Elevation", elevation_row);

  auto* rotation_row = new QWidget(_map_stamp_options);
  auto* rotation_layout = new QHBoxLayout(rotation_row);
  rotation_layout->setContentsMargins(0, 0, 0, 0);
  _map_stamp_rotation = new QSpinBox(rotation_row);
  _map_stamp_rotation->setRange(0, 359);
  _map_stamp_rotation->setWrapping(true);
  _map_stamp_rotation->setSuffix(" deg");
  _map_stamp_rotation->setToolTip(
      "Rotates terrain and textures. Hold R and move the mouse horizontally; hold Ctrl after "
      "R for fine control. Mouse wheel uses 15 degree steps; Ctrl+wheel uses 1.");
  _map_stamp_rotation->setEnabled(false);
  auto* reset_rotation = new QPushButton("Reset", rotation_row);
  rotation_layout->addWidget(_map_stamp_rotation, 1);
  rotation_layout->addWidget(reset_rotation);
  placement_form->addRow("Rotation", rotation_row);
  options_layout->addLayout(placement_form);

  _map_stamp_height_drag = new QCheckBox("Drag elevation: release to paste", _map_stamp_options);
  _map_stamp_height_drag->setToolTip(
      "Plain Left-drag adjusts a textured wireframe preview. Release Left to paste once; "
      "Right-click cancels. Ctrl makes fine adjustments.");
  options_layout->addWidget(_map_stamp_height_drag);

  _map_stamp_randomize_rotation = new QCheckBox("Randomize rotation after placement",
                                                _map_stamp_options);
  options_layout->addWidget(_map_stamp_randomize_rotation);

  auto* capture_group = new QGroupBox("Capture new stamp", _map_stamp_options);
  capture_group->setCheckable(true);
  capture_group->setChecked(false);
  auto* capture_body = new QWidget(capture_group);
  auto* capture_layout = new QVBoxLayout(capture_group);
  capture_layout->setContentsMargins(8, 4, 8, 8);
  auto* capture_body_layout = new QVBoxLayout(capture_body);
  capture_body_layout->setContentsMargins(0, 0, 0, 0);
  _map_stamp_shape = new QComboBox(capture_body);
  _map_stamp_shape->addItems({"Circle footprint", "Square footprint", "Painted footprint"});
  _map_stamp_shape->setToolTip(
      "Chooses the footprint for the next captured stamp. Painted footprint lets you brush the "
      "exact source area and automatically captures a reference-only terrain collar around it.");
  _map_stamp_painted_controls = new QWidget(capture_body);
  auto* painted_layout = new QVBoxLayout(_map_stamp_painted_controls);
  painted_layout->setContentsMargins(0, 0, 0, 0);
  _map_stamp_painted_selection = new QCheckBox("Paint source area", _map_stamp_painted_controls);
  _map_stamp_painted_selection->setToolTip(
      "Left-drag paints the source footprint blue. Ctrl+drag erases. No terrain changes while "
      "selection painting is active.");
  _map_stamp_painted_radius = new UiCommon::ExtendedSlider(_map_stamp_painted_controls);
  _map_stamp_painted_radius->setPrefix("Brush radius");
  _map_stamp_painted_radius->setRange(1.0, 500.0);
  _map_stamp_painted_radius->setDecimals(2);
  _map_stamp_painted_radius->setValue(20.0);
  auto* clear_painted = new QPushButton("Clear painted selection", _map_stamp_painted_controls);
  painted_layout->addWidget(_map_stamp_painted_selection);
  painted_layout->addWidget(_map_stamp_painted_radius);
  painted_layout->addWidget(clear_painted);
  _map_stamp_painted_controls->setVisible(false);
  auto* capture = new QPushButton("Capture at cursor", capture_body);
  capture_body_layout->addWidget(_map_stamp_shape);
  capture_body_layout->addWidget(_map_stamp_painted_controls);
  capture_body_layout->addWidget(capture);
  capture_layout->addWidget(capture_body);
  capture_body->setVisible(false);
  connect(capture_group, &QGroupBox::toggled, capture_body, &QWidget::setVisible);
  options_layout->addWidget(capture_group);

  auto* advanced_group = new QGroupBox("Advanced placement", _map_stamp_options);
  advanced_group->setCheckable(true);
  advanced_group->setChecked(false);
  auto* advanced_body = new QWidget(advanced_group);
  auto* advanced_layout = new QVBoxLayout(advanced_group);
  advanced_layout->setContentsMargins(8, 4, 8, 8);
  auto* advanced_body_layout = new QFormLayout(advanced_body);
  advanced_body_layout->setContentsMargins(0, 0, 0, 0);
  _map_stamp_position_lock = new QCheckBox("Lock position (F)", advanced_body);
  _map_stamp_position_lock->setToolTip(
      "F captures and locks the current map position. Space+F toggles the existing lock.");
  advanced_body_layout->addRow(_map_stamp_position_lock);
  _map_stamp_edge_blend = new QDoubleSpinBox(advanced_body);
  _map_stamp_edge_blend->setRange(.05, .50);
  _map_stamp_edge_blend->setDecimals(2);
  _map_stamp_edge_blend->setSingleStep(.05);
  _map_stamp_edge_blend->setValue(.25);
  _map_stamp_edge_blend->setToolTip(
      "Exact mode and painted footprints place this transition outside the preserved core. "
      "Regular conforming circles/squares place it inside Size. Heights and textures use the "
      "same transition.");
  advanced_body_layout->addRow("Edge blend", _map_stamp_edge_blend);
  advanced_layout->addWidget(advanced_body);
  advanced_body->setVisible(false);
  connect(advanced_group, &QGroupBox::toggled, advanced_body, &QWidget::setVisible);
  options_layout->addWidget(advanced_group);

  auto* protection_group = new QGroupBox("Terrain protection", _map_stamp_options);
  protection_group->setCheckable(true);
  protection_group->setChecked(false);
  auto* protection_body = new QWidget(protection_group);
  auto* protection_group_layout = new QVBoxLayout(protection_group);
  protection_group_layout->setContentsMargins(8, 4, 8, 8);
  auto* protection_layout = new QVBoxLayout(protection_body);
  protection_layout->setContentsMargins(0, 0, 0, 0);

  _map_stamp_auto_protection = new QCheckBox("Protect existing mountains", protection_body);
  _map_stamp_auto_protection->setChecked(false);
  _map_stamp_auto_protection->setToolTip(
      "Automatically excludes steep slopes and terrain rising far above the local surface.");
  protection_layout->addWidget(_map_stamp_auto_protection);

  auto* protection_form = new QFormLayout();
  _map_stamp_protection_slope = new QDoubleSpinBox(protection_body);
  _map_stamp_protection_slope->setRange(0.0, 85.0);
  _map_stamp_protection_slope->setValue(32.0);
  _map_stamp_protection_slope->setSuffix(" deg");
  _map_stamp_protection_relief = new QDoubleSpinBox(protection_body);
  _map_stamp_protection_relief->setRange(0.0, 500.0);
  _map_stamp_protection_relief->setValue(18.0);
  protection_form->addRow("Slope", _map_stamp_protection_slope);
  protection_form->addRow("Relief", _map_stamp_protection_relief);
  protection_layout->addLayout(protection_form);

  _map_stamp_show_protection = new QCheckBox("Show manual exclusions", protection_body);
  _map_stamp_show_protection->setChecked(true);
  auto* clear_protection = new QPushButton("Clear manual exclusions", protection_body);
  protection_layout->addWidget(_map_stamp_show_protection);
  protection_layout->addWidget(clear_protection);

  _map_stamp_exclusion_brush = new QGroupBox("Paint exclusions", protection_body);
  _map_stamp_exclusion_brush->setCheckable(true);
  _map_stamp_exclusion_brush->setChecked(false);
  _map_stamp_exclusion_brush->setToolTip(
      "Temporarily replaces stamp placement with a dedicated exclusion-painting brush.");
  auto* exclusion_body = new QWidget(_map_stamp_exclusion_brush);
  auto* exclusion_group_layout = new QVBoxLayout(_map_stamp_exclusion_brush);
  exclusion_group_layout->setContentsMargins(8, 4, 8, 8);
  auto* exclusion_layout = new QVBoxLayout(exclusion_body);
  exclusion_layout->setContentsMargins(0, 0, 0, 0);

  _map_stamp_exclusion_radius = new UiCommon::ExtendedSlider(exclusion_body);
  _map_stamp_exclusion_radius->setPrefix("Radius");
  _map_stamp_exclusion_radius->setRange(1.0, 1000.0);
  _map_stamp_exclusion_radius->setDecimals(2);
  _map_stamp_exclusion_radius->setValue(20.0);
  exclusion_layout->addWidget(_map_stamp_exclusion_radius);

  auto* exclusion_shape_row = new QHBoxLayout();
  auto* exclusion_circle = new QRadioButton("Circle", exclusion_body);
  auto* exclusion_square = new QRadioButton("Square", exclusion_body);
  _map_stamp_exclusion_shape = new QButtonGroup(exclusion_body);
  _map_stamp_exclusion_shape->addButton(exclusion_circle,
      static_cast<int>(BrushShape::CIRCLE));
  _map_stamp_exclusion_shape->addButton(exclusion_square,
      static_cast<int>(BrushShape::SQUARE));
  exclusion_circle->setChecked(true);
  exclusion_shape_row->addWidget(exclusion_circle);
  exclusion_shape_row->addWidget(exclusion_square);
  exclusion_layout->addLayout(exclusion_shape_row);

  auto* exclusion_operation_row = new QHBoxLayout();
  auto* exclusion_paint = new QRadioButton("Paint", exclusion_body);
  auto* exclusion_erase = new QRadioButton("Erase", exclusion_body);
  _map_stamp_exclusion_operation = new QButtonGroup(exclusion_body);
  _map_stamp_exclusion_operation->addButton(exclusion_paint, 0);
  _map_stamp_exclusion_operation->addButton(exclusion_erase, 1);
  exclusion_paint->setChecked(true);
  exclusion_operation_row->addWidget(exclusion_paint);
  exclusion_operation_row->addWidget(exclusion_erase);
  exclusion_layout->addLayout(exclusion_operation_row);
  exclusion_group_layout->addWidget(exclusion_body);
  exclusion_body->setVisible(false);
  connect(_map_stamp_exclusion_brush, &QGroupBox::toggled,
          exclusion_body, &QWidget::setVisible);
  protection_layout->addWidget(_map_stamp_exclusion_brush);

  protection_group_layout->addWidget(protection_body);
  protection_body->setVisible(false);
  connect(protection_group, &QGroupBox::toggled, protection_body, &QWidget::setVisible);
  options_layout->addWidget(protection_group);

  _map_stamp_status = new QLabel(
      "Choose a stamp, then Shift+Left to place it or enable Drag elevation.",
      _map_stamp_options);
  _map_stamp_status->setWordWrap(true);
  options_layout->addWidget(_map_stamp_status);

  group_layout->addWidget(_map_stamp_options);
  _map_stamp_options->setVisible(false);

  if (auto* root = qobject_cast<QVBoxLayout*>(layout()))
    root->insertWidget(1, group);

  refreshMapStampLibrary();

  connect(_map_stamp_radius, &UiCommon::ExtendedSlider::valueChanged, this,
          [this](double value)
  {
    if (_ui.radiusSlider->value() != value)
      _ui.radiusSlider->setValue(value);
    markMapStampTerrainPreviewDirty();
  });
  connect(_ui.radiusSlider, &UiCommon::ExtendedSlider::valueChanged, this,
          [this](double value)
  {
    if (_map_stamp_radius->value() != value)
      _map_stamp_radius->setValue(value);
  });
  connect(_map_stamp_rotation, qOverload<int>(&QSpinBox::valueChanged), this,
          [this](int rotation) { setRotation(rotation); });
  connect(reset_rotation, &QPushButton::clicked, this, [this] { setRotation(0); });
  connect(_map_stamp_height_mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) { markMapStampTerrainPreviewDirty(); });
  connect(_map_stamp_height_scale, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) { markMapStampTerrainPreviewDirty(false); });
  connect(_map_stamp_height_offset, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) { markMapStampTerrainPreviewDirty(false); });
  connect(_map_stamp_opacity, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) { markMapStampTerrainPreviewDirty(); });
  connect(_map_stamp_edge_blend, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) { markMapStampTerrainPreviewDirty(); });
  connect(reset_elevation, &QPushButton::clicked, this,
          [this] { _map_stamp_height_offset->setValue(0.0); });
  connect(_map_stamp_height_drag, &QCheckBox::toggled, this, [this](bool enabled)
  {
    endMapStampHeightDrag();
    if (!enabled)
      clearMapStampTerrainPreview();
    else
      markMapStampTerrainPreviewDirty();
    _map_stamp_status->setText(enabled
        ? "Elevation drag armed. Plain Left-drag adjusts a flicker-safe preview; release to "
          "paste once and unlock. Right-click cancels; Ctrl makes fine adjustments."
        : "Elevation drag off. Shift+Left places the stamp normally.");
  });
  for (QDoubleSpinBox* protection_control
       : {_map_stamp_protection_slope, _map_stamp_protection_relief})
  {
    connect(protection_control, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) { markMapStampTerrainPreviewDirty(); });
  }
  connect(_map_stamp_auto_protection, &QCheckBox::toggled, this,
          [this](bool) { markMapStampTerrainPreviewDirty(); });
  connect(_map_stamp_library, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int)
  {
    QString const path = _map_stamp_library->currentData().toString();
    if (!path.isEmpty())
      loadMapStamp(path);
  });
  connect(_map_stamp_enabled, &QCheckBox::toggled, this, [this](bool enabled)
  {
    _map_stamp_options->setVisible(enabled);
    _ui.widget->setVisible(!enabled);
    _ui.brushList->setVisible(!enabled);
    if (enabled)
    {
      deactivateMapStampExclusionBrush();
      if (hasLoadedMapStamp())
        updateMapStampPreview();
      _map_stamp_status->setText(
          hasLoadedMapStamp()
          ? "Map stamp active. Shift+Left places it; Drag elevation previews and pastes on release."
          : "No captured stamp is available. Expand Capture new stamp to create one.");
      _map_view->invalidate();
      _map_view->update();
    }
    else
    {
      endMapStampHeightDrag();
      clearMapStampTerrainPreview();
      deactivateMapStampPaintedSelection();
    }
  });
  connect(_map_stamp_exclusion_brush, &QGroupBox::toggled, this, [this](bool enabled)
  {
    if (enabled)
    {
      deactivateMapStampPaintedSelection();
      endMapStampHeightDrag();
      clearMapStampTerrainPreview();
      _map_stamp_show_protection->setChecked(true);
      updateMapStampPreview();
      _map_stamp_status->setText(
          "Exclusion brush active. Left-drag uses the selected Paint/Erase operation; "
          "Alt+Left-drag changes its radius.");
    }
    else
    {
      _map_stamp_status->setText(
          "Exclusion brush off. The saved-stamp preview and Shift+Left placement are active.");
      if (hasActiveMapStamp())
        updateMapStampPreview();
      markMapStampTerrainPreviewDirty();
    }
    _map_view->invalidate();
    _map_view->update();
  });
  connect(_map_stamp_position_lock, &QCheckBox::toggled, this, [this](bool locked)
  {
    if (locked)
    {
      _map_stamp_locked_position = _map_view->cursorPosition();
      _map_stamp_status->setText(QString("Placement locked at X %1, Z %2. Shift+Left places once.")
          .arg(_map_stamp_locked_position.x, 0, 'f', 1)
          .arg(_map_stamp_locked_position.z, 0, 'f', 1));
    }
    else
      _map_stamp_status->setText(
          "Placement unlocked. Left-drag to preview and paste, or press F to lock manually.");
    markMapStampTerrainPreviewDirty();
  });
  connect(clear_protection, &QPushButton::clicked, this, [this]
  {
    _map_stamp_protection_strokes.clear();
    updateMapStampPreview();
    markMapStampTerrainPreviewDirty();
    _map_stamp_status->setText("Manual protection mask cleared; pink exclusions removed.");
  });
  connect(_map_stamp_shape, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this, capture](int index)
  {
    bool const painted = index == 2;
    _map_stamp_painted_controls->setVisible(painted);
    capture->setText(painted ? "Capture painted selection" : "Capture at cursor");
    if (!painted)
      deactivateMapStampPaintedSelection();
  });
  connect(_map_stamp_painted_selection, &QCheckBox::toggled, this, [this](bool enabled)
  {
    endMapStampHeightDrag();
    clearMapStampTerrainPreview();
    if (enabled)
    {
      deactivateMapStampExclusionBrush();
      _map_stamp_status->setText(
          "Painted capture active. Left-drag paints blue; Ctrl+drag erases. Capture when ready.");
    }
    else
    {
      endMapStampSelectionStroke();
      if (hasLoadedMapStamp())
        updateMapStampPreview();
      _map_stamp_status->setText(_map_stamp_painted_cells.empty()
          ? "Painted capture paused. No source area is selected."
          : QString("Painted capture paused with %1 selected cells.")
              .arg(_map_stamp_painted_cells.size()));
    }
    _map_view->invalidate();
    _map_view->update();
  });
  connect(clear_painted, &QPushButton::clicked, this, [this]
  {
    _map_stamp_painted_cells.clear();
    if (_map_view && _map_view->getWorld())
      _map_view->getWorld()->renderer()->clearPaintedStampSelectionOverlay();
    endMapStampSelectionStroke();
    _map_stamp_status->setText("Painted source selection cleared.");
    _map_view->invalidate();
    _map_view->update();
  });
  connect(capture, &QPushButton::clicked, this, [this]
  {
    bool const painted_shape = _map_stamp_shape->currentIndex() == 2;
    if (painted_shape && _map_stamp_painted_cells.empty())
    {
      _map_stamp_status->setText(
          "Paint a source area first, then choose Capture painted selection.");
      return;
    }
    bool accepted = false;
    QString name = QInputDialog::getText(this, "Capture map-derived stamp", "Stamp name:",
                                          QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty())
      return;
    name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1F]"), "_");
    while (name.endsWith('.') || name.endsWith(' '))
      name.chop(1);
    if (name.isEmpty())
      return;

    QDir directory(QString::fromStdString(
        Noggit::Project::CurrentProject::get()->ProjectPath) + "/noggit-assets/stamps");
    if (!directory.exists() && !directory.mkpath("."))
    {
      _map_stamp_status->setText("Unable to create the project stamp library.");
      return;
    }
    QString const path = directory.filePath(name + ".nogstamp");
    if (QFileInfo::exists(path)
        && QMessageBox::question(this, "Replace map stamp",
             QString("Replace the existing stamp '%1'?").arg(name),
             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
      return;

    _map_stamp_status->setText(QString("Sampling '%1' from the map...").arg(name));
    repaint();
    Stamp::MapStampAsset captured;
    QString error;
    Stamp::MapStampShape const shape = painted_shape ? Stamp::MapStampShape::Painted
        : (_map_stamp_shape->currentIndex() == 1
            ? Stamp::MapStampShape::Square : Stamp::MapStampShape::Circle);
    bool const captured_ok = painted_shape
        ? captured.capture(_map_view->getWorld(),
            Stamp::MapStampPaintedSelection{TEXDETAILSIZE, _map_stamp_painted_cells}, &error)
        : captured.capture(_map_view->getWorld(),
            mapStampPosition(_map_view->cursorPosition()), mapStampRadius(), shape, &error);
    if (!captured_ok
        || !captured.save(path, &error))
    {
      _map_stamp_status->setText(error);
      return;
    }
    clearMapStampTerrainPreview();
    _map_stamp = std::move(captured);
    _map_stamp_height_mode->setEnabled(true);
    _map_stamp_height_mode->setCurrentIndex(0);
    _map_stamp_height_mode->setToolTip(
        "Exact feature preserves the captured shape. Mountain blend safely merges positive relief "
        "without digging. Terrain conform transfers signed relief and may raise or lower terrain.");
    _map_stamp_height_offset->setValue(0.0);
    _map_stamp_radius->setValue(_map_stamp.sourceRadius());
    _map_stamp_position_lock->setChecked(false);
    deactivateMapStampPaintedSelection();
    if (painted_shape)
    {
      _map_stamp_painted_cells.clear();
      _map_view->getWorld()->renderer()->clearPaintedStampSelectionOverlay();
    }
    refreshMapStampLibrary(path);
    _map_stamp_rotation->setEnabled(true);
    _map_stamp_enabled->setChecked(true);
    _map_stamp_status->setText(
        QString("Captured '%1' (%2): grounded feature radius %3, terrain %4x%4, "
                "texture %5x%5, %6 layers.")
          .arg(name).arg(shape == Stamp::MapStampShape::Painted ? "painted"
              : (shape == Stamp::MapStampShape::Square ? "square" : "circle"))
          .arg(_map_stamp.sourceRadius(), 0, 'f', 1)
          .arg(_map_stamp.heightResolution()).arg(_map_stamp.textureResolution())
          .arg(_map_stamp.textureCount()));
    updateMapStampPreview();
  });
  connect(remove, &QPushButton::clicked, this, [this]
  {
    QString const path = _map_stamp_library->currentData().toString();
    if (path.isEmpty())
      return;
    QString const name = QFileInfo(path).completeBaseName();
    if (QMessageBox::question(this, "Delete map stamp",
          QString("Delete '%1' from the project stamp library?").arg(name),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
      return;
    if (!QFile::remove(path))
    {
      _map_stamp_status->setText(QString("Unable to delete '%1'.").arg(name));
      return;
    }
    clearMapStampTerrainPreview();
    _map_stamp = {};
    _map_stamp_height_offset->setValue(0.0);
    _map_stamp_rotation->setEnabled(false);
    refreshMapStampLibrary();
    _map_stamp_status->setText(QString("Deleted '%1'.").arg(name));
  });
}

void BrushStack::refreshMapStampLibrary(QString const& active_path)
{
  QString const previous = active_path.isEmpty() ? _map_stamp_library->currentData().toString()
                                                  : QDir::cleanPath(active_path);
  QSignalBlocker const blocker(_map_stamp_library);
  _map_stamp_library->clear();
  QDir const directory(QString::fromStdString(
      Noggit::Project::CurrentProject::get()->ProjectPath) + "/noggit-assets/stamps");
  QFileInfoList const assets = directory.entryInfoList({"*.nogstamp"}, QDir::Files,
                                                        QDir::Name | QDir::IgnoreCase);
  for (QFileInfo const& asset : assets)
    _map_stamp_library->addItem(asset.completeBaseName(), QDir::cleanPath(asset.absoluteFilePath()));
  if (_map_stamp_library->count() == 0)
  {
    _map_stamp_library->addItem("No captured map stamps");
    _map_stamp_library->setEnabled(false);
    return;
  }
  _map_stamp_library->setEnabled(true);
  int const index = _map_stamp_library->findData(previous);
  _map_stamp_library->setCurrentIndex(index >= 0 ? index : 0);
  QString const selected = _map_stamp_library->currentData().toString();
  if (!selected.isEmpty() && (!_map_stamp.valid() || !active_path.isEmpty()))
    loadMapStamp(selected);
}

bool BrushStack::loadMapStamp(QString const& path)
{
  clearMapStampTerrainPreview();
  QString error;
  Stamp::MapStampAsset loaded;
  if (!loaded.load(path, &error))
  {
    _map_stamp = {};
    _map_stamp_status->setText(error);
    _map_stamp_rotation->setEnabled(false);
    return false;
  }
  _map_stamp = std::move(loaded);
  bool const exact_height = _map_stamp.supportsExactHeight();
  _map_stamp_height_mode->setEnabled(exact_height);
  _map_stamp_height_mode->setCurrentIndex(exact_height ? 0 : 2);
  _map_stamp_height_mode->setToolTip(exact_height
      ? "Exact feature preserves the captured shape. Mountain blend safely merges positive relief "
        "without digging. Terrain conform transfers signed relief and may raise or lower terrain."
      : "This legacy stamp retained only conforming relief. Recapture it to enable Exact feature.");
  _map_stamp_radius->setValue(_map_stamp.sourceRadius());
  _map_stamp_height_scale->setValue(1.0);
  _map_stamp_height_offset->setValue(0.0);
  _map_stamp_shape->setCurrentIndex(_map_stamp.shape() == Stamp::MapStampShape::Painted ? 2
      : (_map_stamp.shape() == Stamp::MapStampShape::Square ? 1 : 0));
  _map_stamp_rotation->setEnabled(true);
  _map_stamp_status->setText(
      QString("Loaded '%1' (%2, %3): radius %4, terrain %5x%5, texture %6x%6, %7 layers.")
        .arg(QFileInfo(path).completeBaseName())
        .arg(_map_stamp.shape() == Stamp::MapStampShape::Painted ? "painted"
            : (_map_stamp.shape() == Stamp::MapStampShape::Square ? "square" : "circle"))
        .arg(exact_height ? "exact + mountain + terrain" : "legacy terrain conform only")
        .arg(_map_stamp.sourceRadius(), 0, 'f', 1)
        .arg(_map_stamp.heightResolution()).arg(_map_stamp.textureResolution())
        .arg(_map_stamp.textureCount()));
  if (_map_stamp_enabled->isChecked())
    updateMapStampPreview();
  return true;
}

bool BrushStack::hasLoadedMapStamp() const
{
  return _map_stamp.valid();
}

bool BrushStack::hasActiveMapStamp() const
{
  return _map_stamp_enabled && _map_stamp_enabled->isChecked() && hasLoadedMapStamp();
}

bool BrushStack::isMapStampProtectionVisible() const
{
  return _map_stamp_show_protection && _map_stamp_show_protection->isChecked();
}

glm::vec3 BrushStack::mapStampProtectionOverlayCenter() const
{
  return _map_stamp_protection_overlay_center;
}

float BrushStack::mapStampProtectionOverlayRadius() const
{
  return _map_stamp_protection_overlay_radius;
}

float BrushStack::mapStampPreviewRadius() const
{
  float const radius = mapStampRadius();
  return hasLoadedMapStamp()
      ? _map_stamp.footprintBoundingRadius(radius, _ui.brushRotation->value(),
                                           mapStampHardness(), mapStampHeightMode())
      : radius;
}

float BrushStack::mapStampInnerRadiusRatio() const
{
  float const edge_blend = 1.f - mapStampHardness();
  return mapStampHeightMode() != Stamp::MapStampHeightMode::ConformToTerrain
      ? 1.f / (1.f + edge_blend) : 1.f - edge_blend;
}

bool BrushStack::isMapStampExclusionBrushEnabled() const
{
  return hasLoadedMapStamp() && _map_stamp_exclusion_brush
      && _map_stamp_exclusion_brush->isChecked();
}

void BrushStack::deactivateMapStampExclusionBrush()
{
  if (_map_stamp_exclusion_brush && _map_stamp_exclusion_brush->isChecked())
  {
    _map_stamp_exclusion_brush->setChecked(false);
    _map_stamp_status->setText(
        "Exclusion brush off. The saved-stamp preview and Shift+Left placement are active.");
  }
}

float BrushStack::mapStampExclusionBrushRadius() const
{
  return _map_stamp_exclusion_radius
      ? static_cast<float>(_map_stamp_exclusion_radius->value()) : 1.f;
}

BrushShape BrushStack::mapStampExclusionBrushShape() const
{
  if (!_map_stamp_exclusion_shape)
    return BrushShape::CIRCLE;
  return static_cast<BrushShape>(_map_stamp_exclusion_shape->checkedId());
}

void BrushStack::changeMapStampExclusionBrushRadius(float change)
{
  if (_map_stamp_exclusion_radius)
    _map_stamp_exclusion_radius->setValue(_map_stamp_exclusion_radius->value() + change);
}

Stamp::MapStampProtectionSettings BrushStack::mapStampProtectionSettings() const
{
  Stamp::MapStampProtectionSettings protection;
  protection.automatic = _map_stamp_auto_protection->isChecked();
  protection.slope_start_degrees = static_cast<float>(_map_stamp_protection_slope->value());
  protection.slope_full_degrees = std::min(89.f, protection.slope_start_degrees + 15.f);
  protection.relief_start = static_cast<float>(_map_stamp_protection_relief->value());
  protection.relief_full = protection.relief_start + 20.f;
  protection.manual_at = [this](float world_x, float world_z) -> std::optional<float>
  {
    float protection_value = 0.f;
    bool touched = false;
    for (MapStampProtectionStroke const& stroke : _map_stamp_protection_strokes)
    {
      float const dx = world_x - stroke.center.x;
      float const dz = world_z - stroke.center.z;
      float const distance = stroke.shape == BrushShape::SQUARE
          ? std::max(std::abs(dx), std::abs(dz)) : std::hypot(dx, dz);
      if (distance >= stroke.radius)
        continue;
      touched = true;
      float const t = std::clamp(
          (stroke.radius - distance) / (stroke.radius * .25f), 0.f, 1.f);
      float const strength = t * t * (3.f - 2.f * t);
      if (stroke.protect)
        protection_value = 1.f - (1.f - protection_value) * (1.f - strength);
      else
        protection_value *= 1.f - strength;
    }
    return touched ? std::optional<float>{protection_value} : std::nullopt;
  };
  return protection;
}

Stamp::MapStampHeightMode BrushStack::mapStampHeightMode() const
{
  if (!_map_stamp_height_mode || _map_stamp_height_mode->currentIndex() == 0)
    return Stamp::MapStampHeightMode::ExactFeature;
  return _map_stamp_height_mode->currentIndex() == 1
      ? Stamp::MapStampHeightMode::MountainBlend
      : Stamp::MapStampHeightMode::ConformToTerrain;
}

float BrushStack::mapStampRadius() const
{
  return _map_stamp_radius ? static_cast<float>(_map_stamp_radius->value())
                           : static_cast<float>(_ui.radiusSlider->value());
}

float BrushStack::mapStampHardness() const
{
  float const edge_blend = _map_stamp_edge_blend
      ? static_cast<float>(_map_stamp_edge_blend->value()) : .25f;
  return std::clamp(1.f - edge_blend, .5f, .95f);
}

bool BrushStack::executeMapStamp(glm::vec3 const& cursor_pos, World* world)
{
  if (!hasActiveMapStamp())
    return false;
  bool const was_locked = _map_stamp_position_lock && _map_stamp_position_lock->isChecked();
  clearMapStampTerrainPreview();
  Stamp::MapStampProtectionSettings protection = mapStampProtectionSettings();
  bool const changed = _map_stamp.apply(world, mapStampPosition(cursor_pos), mapStampRadius(),
                                        _ui.brushRotation->value(), mapStampHardness(),
                                        static_cast<float>(_map_stamp_height_scale->value()),
                                        static_cast<float>(_map_stamp_height_offset->value()),
                                        static_cast<float>(_map_stamp_opacity->value()), protection,
                                        mapStampHeightMode());
  markMapStampTerrainPreviewDirty();
  if (changed && was_locked)
    _map_stamp_position_lock->setChecked(false);
  return changed;
}

void BrushStack::markMapStampTerrainPreviewDirty(bool textures_dirty)
{
  _map_stamp_terrain_preview_dirty = true;
  _map_stamp_texture_preview_dirty |= textures_dirty;
}

void BrushStack::clearMapStampTerrainPreview()
{
  World* world = _map_view ? _map_view->getWorld() : nullptr;
  for (MapStampPreviewChunk const& index : _map_stamp_terrain_preview_chunks)
  {
    MapTile* tile = world ? world->mapIndex.getTile(index.tile_index) : nullptr;
    if (!tile || !tile->finishedLoading() || index.x >= 16 || index.z >= 16)
      continue;
    MapChunk* chunk = tile->getChunk(index.x, index.z);
    chunk->setChunkMoverPreviewHeights(std::nullopt);
    chunk->setChunkMoverPreviewNormals(std::nullopt);
    chunk->getTextureSet()->setChunkMoverTexturePreview(std::nullopt);
  }
  _map_stamp_terrain_preview_chunks.clear();
  _map_stamp_height_preview_lines.clear();
  _map_stamp_terrain_preview_valid = false;
  _map_stamp_texture_preview_dirty = true;
  if (_map_view)
  {
    _map_view->invalidate();
    _map_view->update();
  }
}

void BrushStack::updateMapStampTerrainPreview(glm::vec3 const& cursor_pos, World* world)
{
  if (!world || !isMapStampHeightDragEnabled() || !hasActiveMapStamp()
      || isMapStampPaintedSelectionEnabled()
      || isMapStampExclusionBrushEnabled() || !isMapStampPositionLocked())
  {
    if (_map_stamp_terrain_preview_valid || !_map_stamp_terrain_preview_chunks.empty())
      clearMapStampTerrainPreview();
    return;
  }

  glm::vec3 const center = mapStampPosition(cursor_pos);
  if (_map_stamp_terrain_preview_valid && !_map_stamp_terrain_preview_dirty
      && std::abs(center.x - _map_stamp_terrain_preview_center.x) < .01f
      && std::abs(center.z - _map_stamp_terrain_preview_center.z) < .01f)
  {
    return;
  }

  Stamp::MapStampProtectionSettings protection = mapStampProtectionSettings();
  std::vector<MapChunk*> preview_chunks;
  std::vector<std::vector<glm::vec3>> preview_lines;
  bool const preview_ready = _map_stamp.previewTerrain(
      world, center, mapStampRadius(), _ui.brushRotation->value(), mapStampHardness(),
      static_cast<float>(_map_stamp_height_scale->value()),
      static_cast<float>(_map_stamp_height_offset->value()),
      static_cast<float>(_map_stamp_opacity->value()), protection, mapStampHeightMode(),
      _map_stamp_texture_preview_dirty, preview_chunks, preview_lines);

  std::vector<MapStampPreviewChunk> desired_chunks;
  desired_chunks.reserve(preview_chunks.size());
  for (MapChunk* chunk : preview_chunks)
  {
    desired_chunks.push_back(
        {chunk->mt->index, static_cast<unsigned>(chunk->px),
         static_cast<unsigned>(chunk->py)});
  }

  auto desired_contains = [&](MapStampPreviewChunk const& previous)
  {
    return std::any_of(desired_chunks.begin(), desired_chunks.end(),
        [&](MapStampPreviewChunk const& desired)
        {
          return desired.tile_index == previous.tile_index
              && desired.x == previous.x && desired.z == previous.z;
        });
  };
  for (MapStampPreviewChunk const& previous : _map_stamp_terrain_preview_chunks)
  {
    if (preview_ready && desired_contains(previous))
      continue;
    MapTile* tile = world->mapIndex.getTile(previous.tile_index);
    if (!tile || !tile->finishedLoading() || previous.x >= 16 || previous.z >= 16)
      continue;
    MapChunk* chunk = tile->getChunk(previous.x, previous.z);
    chunk->setChunkMoverPreviewHeights(std::nullopt);
    chunk->setChunkMoverPreviewNormals(std::nullopt);
    chunk->getTextureSet()->setChunkMoverTexturePreview(std::nullopt);
  }

  _map_stamp_terrain_preview_chunks = preview_ready
      ? std::move(desired_chunks) : std::vector<MapStampPreviewChunk>{};
  _map_stamp_terrain_preview_center = center;
  _map_stamp_height_preview_lines = preview_ready
      ? std::move(preview_lines) : std::vector<std::vector<glm::vec3>>{};
  _map_stamp_terrain_preview_valid = preview_ready;
  _map_stamp_terrain_preview_dirty = !preview_ready;
  if (preview_ready)
    _map_stamp_texture_preview_dirty = false;
  _map_view->invalidate();
  _map_view->update();
}

bool BrushStack::isMapStampHeightDragEnabled() const
{
  return _map_stamp_height_drag && _map_stamp_height_drag->isChecked();
}

bool BrushStack::isMapStampHeightDragActive() const
{
  return _map_stamp_height_drag_active;
}

bool BrushStack::beginMapStampHeightDrag(glm::vec3 const& cursor_pos)
{
  if (!isMapStampHeightDragEnabled() || !hasActiveMapStamp()
      || isMapStampPaintedSelectionEnabled()
      || isMapStampExclusionBrushEnabled() || _map_stamp_height_drag_active)
  {
    return false;
  }

  _map_stamp_height_drag_active = true;
  if (!_map_stamp_position_lock->isChecked())
    lockMapStampPosition(cursor_pos);
  _map_stamp_status->setText(
      "Adjusting elevation. Drag vertically; release to paste, or Right-click to cancel. "
      "Hold Ctrl for fine adjustment.");
  return true;
}

void BrushStack::adjustMapStampHeightDrag(float vertical_pixels, bool fine_adjustment)
{
  if (!_map_stamp_height_drag_active || vertical_pixels == 0.f)
    return;

  float const units_per_pixel = fine_adjustment ? .05f : .5f;
  _map_stamp_height_offset->setValue(
      _map_stamp_height_offset->value() + vertical_pixels * units_per_pixel);
  _map_stamp_status->setText(QString("Elevation %1. Release Left to paste; Right-click cancels.")
      .arg(_map_stamp_height_offset->value(), 0, 'f', 2));
}

bool BrushStack::commitMapStampHeightDrag(World* world)
{
  if (!_map_stamp_height_drag_active)
    return false;

  _map_stamp_height_drag_active = false;
  bool const changed = executeMapStamp(_map_stamp_locked_position, world);
  if (_map_stamp_position_lock)
    _map_stamp_position_lock->setChecked(false);
  clearMapStampTerrainPreview();
  _map_stamp_status->setText(changed
      ? QString("Stamp pasted at elevation %1. Position unlocked; Undo restores the previous terrain.")
          .arg(_map_stamp_height_offset->value(), 0, 'f', 2)
      : "The stamp could not be pasted. Position unlocked; no terrain was changed.");
  return changed;
}

void BrushStack::endMapStampHeightDrag()
{
  if (!_map_stamp_height_drag_active)
    return;

  _map_stamp_height_drag_active = false;
  clearMapStampTerrainPreview();
  if (_map_stamp_position_lock)
    _map_stamp_position_lock->setChecked(false);
  _map_stamp_status->setText("Elevation drag cancelled. Position unlocked; no terrain was changed.");
}

std::vector<std::vector<glm::vec3>> const& BrushStack::mapStampHeightPreviewLines() const
{
  return _map_stamp_height_preview_lines;
}

void BrushStack::paintMapStampProtection(glm::vec3 const& cursor_pos)
{
  if (!isMapStampExclusionBrushEnabled())
    return;
  float const radius = mapStampExclusionBrushRadius();
  BrushShape const shape = mapStampExclusionBrushShape();
  bool const protect = !_map_stamp_exclusion_operation
      || _map_stamp_exclusion_operation->checkedId() == 0;
  if (!_map_stamp_protection_strokes.empty())
  {
    MapStampProtectionStroke const& last = _map_stamp_protection_strokes.back();
    float const dx = cursor_pos.x - last.center.x;
    float const dz = cursor_pos.z - last.center.z;
    float const spacing = shape == BrushShape::SQUARE
        ? std::max(std::abs(dx), std::abs(dz)) : std::hypot(dx, dz);
    if (last.protect == protect && last.shape == shape && spacing < radius * .2f)
      return;
  }
  _map_stamp_protection_strokes.push_back({cursor_pos, radius, protect, shape});
  updateMapStampPreview();
  markMapStampTerrainPreviewDirty();
  _map_stamp_status->setText(protect
      ? "Exclusion painted pink. Select Erase to remove exclusions."
      : "Exclusion erased. Select Paint to add exclusions.");
}

bool BrushStack::isMapStampPaintedSelectionEnabled() const
{
  return _map_stamp_enabled && _map_stamp_enabled->isChecked()
      && _map_stamp_shape && _map_stamp_shape->currentIndex() == 2
      && _map_stamp_painted_selection && _map_stamp_painted_selection->isChecked();
}

float BrushStack::mapStampPaintedSelectionRadius() const
{
  return _map_stamp_painted_radius
      ? static_cast<float>(_map_stamp_painted_radius->value()) : TEXDETAILSIZE;
}

void BrushStack::paintMapStampSelection(glm::vec3 const& cursor_pos, bool erase)
{
  if (!isMapStampPaintedSelectionEnabled())
    return;
  constexpr float grid_step = TEXDETAILSIZE;
  if (_map_stamp_painted_last_position
      && glm::distance(glm::vec2{cursor_pos.x, cursor_pos.z},
           glm::vec2{_map_stamp_painted_last_position->x,
                     _map_stamp_painted_last_position->z}) < grid_step * .2f)
    return;

  float const radius = std::max(grid_step, mapStampPaintedSelectionRadius());
  glm::vec3 const start = _map_stamp_painted_last_position.value_or(cursor_pos);
  float const length = glm::distance(glm::vec2{start.x, start.z},
                                     glm::vec2{cursor_pos.x, cursor_pos.z});
  int const dab_count = std::max(1, static_cast<int>(std::ceil(
      length / std::max(grid_step, radius * .22f))));
  std::vector<glm::ivec2> changed_cells;
  int const cell_diameter = static_cast<int>(std::ceil(radius * 2.f / grid_step)) + 1;
  changed_cells.reserve(std::min<std::size_t>(
      static_cast<std::size_t>(cell_diameter) * cell_diameter, 65536));
  float const radius_squared = radius * radius;
  for (int dab = 0; dab <= dab_count; ++dab)
  {
    float const t = static_cast<float>(dab) / dab_count;
    glm::vec3 const point = start * (1.f - t) + cursor_pos * t;
    int const minimum_x = static_cast<int>(std::floor((point.x - radius) / grid_step));
    int const maximum_x = static_cast<int>(std::floor((point.x + radius) / grid_step));
    int const minimum_z = static_cast<int>(std::floor((point.z - radius) / grid_step));
    int const maximum_z = static_cast<int>(std::floor((point.z + radius) / grid_step));
    for (int grid_z = minimum_z; grid_z <= maximum_z; ++grid_z)
      for (int grid_x = minimum_x; grid_x <= maximum_x; ++grid_x)
      {
        glm::vec2 const cell_center{(grid_x + .5f) * grid_step,
                                    (grid_z + .5f) * grid_step};
        glm::vec2 const offset = cell_center - glm::vec2{point.x, point.z};
        if (glm::dot(offset, offset) > radius_squared)
          continue;
        std::pair<int, int> const key{grid_z, grid_x};
        if (erase)
        {
          if (_map_stamp_painted_cells.erase(key))
            changed_cells.emplace_back(grid_x, grid_z);
        }
        else
        {
          auto const insertion = _map_stamp_painted_cells.emplace(key, 1.f);
          if (insertion.second)
            changed_cells.emplace_back(grid_x, grid_z);
        }
      }
  }
  _map_stamp_painted_last_position = cursor_pos;
  if (!changed_cells.empty())
    _map_view->getWorld()->renderer()->updatePaintedStampSelectionOverlay(
        changed_cells, !erase);
  _map_stamp_status->setText(QString(
      "Painted source: %1 cells. Left-drag paints; Ctrl+drag erases; capture when ready.")
      .arg(_map_stamp_painted_cells.size()));
  _map_view->invalidate();
  _map_view->update();
}

void BrushStack::endMapStampSelectionStroke()
{
  _map_stamp_painted_last_position.reset();
}

void BrushStack::deactivateMapStampPaintedSelection()
{
  endMapStampSelectionStroke();
  if (_map_stamp_painted_selection && _map_stamp_painted_selection->isChecked())
    _map_stamp_painted_selection->setChecked(false);
}

void BrushStack::lockMapStampPosition(glm::vec3 const& cursor_pos)
{
  _map_stamp_locked_position = cursor_pos;
  _map_stamp_position_lock->setChecked(true);
}

void BrushStack::toggleMapStampPositionLock(glm::vec3 const& cursor_pos)
{
  if (_map_stamp_position_lock->isChecked())
    _map_stamp_position_lock->setChecked(false);
  else
    lockMapStampPosition(cursor_pos);
}

bool BrushStack::isMapStampPositionLocked() const
{
  return _map_stamp_position_lock && _map_stamp_position_lock->isChecked();
}

glm::vec3 BrushStack::mapStampPosition(glm::vec3 const& cursor_pos) const
{
  return isMapStampPositionLocked() ? _map_stamp_locked_position : cursor_pos;
}

void BrushStack::updateMapStampPreview()
{
  if (!hasLoadedMapStamp())
    return;

  constexpr int preview_resolution = 1024;
  int const rotation = _ui.brushRotation->value();
  float const extent_scale = _map_stamp.footprintBoundingRadius(
      1.f, rotation, mapStampHardness(), mapStampHeightMode());
  int const source_size = std::clamp(
      static_cast<int>(std::lround(preview_resolution / extent_scale)), 1, preview_resolution);
  QImage const source_preview = _map_stamp.previewImage().scaled(
      source_size, source_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  QImage stamp_preview(preview_resolution, preview_resolution, QImage::Format_ARGB32);
  stamp_preview.fill(qRgba(0, 0, 0, 255));
  {
    QPainter painter(&stamp_preview);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(preview_resolution * .5, preview_resolution * .5);
    painter.rotate(rotation);
    painter.drawImage(QPointF(-source_size * .5, -source_size * .5), source_preview);
  }

  if (!_map_stamp_protection_strokes.empty())
  {
    MapStampProtectionStroke const& first = _map_stamp_protection_strokes.front();
    float min_x = first.center.x - first.radius;
    float max_x = first.center.x + first.radius;
    float min_z = first.center.z - first.radius;
    float max_z = first.center.z + first.radius;
    for (MapStampProtectionStroke const& stroke : _map_stamp_protection_strokes)
    {
      min_x = std::min(min_x, stroke.center.x - stroke.radius);
      max_x = std::max(max_x, stroke.center.x + stroke.radius);
      min_z = std::min(min_z, stroke.center.z - stroke.radius);
      max_z = std::max(max_z, stroke.center.z + stroke.radius);
    }
    _map_stamp_protection_overlay_center = {
        (min_x + max_x) * .5f, 0.f, (min_z + max_z) * .5f};
    _map_stamp_protection_overlay_radius = std::max(
        std::max(max_x - min_x, max_z - min_z) * .5f, 1.f);
  }
  else
  {
    _map_stamp_protection_overlay_center = {};
    _map_stamp_protection_overlay_radius = 1.f;
  }

  if (!stamp_preview.isNull())
  {
    QImage mask(stamp_preview.size(), QImage::Format_ARGB32);
    mask.fill(Qt::transparent);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    float const width = static_cast<float>(mask.width());
    float const height = static_cast<float>(mask.height());
    for (MapStampProtectionStroke const& stroke : _map_stamp_protection_strokes)
    {
      QPointF const center(
          (.5f + (stroke.center.x - _map_stamp_protection_overlay_center.x)
              / (2.f * _map_stamp_protection_overlay_radius)) * width,
          (.5f + (stroke.center.z - _map_stamp_protection_overlay_center.z)
              / (2.f * _map_stamp_protection_overlay_radius)) * height);
      float const pixel_radius = stroke.radius
          / (2.f * _map_stamp_protection_overlay_radius) * std::min(width, height);
      int const pixel_diameter = std::max(1, static_cast<int>(std::ceil(pixel_radius * 2.f)));
      QImage stroke_mask(pixel_diameter, pixel_diameter, QImage::Format_ARGB32);
      stroke_mask.fill(Qt::transparent);
      float const image_center = pixel_diameter * .5f;
      for (int y = 0; y < pixel_diameter; ++y)
      {
        auto* line = reinterpret_cast<QRgb*>(stroke_mask.scanLine(y));
        for (int x = 0; x < pixel_diameter; ++x)
        {
          float const dx = (x + .5f - image_center) / std::max(pixel_radius, 1.f);
          float const dy = (y + .5f - image_center) / std::max(pixel_radius, 1.f);
          float const distance = stroke.shape == BrushShape::SQUARE
              ? std::max(std::abs(dx), std::abs(dy)) : std::hypot(dx, dy);
          float const t = std::clamp((1.f - distance) / .25f, 0.f, 1.f);
          float const strength = t * t * (3.f - 2.f * t);
          line[x] = qRgba(255, 255, 255, static_cast<int>(strength * 255.f));
        }
      }
      painter.setCompositionMode(stroke.protect
          ? QPainter::CompositionMode_SourceOver
          : QPainter::CompositionMode_DestinationOut);
      painter.drawImage(QPointF(center.x() - image_center, center.y() - image_center),
                        stroke_mask);
    }
    painter.end();

    _map_stamp_preview = stamp_preview.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < _map_stamp_preview.height(); ++y)
    {
      auto* preview_line = reinterpret_cast<QRgb*>(_map_stamp_preview.scanLine(y));
      auto const* mask_line = reinterpret_cast<QRgb const*>(mask.constScanLine(y));
      for (int x = 0; x < _map_stamp_preview.width(); ++x)
      {
        int const preview = qRed(preview_line[x]);
        int const protection = qAlpha(mask_line[x]);
        // setBrushTexture uploads QRgb's little-endian bytes as RGBA. Blue therefore carries
        // the regular preview in shader .r, while green carries the manual-protection overlay.
        preview_line[x] = qRgba(0, protection, preview, 255);
      }
    }
  }

  _map_view->setBrushTexture(&_map_stamp_preview);
}

void BrushStack::addAction(BrushStackItem* brush_stack_item)
{
  _active_item_button_group->addButton(brush_stack_item->getActiveButton());
  brush_stack_item->syncSliders(_ui.radiusSlider->value(), _ui.innerRadiusSlider->value(), _ui.speedSlider->value(), _ui.brushRotation->value(), _ui.sculptRadio->isChecked());

  _active_item = brush_stack_item;
  brush_stack_item->getActiveButton()->setChecked(true);

  connect(brush_stack_item, &BrushStackItem::settingsChanged,
          [this](BrushStackItem* item)
          {
            item->syncSliders(_ui.radiusSlider->value(), _ui.innerRadiusSlider->value(), _ui.speedSlider->value(), _ui.brushRotation->value(), _ui.sculptRadio->isChecked());
          });

  connect(brush_stack_item, &BrushStackItem::activated,
          [this](BrushStackItem* item)
          {
            _active_item = item;

            if(item)
              item->updateMask();
          });

  connect(brush_stack_item, &BrushStackItem::requestDelete,
          [this](BrushStackItem* item)
          {
            if (_active_item == item)
              _active_item = nullptr;

            for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
            {
              auto l_item = _ui.brushList->layout()->itemAt(i);

              if (l_item->widget() != item)
                continue;

              _active_item_button_group->removeButton(static_cast<BrushStackItem*>(l_item->widget())->getActiveButton());
              _ui.brushList->layout()->removeItem(l_item);

              l_item->widget()->deleteLater();
              delete l_item;
            }

            if(item)
              item->updateMask();

          });

}

void BrushStack::execute(glm::vec3 const& cursor_pos, World* world, float dt, bool mod_shift_down, bool mod_alt_down, bool mod_ctrl_down, bool is_under_map)
{
  for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
  {
    auto item = reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget());

    if (!item->isAffecting())
      continue;

    item->execute(cursor_pos, world, dt, mod_shift_down, mod_alt_down, mod_ctrl_down, is_under_map);
  }
}

void BrushStack::changeRadius(float change)
{
  _ui.radiusSlider->setValue(_ui.radiusSlider->value() + change);
}

void BrushStack::changeInnerRadius(float change)
{
  _ui.innerRadiusSlider->setValue(_ui.innerRadiusSlider->value() + change);
}

void BrushStack::changeSpeed(float change)
{
  _ui.speedSlider->setValue(_ui.speedSlider->value() + change);
}

float BrushStack::getRadius()
{
  return _ui.radiusSlider->value();
}

float BrushStack::getInnerRadius()
{
  return _ui.innerRadiusSlider->value();
}

float BrushStack::getSpeed()
{
  return _ui.speedSlider->value();
}

bool Noggit::Ui::Tools::BrushStack::getBrushMode() const
{
  return _ui.sculptRadio->isChecked();
}

bool Noggit::Ui::Tools::BrushStack::getRandomizeRotation() const
{
  return hasActiveMapStamp() && _map_stamp_randomize_rotation
      ? _map_stamp_randomize_rotation->isChecked()
      : _ui.randomizeRotation->isChecked();
}

BrushStackItem* Noggit::Ui::Tools::BrushStack::getActiveBrushItem()
{
  return _active_item;
}

void BrushStack::changeRotation(int change)
{
  setRotation(_ui.brushRotation->value() + change);
}

void BrushStack::setRotation(int rotation)
{
  int orientation = rotation % 360;
  if (orientation < 0)
    orientation += 360;

  if (_map_stamp_rotation && _map_stamp_rotation->value() != orientation)
  {
    QSignalBlocker const blocker(_map_stamp_rotation);
    _map_stamp_rotation->setValue(orientation);
  }
  if (_ui.brushRotation->value() != orientation)
    _ui.brushRotation->setValue(orientation);
}

QJsonObject BrushStack::toJSON()
{
  QJsonObject json;
  QJsonArray array;

  for (int i = 0; i < _ui.brushList->layout()->count(); ++i)
  {
    array.append(reinterpret_cast<BrushStackItem*>(_ui.brushList->layout()->itemAt(i)->widget())->toJSON());
  }

  json["actions"] = array;

  return json;
}

void BrushStack::fromJSON(const QJsonObject& json)
{
  if (!json.contains("actions"))
  {
    LogError << "Attempted loaded malformed brush." << std::endl;
    return;
  }

  QJsonArray array = json["actions"].toArray();

  for (int i = 0; i < array.count(); ++i)
  {
    QJsonObject obj = array[i].toObject();

    if (!obj.contains("brush_action_type"))
    {
      LogError << "Attempted loaded malformed brush." << std::endl;
      continue;
    }

    QString type = obj["brush_action_type"].toString();

    auto brush_stack_item = new BrushStackItem(this);
    _ui.brushList->layout()->addWidget(brush_stack_item);

    if (type == "TERRAIN")
    {
      brush_stack_item->setTool(new Noggit::Ui::TerrainTool(_map_view, this, true));
    }
    else if (type == "FLATTEN_BLUR")
    {
      brush_stack_item->setTool(new Noggit::Ui::flatten_blur_tool(this));
    }
    else if (type == "TEXTURING")
    {
      brush_stack_item->setTool(new Noggit::Ui::texturing_tool(&_map_view->getCamera()->position, _map_view, nullptr, this));
    }
    else if (type == "SHADER")
    {
      brush_stack_item->setTool(new Noggit::Ui::ShaderTool(_map_view, this));
    }
    else
    {
      brush_stack_item->deleteLater();
      LogError << "Attempted loading malformed brush." << std::endl;
      continue;
    }

    brush_stack_item->fromJSON(obj);
    addAction(brush_stack_item);

  }

}
