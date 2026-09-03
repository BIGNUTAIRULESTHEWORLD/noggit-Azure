// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "FenceTool.hpp"

#include <noggit/ActionManager.hpp>
#include <noggit/MapView.h>
#include <noggit/ModelInstance.h>
#include <noggit/SceneObject.hpp>
#include <noggit/Selection.h>
#include <noggit/World.h>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>

#include <glm/geometric.hpp>

namespace
{
  constexpr float pi = 3.14159265358979323846f;
  constexpr std::size_t maximum_fence_placements = 5000;

  float horizontalDistance(glm::vec3 const& first, glm::vec3 const& second)
  {
    return glm::length(glm::vec2{second.x - first.x, second.z - first.z});
  }

  glm::vec3 terrainPosition(World* world, glm::vec3 position)
  {
    if (auto const ground = world->try_get_ground_height(position))
      position.y = ground->y;
    return position;
  }

  std::string lowercase(std::string value)
  {
    std::transform(value.begin(), value.end(), value.begin(),
      [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
  }

  std::string assetBasename(std::string const& path)
  {
    std::size_t const separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
  }

  std::string assetDirectory(std::string const& path)
  {
    std::size_t const separator = path.find_last_of("/\\");
    return separator == std::string::npos ? std::string{} : path.substr(0, separator);
  }

  bool containsAny(std::string const& value,
                   std::initializer_list<char const*> const tokens)
  {
    return std::any_of(tokens.begin(), tokens.end(),
      [&value](char const* token) { return value.find(token) != std::string::npos; });
  }
}

namespace Noggit
{
  FenceTool::FenceTool(MapView* map_view)
    : Tool(map_view)
  {
  }

  FenceTool::~FenceTool()
  {
    delete _panel;
  }

  char const* FenceTool::name() const
  {
    return "Fence Builder";
  }

  editing_mode FenceTool::editingMode() const
  {
    return editing_mode::fence;
  }

  Ui::FontNoggit::Icons FenceTool::icon() const
  {
    return Ui::FontNoggit::DOODAD;
  }

  void FenceTool::setupUi(Ui::Tools::ToolPanel* tool_panel)
  {
    _panel = new QWidget(mapView());
    _panel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(_panel);
    layout->setAlignment(Qt::AlignTop);
    layout->setContentsMargins(6, 6, 6, 6);

    auto const fill_width = [](QWidget* widget)
    {
      widget->setMinimumWidth(0);
      widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    };
    auto const wrap_label = [](QLabel* label)
    {
      label->setMinimumWidth(0);
      label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      label->setWordWrap(true);
    };

    auto* instructions = new QLabel(
      "1. Select Reference, then hold left mouse and paint blue across the existing fence corridor. Ctrl+paint erases.\n"
      "2. Choose OK - Use Selection; fence sections and posts are detected automatically.\n"
      "3. Hold left mouse and drag along the new road centerline, then commit.", _panel);
    wrap_label(instructions);
    layout->addWidget(instructions);

    auto* pattern_group = new QGroupBox("Fence pattern", _panel);
    auto* pattern_layout = new QVBoxLayout(pattern_group);
    _pattern_label = new QLabel("Section: not captured\nPost: not captured", pattern_group);
    wrap_label(_pattern_label);
    auto* manual_label = new QLabel("Optional manual asset overrides", pattern_group);
    wrap_label(manual_label);
    auto* pattern_buttons = new QVBoxLayout();
    auto* pick_section = new QPushButton("Override Section M2", pattern_group);
    pick_section->setToolTip("The next viewport click captures an existing M2 as the repeating fence section.");
    auto* pick_post = new QPushButton("Override Post M2", pattern_group);
    pick_post->setToolTip("The next viewport click captures an existing M2 as the post placed at every section boundary.");
    fill_width(pick_section);
    fill_width(pick_post);
    pattern_buttons->addWidget(pick_section);
    pattern_buttons->addWidget(pick_post);
    pattern_layout->addWidget(_pattern_label);
    pattern_layout->addWidget(manual_label);
    pattern_layout->addLayout(pattern_buttons);

    auto* capture_form = new QFormLayout();
    capture_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    capture_form->setLabelAlignment(Qt::AlignLeft);
    capture_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    _capture_width_spin = new QDoubleSpinBox(pattern_group);
    _capture_width_spin->setRange(0.5, 64.0);
    _capture_width_spin->setDecimals(2);
    _capture_width_spin->setSingleStep(1.0);
    _capture_width_spin->setValue(8.0);
    _capture_width_spin->setSuffix(" units");
    _capture_width_spin->setToolTip("Radius of the blue reference-selection brush. Increase it to reach fences on both sides of a road.");
    fill_width(_capture_width_spin);
    capture_form->addRow("Reference brush radius", _capture_width_spin);
    pattern_layout->addLayout(capture_form);

    auto* area_buttons = new QVBoxLayout();
    auto* capture_area = new QPushButton("Select Reference", pattern_group);
    capture_area->setToolTip("Start painting a blue mask over an existing fence corridor. Ctrl+paint removes selection.");
    auto* clear_area = new QPushButton("Clear Reference", pattern_group);
    clear_area->setToolTip("Return to continuous fence generation using the spacing control.");
    auto* lock_pattern = new QPushButton("OK - Use Selection", pattern_group);
    lock_pattern->setToolTip("Analyze the painted reference, automatically detect the fence section and post assets, and switch to path painting.");
    fill_width(capture_area);
    fill_width(lock_pattern);
    fill_width(clear_area);
    area_buttons->addWidget(capture_area);
    area_buttons->addWidget(lock_pattern);
    area_buttons->addWidget(clear_area);
    pattern_layout->addLayout(area_buttons);
    layout->addWidget(pattern_group);

    auto* placement_group = new QGroupBox("Placement", _panel);
    auto* placement_form = new QFormLayout(placement_group);
    placement_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    placement_form->setLabelAlignment(Qt::AlignLeft);
    placement_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    _side_combo = new QComboBox(placement_group);
    _side_combo->addItems({"Left side", "Right side", "Both sides"});
    _side_combo->setCurrentIndex(2);
    fill_width(_side_combo);
    placement_form->addRow("Fence side", _side_combo);

    _offset_spin = new QDoubleSpinBox(placement_group);
    _offset_spin->setRange(0.0, 500.0);
    _offset_spin->setDecimals(2);
    _offset_spin->setSingleStep(0.5);
    _offset_spin->setValue(8.0);
    _offset_spin->setSuffix(" units");
    _offset_spin->setToolTip("Distance from the drawn centerline to each fence run.");
    fill_width(_offset_spin);
    placement_form->addRow("Road-edge offset", _offset_spin);

    _corridor_nudge_spin = new QDoubleSpinBox(placement_group);
    _corridor_nudge_spin->setRange(-100.0, 100.0);
    _corridor_nudge_spin->setDecimals(2);
    _corridor_nudge_spin->setSingleStep(0.25);
    _corridor_nudge_spin->setSuffix(" units");
    _corridor_nudge_spin->setToolTip("Shifts an entire captured fence corridor left or right relative to the painted destination path.");
    fill_width(_corridor_nudge_spin);
    placement_form->addRow("Corridor nudge", _corridor_nudge_spin);

    _width_scale_spin = new QDoubleSpinBox(placement_group);
    _width_scale_spin->setRange(0.1, 4.0);
    _width_scale_spin->setDecimals(3);
    _width_scale_spin->setSingleStep(0.05);
    _width_scale_spin->setValue(1.0);
    _width_scale_spin->setToolTip("Scales captured lateral distances around the centerline for roads wider or narrower than the reference.");
    fill_width(_width_scale_spin);
    placement_form->addRow("Corridor width scale", _width_scale_spin);

    _alignment_combo = new QComboBox(placement_group);
    _alignment_combo->addItem("Follow road (rigid groups)");
    _alignment_combo->setToolTip("Keeps connected fence sections and posts together as a rigid group while each separate group follows the road.");
    fill_width(_alignment_combo);
    placement_form->addRow("Fence alignment", _alignment_combo);

    _reverse_pattern_check = new QCheckBox("Reverse captured pattern direction", placement_group);
    _reverse_pattern_check->setToolTip("Rotates the captured corridor frame 180 degrees when its direction is opposite the destination path.");
    placement_form->addRow(_reverse_pattern_check);

    _spacing_spin = new QDoubleSpinBox(placement_group);
    _spacing_spin->setRange(0.1, 200.0);
    _spacing_spin->setDecimals(2);
    _spacing_spin->setSingleStep(0.1);
    _spacing_spin->setValue(4.0);
    _spacing_spin->setSuffix(" units");
    _spacing_spin->setToolTip("Maximum center-to-center spacing in continuous mode. Captured area patterns preserve their own spacing.");
    fill_width(_spacing_spin);
    placement_form->addRow("Continuous max spacing", _spacing_spin);

    _axis_combo = new QComboBox(placement_group);
    _axis_combo->addItems({"Auto from model", "Local X", "Local Z"});
    _axis_combo->setToolTip("Selects which local model axis runs along the fence path.");
    fill_width(_axis_combo);
    placement_form->addRow("Fence length axis", _axis_combo);

    _yaw_offset_spin = new QDoubleSpinBox(placement_group);
    _yaw_offset_spin->setRange(-180.0, 180.0);
    _yaw_offset_spin->setDecimals(1);
    _yaw_offset_spin->setSingleStep(1.0);
    _yaw_offset_spin->setSuffix(" deg");
    _yaw_offset_spin->setToolTip("Corrects models whose visual forward direction differs from their bounding-box axis.");
    fill_width(_yaw_offset_spin);
    placement_form->addRow("Yaw correction", _yaw_offset_spin);

    _height_offset_spin = new QDoubleSpinBox(placement_group);
    _height_offset_spin->setRange(-100.0, 100.0);
    _height_offset_spin->setDecimals(2);
    _height_offset_spin->setSingleStep(0.1);
    _height_offset_spin->setSuffix(" units");
    _height_offset_spin->setToolTip("Vertical distance between the model origin and terrain. Captured from the source section.");
    fill_width(_height_offset_spin);
    placement_form->addRow("Section height offset", _height_offset_spin);

    _post_height_offset_spin = new QDoubleSpinBox(placement_group);
    _post_height_offset_spin->setRange(-100.0, 100.0);
    _post_height_offset_spin->setDecimals(2);
    _post_height_offset_spin->setSingleStep(0.1);
    _post_height_offset_spin->setSuffix(" units");
    _post_height_offset_spin->setToolTip("Vertical distance between the post model origin and terrain. Captured from the source post.");
    fill_width(_post_height_offset_spin);
    placement_form->addRow("Post height offset", _post_height_offset_spin);

    _align_slope_check = new QCheckBox("Pitch sections along terrain slope", placement_group);
    _align_slope_check->setChecked(true);
    placement_form->addRow(_align_slope_check);
    layout->addWidget(placement_group);

    auto* path_group = new QGroupBox("Path", _panel);
    auto* path_layout = new QVBoxLayout(path_group);
    _path_label = new QLabel("No path points", path_group);
    wrap_label(_path_label);
    auto* path_buttons = new QVBoxLayout();
    auto* undo_point = new QPushButton("Undo Point", path_group);
    auto* clear_path = new QPushButton("Clear Path", path_group);
    fill_width(undo_point);
    fill_width(clear_path);
    path_buttons->addWidget(undo_point);
    path_buttons->addWidget(clear_path);
    path_layout->addWidget(_path_label);
    path_layout->addLayout(path_buttons);
    layout->addWidget(path_group);

    _status_label = new QLabel("Choose Select Reference and paint over an existing fence corridor.", _panel);
    wrap_label(_status_label);
    _status_label->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    layout->addWidget(_status_label);

    auto* commit = new QPushButton("Commit Fence", _panel);
    commit->setToolTip("Places all previewed M2 sections as one undoable action.");
    fill_width(commit);
    layout->addWidget(commit);

    tool_panel->registerTool(this, _panel);

    QObject::connect(pick_section, &QPushButton::clicked, _panel, [this]()
    {
      _pick_role = PickRole::section;
      updatePanelStatus("Click an existing M2 fence section in the viewport.");
      mapView()->invalidate();
    });
    QObject::connect(pick_post, &QPushButton::clicked, _panel, [this]()
    {
      _pick_role = PickRole::post;
      updatePanelStatus("Click the M2 fence post in the viewport.");
      mapView()->invalidate();
    });
    QObject::connect(capture_area, &QPushButton::clicked, _panel,
                     [this]() { beginPatternAreaCapture(); });
    QObject::connect(lock_pattern, &QPushButton::clicked, _panel,
                     [this]() { capturePatternArea(); });
    QObject::connect(clear_area, &QPushButton::clicked, _panel,
                     [this]() { clearCapturedPattern(); });
    QObject::connect(undo_point, &QPushButton::clicked, _panel,
                     [this]() { removeLastControlPoint(); });
    QObject::connect(clear_path, &QPushButton::clicked, _panel,
                     [this]() { clearPath(); });
    QObject::connect(commit, &QPushButton::clicked, _panel,
                     [this]() { commitFence(); });

    auto const rebuild = [this]() { rebuildPlacementPreview(); };
    QObject::connect(_side_combo, qOverload<int>(&QComboBox::currentIndexChanged), _panel,
                     [rebuild](int) { rebuild(); });
    QObject::connect(_axis_combo, qOverload<int>(&QComboBox::currentIndexChanged), _panel,
                     [rebuild](int) { rebuild(); });
    QObject::connect(_offset_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), _panel,
                     [rebuild](double) { rebuild(); });
    QObject::connect(_corridor_nudge_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), _panel,
                     [rebuild](double) { rebuild(); });
    QObject::connect(_width_scale_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), _panel,
                     [rebuild](double) { rebuild(); });
    QObject::connect(_alignment_combo, qOverload<int>(&QComboBox::currentIndexChanged), _panel,
                     [rebuild](int) { rebuild(); });
    QObject::connect(_reverse_pattern_check, &QCheckBox::toggled, _panel,
                     [rebuild](bool) { rebuild(); });
    QObject::connect(_spacing_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), _panel,
                     [rebuild](double) { rebuild(); });
    QObject::connect(_yaw_offset_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), _panel,
                     [rebuild](double) { rebuild(); });
    QObject::connect(_height_offset_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), _panel,
                     [rebuild](double) { rebuild(); });
    QObject::connect(_post_height_offset_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), _panel,
                     [rebuild](double) { rebuild(); });
    QObject::connect(_capture_width_spin, qOverload<double>(&QDoubleSpinBox::valueChanged), _panel,
                     [this](double)
                     {
                       mapView()->invalidate();
                     });
    QObject::connect(_align_slope_check, &QCheckBox::toggled, _panel,
                     [rebuild](bool) { rebuild(); });
  }

  ToolDrawParameters FenceTool::drawParameters() const
  {
    ToolDrawParameters parameters;
    parameters.radius = _capturing_pattern_area && _capture_width_spin
      ? static_cast<float>(_capture_width_spin->value()) : 1.0f;
    parameters.cursor_type = CursorType::CIRCLE;
    parameters.cursor_color = _capturing_pattern_area
      ? glm::vec4{0.1f, 0.55f, 1.0f, 1.0f}
      : (_pick_role != PickRole::none
          ? glm::vec4{1.0f, 0.75f, 0.05f, 1.0f}
          : glm::vec4{0.05f, 0.85f, 1.0f, 1.0f});
    return parameters;
  }

  void FenceTool::onSelected()
  {
    // MapView clears selection after switching tools, so capture the object
    // selected in Object Editor while it is still available.
    captureSelectedPattern(true);
    rebuildPreview();
  }

  void FenceTool::onDeselected()
  {
    _pick_role = PickRole::none;
    _capturing_pattern_area = false;
    _painting_path = false;
    _reference_stroke_active = false;
    _reference_erase_stroke = false;
    _reference_last_brush_position.reset();
    _live_guide.clear();
    mapView()->invalidate();
  }

  void FenceTool::onMousePress(MousePressParameters const& params)
  {
    if (params.button != Qt::LeftButton || params.mod_space_down || NOGGIT_CUR_ACTION)
      return;

    if (_capturing_pattern_area)
    {
      _reference_stroke_active = true;
      _reference_erase_stroke = params.mod_ctrl_down;
      _reference_last_brush_position.reset();
      if (auto const point = terrainPointAt(params.mouse_position))
        paintPatternReference(*point, _reference_erase_stroke);
      else
        updatePanelStatus("Reference selection requires terrain beneath the cursor.");
      return;
    }

    if (_pick_role != PickRole::none)
    {
      pickSourceAt(params.mouse_position);
      return;
    }

    _painting_path = true;
    addControlPoint(terrainPointAt(params.mouse_position).value_or(mapView()->cursorPosition()));
  }

  void FenceTool::onMouseRelease(MouseReleaseParameters const& params)
  {
    if (params.button == Qt::LeftButton)
    {
      _painting_path = false;
      _reference_stroke_active = false;
      _reference_erase_stroke = false;
      _reference_last_brush_position.reset();
    }
  }

  void FenceTool::onMouseMove(MouseMoveParameters const& params)
  {
    if (_capturing_pattern_area && _reference_stroke_active
        && params.left_mouse && !params.right_mouse && !params.mod_space_down)
    {
      if (auto const point = terrainPointAt(params.mouse_position))
        paintPatternReference(*point, _reference_erase_stroke);
      return;
    }

    if (_painting_path && params.left_mouse && !params.right_mouse && !params.mod_space_down)
    {
      if (auto const point = terrainPointAt(params.mouse_position))
      {
        if (_control_points.empty() || horizontalDistance(_control_points.back(), *point) >= 2.0f)
          addControlPoint(*point);
      }
      return;
    }

    if (_control_points.empty() || params.right_mouse || params.mod_space_down)
    {
      _live_guide.clear();
      return;
    }

    glm::vec3 live = terrainPosition(mapView()->getWorld(), mapView()->cursorPosition());
    live.y += 0.35f;
    glm::vec3 last = _control_points.back();
    last.y += 0.35f;
    _live_guide = {last, live};
    mapView()->invalidate();
  }

  void FenceTool::postRender()
  {
    glm::mat4x4 const mvp = mapView()->projection() * mapView()->model_view();

    auto lifted = [](std::vector<glm::vec3> points)
    {
      for (glm::vec3& point : points)
        point.y += 0.22f;
      return points;
    };

    if (_path_preview.size() >= 2)
      _line_renderer.draw(mvp, lifted(_path_preview), glm::vec4{0.1f, 0.8f, 1.0f, 0.95f}, false);
    if (_left_preview.size() >= 2)
      _line_renderer.draw(mvp, lifted(_left_preview), glm::vec4{0.2f, 1.0f, 0.35f, 0.9f}, false);
    if (_right_preview.size() >= 2)
      _line_renderer.draw(mvp, lifted(_right_preview), glm::vec4{0.2f, 1.0f, 0.35f, 0.9f}, false);
    if (_placement_segments.size() >= 2)
      _line_renderer.drawSegments(mvp, _placement_segments, glm::vec4{1.0f, 0.85f, 0.05f, 1.0f});
    if (_post_segments.size() >= 2)
      _line_renderer.drawSegments(mvp, _post_segments, glm::vec4{1.0f, 0.35f, 0.05f, 1.0f});
    for (auto const& line : _reference_mask_lines)
    {
      if (line.size() >= 2)
        _line_renderer.draw(mvp, line, glm::vec4{0.1f, 0.55f, 1.0f, 0.8f}, false);
    }
    if (_live_guide.size() >= 2)
      _line_renderer.draw(mvp, _live_guide, glm::vec4{0.25f, 0.65f, 1.0f, 0.65f}, false);
  }

  void FenceTool::unload()
  {
    _line_renderer.unload();
    Tool::unload();
  }

  bool FenceTool::captureSelectedPattern(bool quiet_if_missing)
  {
    std::vector<std::pair<float, SceneObject*>> selected_models;
    for (auto const& selection : mapView()->getWorld()->current_selection())
    {
      if (selection.index() != eEntry_Object)
        continue;

      SceneObject* object = std::get<selected_object_type>(selection);
      if (!object || object->which() != eMODEL || object->chunk_mover_preview)
        continue;

      auto* model = static_cast<ModelInstance*>(object);
      model->model->wait_until_loaded();
      if (model->model->loading_failed())
        continue;

      glm::vec3 const local_size = glm::abs(
        model->model->bounding_box_max - model->model->bounding_box_min);
      float const horizontal_length = std::max(local_size.x, local_size.z) * object->scale;
      selected_models.emplace_back(horizontal_length, object);
    }

    if (selected_models.empty())
    {
      if (!quiet_if_missing)
        updatePanelStatus("Select the fence section and post M2s, or use the two picker buttons.");
      return false;
    }

    std::sort(selected_models.begin(), selected_models.end(),
      [](auto const& first, auto const& second) { return first.first > second.first; });

    // With a two-object selection the longer horizontal model is the section,
    // and the smaller one is the post. A single selection remains a convenient
    // shortcut for capturing just the section.
    bool captured = captureSource(selected_models.front().second, PickRole::section);
    if (selected_models.size() >= 2)
      captured = captureSource(selected_models.back().second, PickRole::post) || captured;
    return captured;
  }

  bool FenceTool::captureSource(SceneObject* object, PickRole role)
  {
    if (role == PickRole::none || !object || object->which() != eMODEL || object->chunk_mover_preview)
    {
      updatePanelStatus("Fence Builder currently accepts stored M2 placements only.");
      return false;
    }

    auto* model = static_cast<ModelInstance*>(object);
    model->model->wait_until_loaded();
    if (model->model->loading_failed())
    {
      updatePanelStatus("The selected fence model could not be loaded.");
      return false;
    }

    model->recalcExtents();
    // Read the loaded model bounds directly. ModelInstance::getLocalExtents()
    // currently returns a reference to a temporary array in the legacy API.
    glm::vec3 const local_size = glm::abs(
      model->model->bounding_box_max - model->model->bounding_box_min);
    FenceAxis const inferred_axis = local_size.x >= local_size.z ? FenceAxis::local_x : FenceAxis::local_z;
    float const axis_min = inferred_axis == FenceAxis::local_x
      ? model->model->bounding_box_min.x : model->model->bounding_box_min.z;
    float const axis_max = inferred_axis == FenceAxis::local_x
      ? model->model->bounding_box_max.x : model->model->bounding_box_max.z;
    float const inferred_length = std::max(0.1f,
      (axis_max - axis_min) * object->scale);
    float height_offset = 0.0f;
    if (auto const ground = mapView()->getWorld()->try_get_ground_height(object->pos))
      height_offset = object->pos.y - ground->y;

    PatternSource& source = role == PickRole::section ? _section_source : _post_source;
    auto const new_file_key = model->model->file_key();
    if (source.file_key && source.file_key->stringRepr() != new_file_key.stringRepr())
    {
      _captured_pattern.clear();
      _captured_pattern_groups.clear();
      _captured_pattern_length = 0.0f;
      _captured_full_corridor = false;
      _pattern_locked = false;
      if (_side_combo)
        _side_combo->setEnabled(true);
      if (_offset_spin)
        _offset_spin->setEnabled(true);
    }
    source.file_key = new_file_key;
    source.name = source.file_key->stringRepr();
    source.scale = object->scale;
    source.rotation = object->dir;
    source.height_offset = height_offset;
    source.inferred_length = inferred_length;
    source.bounds_min = model->model->bounding_box_min;
    source.bounds_max = model->model->bounding_box_max;
    source.inferred_axis = inferred_axis;

    if (role == PickRole::section)
    {
      if (_axis_combo)
        _axis_combo->setCurrentIndex(0);
      if (_spacing_spin)
        _spacing_spin->setValue(inferred_length);
      if (_height_offset_spin)
        _height_offset_spin->setValue(height_offset);
    }
    else if (_post_height_offset_spin)
    {
      _post_height_offset_spin->setValue(height_offset);
    }

    _pick_role = PickRole::none;
    refreshPatternLabel();
    rebuildPlacementPreview();
    updatePanelStatus(role == PickRole::section
      ? "Fence section captured. Now pick the post M2."
      : "Fence post captured. The pattern is ready once both roles are filled.");
    return true;
  }

  bool FenceTool::pickSourceAt(QPoint const& mouse_position)
  {
    PickRole const role = _pick_role;
    for (auto const& result : mapView()->intersect_result(mouse_position, false, true))
    {
      if (result.second.index() != eEntry_Object)
        continue;
      if (captureSource(std::get<selected_object_type>(result.second), role))
        return true;
    }

    updatePanelStatus(role == PickRole::post
      ? "No stored M2 post was found under that click. Pick Post M2 remains active."
      : "No stored M2 fence section was found under that click. Pick Section M2 remains active.");
    return false;
  }

  std::optional<glm::vec3> FenceTool::terrainPointAt(QPoint const& mouse_position)
  {
    for (auto const& result : mapView()->intersect_result(mouse_position, true, false))
    {
      if (result.second.index() == eEntry_MapChunk)
        return std::get<selected_chunk_type>(result.second).position;
    }
    return std::nullopt;
  }

  void FenceTool::beginPatternAreaCapture()
  {
    _pick_role = PickRole::none;
    _capturing_pattern_area = true;
    _pattern_locked = false;
    _painting_path = false;
    _reference_stroke_active = false;
    _reference_erase_stroke = false;
    _reference_last_brush_position.reset();
    _reference_mask.clear();
    _reference_mask_lines.clear();
    _reference_centerline.clear();
    _captured_pattern.clear();
    _captured_pattern_groups.clear();
    _captured_pattern_length = 0.0f;
    _captured_full_corridor = false;
    if (_side_combo)
      _side_combo->setEnabled(true);
    if (_offset_spin)
      _offset_spin->setEnabled(true);
    _live_guide.clear();
    clearPath();
    refreshPatternLabel();
    updatePanelStatus("Paint the reference corridor blue with the left mouse button. Increase the brush radius to cover both fence edges; Ctrl+paint erases. Choose OK - Use Selection when finished.");
    mapView()->invalidate();
  }

  void FenceTool::paintPatternReference(glm::vec3 const& point, bool erase)
  {
    if (!_capturing_pattern_area)
      return;

    constexpr float grid_step = 1.0f;
    if (_reference_last_brush_position
        && horizontalDistance(*_reference_last_brush_position, point) < grid_step * 0.2f)
      return;

    float const radius = std::max(grid_step,
      _capture_width_spin ? static_cast<float>(_capture_width_spin->value()) : 8.0f);
    glm::vec3 const stroke_start = _reference_last_brush_position.value_or(point);
    float const stroke_length = horizontalDistance(stroke_start, point);
    float const stamp_spacing = std::max(grid_step, radius * 0.22f);
    int const stamp_count = std::max(1,
      static_cast<int>(std::ceil(stroke_length / stamp_spacing)));

    for (int stamp_index = 0; stamp_index <= stamp_count; ++stamp_index)
    {
      float const fraction = static_cast<float>(stamp_index)
        / static_cast<float>(stamp_count);
      glm::vec3 const stamp = stroke_start * (1.0f - fraction) + point * fraction;
      int const min_x = static_cast<int>(std::floor((stamp.x - radius) / grid_step));
      int const max_x = static_cast<int>(std::floor((stamp.x + radius) / grid_step));
      int const min_z = static_cast<int>(std::floor((stamp.z - radius) / grid_step));
      int const max_z = static_cast<int>(std::floor((stamp.z + radius) / grid_step));
      for (int grid_z = min_z; grid_z <= max_z; ++grid_z)
      {
        for (int grid_x = min_x; grid_x <= max_x; ++grid_x)
        {
          glm::vec2 const cell_center{
            (static_cast<float>(grid_x) + 0.5f) * grid_step,
            (static_cast<float>(grid_z) + 0.5f) * grid_step
          };
          if (glm::distance(cell_center, glm::vec2{stamp.x, stamp.z}) > radius)
            continue;

          std::pair<int, int> const key{grid_z, grid_x};
          if (erase)
            _reference_mask.erase(key);
          else
            _reference_mask[key] = 1.0f;
        }
      }
    }

    _reference_last_brush_position = point;
    rebuildPatternReferencePreview();
    updatePanelStatus(QString("Reference selection: %1 painted cell(s). Ctrl+paint erases; choose OK - Use Selection when finished.")
      .arg(_reference_mask.size()));
    mapView()->invalidate();
  }

  void FenceTool::rebuildPatternReferencePreview()
  {
    _reference_mask_lines.clear();
    if (_reference_mask.empty())
      return;

    constexpr float grid_step = 1.0f;
    auto append_point = [this](std::vector<glm::vec3>& line, int grid_x, int grid_z)
    {
      glm::vec3 position{
        (static_cast<float>(grid_x) + 0.5f) * grid_step,
        0.0f,
        (static_cast<float>(grid_z) + 0.5f) * grid_step
      };
      position = terrainPosition(mapView()->getWorld(), position);
      position.y += 0.35f;
      line.push_back(position);
    };
    auto flush_line = [this](std::vector<glm::vec3>& line)
    {
      if (line.size() == 1)
      {
        glm::vec3 left = line.front();
        glm::vec3 right = line.front();
        left.x -= 0.45f;
        right.x += 0.45f;
        line = {left, right};
      }
      if (line.size() >= 2)
        _reference_mask_lines.push_back(line);
      line.clear();
    };

    std::vector<glm::vec3> current_line;
    int current_z = std::numeric_limits<int>::min();
    int previous_x = std::numeric_limits<int>::min();
    for (auto const& [key, strength] : _reference_mask)
    {
      int const grid_z = key.first;
      int const grid_x = key.second;
      if (grid_z != current_z || (previous_x != std::numeric_limits<int>::min()
          && grid_x != previous_x + 1))
        flush_line(current_line);
      append_point(current_line, grid_x, grid_z);
      current_z = grid_z;
      previous_x = grid_x;
    }
    flush_line(current_line);
  }

  bool FenceTool::extractPatternReferenceCenterline()
  {
    using MaskCell = std::pair<int, int>; // z, x
    if (_reference_mask.empty())
      return false;

    std::set<MaskCell> remaining;
    for (auto const& [cell, strength] : _reference_mask)
    {
      if (strength >= 0.25f)
        remaining.insert(cell);
    }

    auto neighbors = [](MaskCell const& cell)
    {
      std::array<MaskCell, 8> result{};
      std::size_t index = 0;
      for (int dz = -1; dz <= 1; ++dz)
      {
        for (int dx = -1; dx <= 1; ++dx)
        {
          if (dx || dz)
            result[index++] = {cell.first + dz, cell.second + dx};
        }
      }
      return result;
    };

    std::set<MaskCell> selected_component;
    std::size_t discarded_cells = 0;
    while (!remaining.empty())
    {
      std::set<MaskCell> component;
      std::queue<MaskCell> pending;
      pending.push(*remaining.begin());
      remaining.erase(remaining.begin());
      while (!pending.empty())
      {
        MaskCell const cell = pending.front();
        pending.pop();
        component.insert(cell);
        for (MaskCell const& neighbor : neighbors(cell))
        {
          auto found = remaining.find(neighbor);
          if (found != remaining.end())
          {
            pending.push(*found);
            remaining.erase(found);
          }
        }
      }
      if (component.size() > selected_component.size())
      {
        discarded_cells += selected_component.size();
        selected_component = std::move(component);
      }
      else
      {
        discarded_cells += component.size();
      }
    }

    if (selected_component.size() < 12
        || discarded_cells > std::max<std::size_t>(16, selected_component.size() / 8))
    {
      updatePanelStatus("The painted reference is too small or split into disconnected areas. Paint one continuous road-shaped corridor.");
      return false;
    }

    // Reduce the painted corridor to a one-cell-wide medial skeleton. Its
    // longest connected route becomes the curved reference centerline.
    std::set<MaskCell> skeleton = selected_component;
    auto occupied = [&](int z, int x) { return skeleton.count({z, x}) != 0; };
    bool changed = true;
    while (changed)
    {
      changed = false;
      for (int sub_iteration = 0; sub_iteration < 2; ++sub_iteration)
      {
        std::vector<MaskCell> remove;
        for (MaskCell const& cell : skeleton)
        {
          int const z = cell.first;
          int const x = cell.second;
          std::array<int, 8> const p{
            occupied(z - 1, x), occupied(z - 1, x + 1), occupied(z, x + 1),
            occupied(z + 1, x + 1), occupied(z + 1, x), occupied(z + 1, x - 1),
            occupied(z, x - 1), occupied(z - 1, x - 1)
          };
          int const count = std::accumulate(p.begin(), p.end(), 0);
          int transitions = 0;
          for (std::size_t index = 0; index < p.size(); ++index)
            transitions += !p[index] && p[(index + 1) % p.size()];
          bool const first_triplet = sub_iteration == 0
            ? p[0] * p[2] * p[4] == 0 : p[0] * p[2] * p[6] == 0;
          bool const second_triplet = sub_iteration == 0
            ? p[2] * p[4] * p[6] == 0 : p[0] * p[4] * p[6] == 0;
          if (count >= 2 && count <= 6 && transitions == 1
              && first_triplet && second_triplet)
            remove.push_back(cell);
        }
        for (MaskCell const& cell : remove)
          skeleton.erase(cell);
        changed = changed || !remove.empty();
      }
    }

    if (skeleton.size() < 2)
    {
      updatePanelStatus("The painted selection does not form a usable road-shaped corridor.");
      return false;
    }

    using QueueEntry = std::pair<float, MaskCell>;
    auto farthest_from = [&](MaskCell const& start,
                             std::map<MaskCell, MaskCell>* parents)
    {
      std::priority_queue<QueueEntry, std::vector<QueueEntry>,
        std::greater<QueueEntry>> pending;
      std::map<MaskCell, float> distances;
      distances[start] = 0.0f;
      pending.push({0.0f, start});
      MaskCell farthest = start;
      while (!pending.empty())
      {
        auto const [distance, cell] = pending.top();
        pending.pop();
        if (distance > distances[cell] + 0.0001f)
          continue;
        if (distance > distances[farthest])
          farthest = cell;
        for (MaskCell const& neighbor : neighbors(cell))
        {
          if (!skeleton.count(neighbor))
            continue;
          bool const diagonal = neighbor.first != cell.first
            && neighbor.second != cell.second;
          float const candidate = distance + (diagonal ? 1.41421356f : 1.0f);
          auto existing = distances.find(neighbor);
          if (existing == distances.end() || candidate < existing->second)
          {
            distances[neighbor] = candidate;
            if (parents)
              (*parents)[neighbor] = cell;
            pending.push({candidate, neighbor});
          }
        }
      }
      return std::pair<MaskCell, float>{farthest, distances[farthest]};
    };

    MaskCell const first_end = farthest_from(*skeleton.begin(), nullptr).first;
    std::map<MaskCell, MaskCell> parents;
    MaskCell const second_end = farthest_from(first_end, &parents).first;
    std::vector<MaskCell> path;
    for (MaskCell cell = second_end;;)
    {
      path.push_back(cell);
      if (cell == first_end)
        break;
      auto parent = parents.find(cell);
      if (parent == parents.end())
      {
        updatePanelStatus("Could not determine a continuous centerline through the reference.");
        return false;
      }
      cell = parent->second;
    }
    std::reverse(path.begin(), path.end());

    constexpr float grid_step = 1.0f;
    _reference_centerline.clear();
    _reference_centerline.reserve(path.size());
    for (MaskCell const& cell : path)
    {
      glm::vec3 position{
        (static_cast<float>(cell.second) + 0.5f) * grid_step,
        0.0f,
        (static_cast<float>(cell.first) + 0.5f) * grid_step
      };
      _reference_centerline.push_back(terrainPosition(mapView()->getWorld(), position));
    }

    for (int pass = 0; pass < 3 && _reference_centerline.size() >= 3; ++pass)
    {
      std::vector<glm::vec3> smoothed = _reference_centerline;
      for (std::size_t index = 1; index + 1 < smoothed.size(); ++index)
      {
        smoothed[index] = _reference_centerline[index - 1] * 0.25f
          + _reference_centerline[index] * 0.5f
          + _reference_centerline[index + 1] * 0.25f;
        smoothed[index] = terrainPosition(mapView()->getWorld(), smoothed[index]);
      }
      _reference_centerline = std::move(smoothed);
    }
    return true;
  }

  bool FenceTool::capturePatternArea()
  {
    if (!_capturing_pattern_area || _reference_mask.empty())
    {
      updatePanelStatus("Choose Select Reference and paint blue over an existing fence corridor before choosing OK.");
      return false;
    }

    if (!extractPatternReferenceCenterline())
      return false;

    std::vector<float> reference_cumulative(_reference_centerline.size(), 0.0f);
    for (std::size_t index = 1; index < _reference_centerline.size(); ++index)
    {
      reference_cumulative[index] = reference_cumulative[index - 1]
        + horizontalDistance(_reference_centerline[index - 1],
                             _reference_centerline[index]);
    }
    float const length = reference_cumulative.back();
    if (length < 4.0f)
    {
      updatePanelStatus("The painted reference is too short. Paint across at least one complete fence group and its following gap.");
      return false;
    }

    struct ReferenceProjection
    {
      float distance = 0.0f;
      float lateral = 0.0f;
      float yaw = 0.0f;
    };
    auto project_to_reference = [&](glm::vec3 const& position)
    {
      ReferenceProjection result;
      float best_distance_squared = std::numeric_limits<float>::max();
      glm::vec2 const point{position.x, position.z};
      for (std::size_t index = 0; index + 1 < _reference_centerline.size(); ++index)
      {
        glm::vec2 const start{
          _reference_centerline[index].x, _reference_centerline[index].z};
        glm::vec2 const end{
          _reference_centerline[index + 1].x, _reference_centerline[index + 1].z};
        glm::vec2 const segment = end - start;
        float const segment_length_squared = glm::dot(segment, segment);
        if (segment_length_squared < 0.0001f)
          continue;
        float const t = std::clamp(glm::dot(point - start, segment)
          / segment_length_squared, 0.0f, 1.0f);
        glm::vec2 const projected = start + segment * t;
        float const distance_squared = glm::dot(point - projected, point - projected);
        if (distance_squared >= best_distance_squared)
          continue;

        best_distance_squared = distance_squared;
        float const segment_length = std::sqrt(segment_length_squared);
        glm::vec2 const tangent = segment / segment_length;
        glm::vec2 const normal{-tangent.y, tangent.x};
        result.distance = reference_cumulative[index] + t * segment_length;
        result.lateral = glm::dot(point - projected, normal);
        result.yaw = std::atan2(tangent.x, tangent.y) * 180.0f / pi;
      }
      return result;
    };

    constexpr float grid_step = 1.0f;

    struct AssetCandidate
    {
      std::string name;
      ModelInstance* representative = nullptr;
      std::size_t count = 0;
      float long_extent_total = 0.0f;
      float short_extent_total = 0.0f;
      float vertical_extent_total = 0.0f;
    };

    std::vector<ModelInstance*> inside_objects;
    std::vector<AssetCandidate> candidates;
    std::unordered_map<std::string, std::size_t> candidate_lookup;

    mapView()->getWorld()->getModelInstanceStorage().for_each_m2_instance(
      [&](ModelInstance& object)
      {
        if (object.chunk_mover_preview)
          return;

        int const grid_x = static_cast<int>(std::floor(object.pos.x / grid_step));
        int const grid_z = static_cast<int>(std::floor(object.pos.z / grid_step));
        if (_reference_mask.find({grid_z, grid_x}) == _reference_mask.end())
          return;

        object.model->wait_until_loaded();
        if (object.model->loading_failed())
          return;

        inside_objects.push_back(&object);
        std::string const name = object.model->file_key().stringRepr();
        auto [iterator, inserted] = candidate_lookup.emplace(name, candidates.size());
        if (inserted)
          candidates.push_back({name, &object, 0, 0.0f, 0.0f, 0.0f});

        AssetCandidate& candidate = candidates[iterator->second];
        glm::vec3 const local_size = glm::abs(
          object.model->bounding_box_max - object.model->bounding_box_min);
        candidate.long_extent_total += std::max(local_size.x, local_size.z) * object.scale;
        candidate.short_extent_total += std::min(local_size.x, local_size.z) * object.scale;
        candidate.vertical_extent_total += local_size.y * object.scale;
        ++candidate.count;
      });

    if (candidates.size() < 2)
    {
      updatePanelStatus(QString("The painted reference contained %1 distinct M2 asset type(s). Paint over both fence sections and posts, then try OK again.")
        .arg(candidates.size()));
      mapView()->invalidate();
      return false;
    }

    auto average_long = [](AssetCandidate const& candidate)
    {
      return candidate.long_extent_total / static_cast<float>(candidate.count);
    };
    auto elongation = [&](AssetCandidate const& candidate)
    {
      float const average_short = candidate.short_extent_total
        / static_cast<float>(candidate.count);
      return average_long(candidate) / std::max(0.05f, average_short);
    };

    auto section_name_score = [](AssetCandidate const& candidate)
    {
      std::string const full_name = lowercase(candidate.name);
      std::string const basename = assetBasename(full_name);
      if (containsAny(basename,
          {"grass", "bush", "shrub", "flower", "fern", "plant", "weed", "tree", "root", "herb"}))
        return -1000.0f;

      float score = 0.0f;
      if (containsAny(basename, {"fence", "rail", "railing"}))
        score += 300.0f;
      else if (containsAny(basename, {"wall", "barrier", "barricade"}))
        score += 120.0f;
      if (containsAny(basename, {"post", "pole", "stake", "endcap"}))
        score -= 240.0f;
      if (containsAny(full_name, {"/fence/", "/fences/", "\\fence\\", "\\fences\\"}))
        score += 25.0f;
      return score;
    };

    auto post_name_score = [&](AssetCandidate const& candidate)
    {
      std::string const full_name = lowercase(candidate.name);
      std::string const basename = assetBasename(full_name);
      if (containsAny(basename,
          {"grass", "bush", "shrub", "flower", "fern", "plant", "weed", "tree", "root", "herb"}))
        return -1000.0f;

      float score = 0.0f;
      if (containsAny(basename, {"post", "pole", "stake", "endcap"}))
        score += 300.0f;
      if (containsAny(basename, {"lamppost", "lightpost", "signpost", "mailpost"}))
        score -= 700.0f;
      if (full_name.find("fence") != std::string::npos)
        score += 120.0f;
      if (containsAny(basename, {"fence", "rail", "railing"})
          && !containsAny(basename, {"post", "pole", "stake", "endcap"}))
        score -= 100.0f;
      return score;
    };

    AssetCandidate* section_candidate = &*std::max_element(candidates.begin(), candidates.end(),
      [&](AssetCandidate const& first, AssetCandidate const& second)
      {
        float const first_score = section_name_score(first)
          + std::min(20.0f, elongation(first)) * 4.0f
          + std::min<std::size_t>(first.count, 20) * 0.03f;
        float const second_score = section_name_score(second)
          + std::min(20.0f, elongation(second)) * 4.0f
          + std::min<std::size_t>(second.count, 20) * 0.03f;
        return first_score < second_score;
      });
    if (section_name_score(*section_candidate) <= -900.0f)
    {
      updatePanelStatus("Only vegetation-like elongated M2s were found. Paint blue across the fence section itself and try OK again.");
      return false;
    }
    if (section_name_score(*section_candidate) < 100.0f
        && elongation(*section_candidate) < 1.5f)
    {
      updatePanelStatus("No elongated fence-section M2 was found in the painted reference. Narrow the mask around the fence objects and try OK again.");
      return false;
    }

    AssetCandidate* post_candidate = nullptr;
    float best_post_score = std::numeric_limits<float>::lowest();
    std::string const section_directory = assetDirectory(lowercase(section_candidate->name));
    for (AssetCandidate& candidate : candidates)
    {
      if (&candidate == section_candidate)
        continue;
      float const candidate_long = average_long(candidate);
      float const candidate_vertical = candidate.vertical_extent_total
        / static_cast<float>(candidate.count);
      float const vertical_slenderness = candidate_vertical / std::max(0.1f, candidate_long);
      std::string const candidate_directory = assetDirectory(lowercase(candidate.name));
      float const family_score = !section_directory.empty()
        && candidate_directory == section_directory ? 450.0f : 0.0f;
      float const score = post_name_score(candidate) + family_score + vertical_slenderness
        * std::sqrt(static_cast<float>(candidate.count))
        / std::sqrt(std::max(0.1f, candidate_long));
      if (score > best_post_score)
      {
        best_post_score = score;
        post_candidate = &candidate;
      }
    }
    if (!post_candidate || post_name_score(*post_candidate) <= -900.0f)
    {
      updatePanelStatus("No fence-post M2 was found in the painted reference. Paint across at least one post and try OK again.");
      return false;
    }

    captureSource(section_candidate->representative, PickRole::section);
    captureSource(post_candidate->representative, PickRole::post);
    std::string const section_name = section_candidate->name;
    std::string const post_name = post_candidate->name;

    struct SourcePatternObject
    {
      ModelInstance* object = nullptr;
      PieceKind kind = PieceKind::section;
      ReferenceProjection projection;
      float height_offset = 0.0f;
    };

    using SourceObjectKey = std::tuple<int, long long, long long, long long,
      long long, long long, long long, long long>;
    std::set<SourceObjectKey> source_object_keys;
    std::vector<SourcePatternObject> source_objects;
    for (ModelInstance* object : inside_objects)
    {
      std::string const object_name = object->model->file_key().stringRepr();
      PieceKind kind;
      if (object_name == section_name)
        kind = PieceKind::section;
      else if (object_name == post_name)
        kind = PieceKind::post;
      else
        continue;

      auto quantize = [](float value, float precision)
      {
        return std::llround(static_cast<double>(value) * precision);
      };
      SourceObjectKey const source_key{
        static_cast<int>(kind),
        quantize(object->pos.x, 100.0f),
        quantize(object->pos.y, 100.0f),
        quantize(object->pos.z, 100.0f),
        quantize(object->dir.x, 10.0f),
        quantize(object->dir.y, 10.0f),
        quantize(object->dir.z, 10.0f),
        quantize(object->scale, 1000.0f)
      };
      if (!source_object_keys.insert(source_key).second)
        continue;

      float height_offset = 0.0f;
      if (auto const ground = mapView()->getWorld()->try_get_ground_height(object->pos))
        height_offset = object->pos.y - ground->y;
      source_objects.push_back(
        {object, kind, project_to_reference(object->pos), height_offset});
    }

    if (source_objects.empty())
    {
      updatePanelStatus("The highlighted strip contained no matching section or post M2 origins. Increase its width or redraw it.");
      mapView()->invalidate();
      return false;
    }

    // Fence pieces that touch belong to one rigid source group. Mapping a
    // group's pieces through one local frame preserves exact rail/post joins;
    // separate groups can still bend independently around the destination road.
    std::vector<std::size_t> parents(source_objects.size());
    std::iota(parents.begin(), parents.end(), std::size_t{0});
    auto find_root = [&](std::size_t index)
    {
      while (parents[index] != index)
      {
        parents[index] = parents[parents[index]];
        index = parents[index];
      }
      return index;
    };
    auto unite = [&](std::size_t first, std::size_t second)
    {
      std::size_t const first_root = find_root(first);
      std::size_t const second_root = find_root(second);
      if (first_root != second_root)
        parents[second_root] = first_root;
    };
    float const join_distance = std::max(2.0f,
      _section_source.inferred_length * 1.35f);
    for (std::size_t first = 0; first < source_objects.size(); ++first)
    {
      for (std::size_t second = first + 1; second < source_objects.size(); ++second)
      {
        if (horizontalDistance(source_objects[first].object->pos,
                               source_objects[second].object->pos) <= join_distance)
          unite(first, second);
      }
    }

    std::map<std::size_t, std::vector<std::size_t>> components;
    for (std::size_t index = 0; index < source_objects.size(); ++index)
      components[find_root(index)].push_back(index);

    std::vector<CapturedPatternObject> captured;
    std::vector<CapturedPatternGroup> captured_groups;
    float const section_axis_yaw = resolvedAxis() == FenceAxis::local_z
      ? 90.0f : 0.0f;
    for (auto const& component : components)
    {
      std::vector<std::size_t> const& indices = component.second;
      glm::vec3 center{};
      for (std::size_t const index : indices)
        center += source_objects[index].object->pos;
      center /= static_cast<float>(indices.size());

      ReferenceProjection const group_projection = project_to_reference(center);
      float const reference_yaw_radians = group_projection.yaw * pi / 180.0f;
      glm::vec2 reference_tangent{
        std::sin(reference_yaw_radians), std::cos(reference_yaw_radians)};
      glm::vec2 group_tangent{};
      for (std::size_t const index : indices)
      {
        SourcePatternObject const& source = source_objects[index];
        if (source.kind != PieceKind::section)
          continue;
        float const yaw = (source.object->dir.y - section_axis_yaw) * pi / 180.0f;
        glm::vec2 candidate{std::sin(yaw), std::cos(yaw)};
        if (glm::dot(candidate, reference_tangent) < 0.0f)
          candidate = -candidate;
        group_tangent += candidate;
      }
      if (glm::length(group_tangent) < 0.001f)
        group_tangent = reference_tangent;
      else
        group_tangent = glm::normalize(group_tangent);
      glm::vec2 const group_normal{-group_tangent.y, group_tangent.x};
      float const group_yaw = std::atan2(group_tangent.x, group_tangent.y)
        * 180.0f / pi;

      std::size_t const group_index = captured_groups.size();
      captured_groups.push_back({
        group_projection.distance,
        group_projection.lateral,
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest()
      });
      for (std::size_t const index : indices)
      {
        SourcePatternObject const& source = source_objects[index];
        glm::vec2 const delta{
          source.object->pos.x - center.x, source.object->pos.z - center.z};
        glm::vec2 const local_offset{
          glm::dot(delta, group_tangent), glm::dot(delta, group_normal)};
        float minimum_distance = local_offset.x - 0.25f;
        float maximum_distance = local_offset.x + 0.25f;
        if (source.kind == PieceKind::section)
        {
          FenceAxis const axis = resolvedAxis();
          float const axis_min = (axis == FenceAxis::local_x
            ? _section_source.bounds_min.x : _section_source.bounds_min.z)
            * source.object->scale;
          float const axis_max = (axis == FenceAxis::local_x
            ? _section_source.bounds_max.x : _section_source.bounds_max.z)
            * source.object->scale;
          float const yaw = (source.object->dir.y - section_axis_yaw)
            * pi / 180.0f;
          glm::vec2 const object_tangent{std::sin(yaw), std::cos(yaw)};
          float const axis_sign = glm::dot(object_tangent, group_tangent) < 0.0f
            ? -1.0f : 1.0f;
          minimum_distance = local_offset.x
            + std::min(axis_min * axis_sign, axis_max * axis_sign);
          maximum_distance = local_offset.x
            + std::max(axis_min * axis_sign, axis_max * axis_sign);
        }
        CapturedPatternGroup& group = captured_groups.back();
        group.minimum_local_distance = std::min(
          group.minimum_local_distance, minimum_distance);
        group.maximum_local_distance = std::max(
          group.maximum_local_distance, maximum_distance);

        glm::vec3 rotation = source.object->dir;
        rotation.y = std::remainder(rotation.y - group_yaw, 360.0f);
        captured.push_back({
          group_projection.distance + local_offset.x,
          group_projection.lateral + local_offset.y,
          source.height_offset,
          source.object->scale,
          rotation,
          source.kind,
          group_index,
          local_offset
        });
      }
    }

    // The selected strip is repeated cyclically. Put its wrap boundary in the
    // widest truly empty interval so a rigid group at the end cannot collide
    // with a different group at the beginning of the next repetition.
    if (!captured_groups.empty())
    {
      std::vector<std::size_t> group_order(captured_groups.size());
      std::iota(group_order.begin(), group_order.end(), std::size_t{0});
      std::sort(group_order.begin(), group_order.end(),
        [&](std::size_t first, std::size_t second)
        {
          return captured_groups[first].distance
            < captured_groups[second].distance;
        });

      float largest_gap = std::numeric_limits<float>::lowest();
      float seam_distance = 0.0f;
      for (std::size_t order_index = 0;
           order_index < group_order.size(); ++order_index)
      {
        CapturedPatternGroup const& current
          = captured_groups[group_order[order_index]];
        bool const wraps = order_index + 1 == group_order.size();
        CapturedPatternGroup const& next = captured_groups[
          group_order[wraps ? 0 : order_index + 1]];
        float const current_end = current.distance
          + current.maximum_local_distance;
        float const next_start = next.distance + next.minimum_local_distance
          + (wraps ? length : 0.0f);
        float const gap = next_start - current_end;
        if (gap > largest_gap)
        {
          largest_gap = gap;
          seam_distance = current_end + gap * 0.5f;
        }
      }

      if (largest_gap > 0.01f)
      {
        seam_distance = std::fmod(seam_distance, length);
        if (seam_distance < 0.0f)
          seam_distance += length;
        for (CapturedPatternGroup& group : captured_groups)
        {
          group.distance = std::fmod(
            group.distance - seam_distance + length, length);
        }
        for (CapturedPatternObject& object : captured)
        {
          object.distance = captured_groups[object.group_index].distance
            + object.group_local_offset.x;
        }
      }
    }

    std::sort(captured.begin(), captured.end(),
      [](CapturedPatternObject const& first, CapturedPatternObject const& second)
      {
        return first.distance < second.distance;
      });

    _captured_pattern = std::move(captured);
    _captured_pattern_groups = std::move(captured_groups);
    _captured_pattern_length = length;
    _pattern_locked = true;
    _captured_full_corridor = true;
    _capturing_pattern_area = false;
    _reference_stroke_active = false;
    _reference_erase_stroke = false;
    _reference_last_brush_position.reset();
    _reference_mask.clear();
    _reference_mask_lines.clear();
    _reference_centerline.clear();
    if (_side_combo)
      _side_combo->setEnabled(false);
    if (_offset_spin)
      _offset_spin->setEnabled(false);
    clearPath();
    refreshPatternLabel();
    rebuildPlacementPreview();
    std::size_t const sections = std::count_if(_captured_pattern.begin(), _captured_pattern.end(),
      [](CapturedPatternObject const& object) { return object.kind == PieceKind::section; });
    updatePanelStatus(QString("Reference accepted: %1 section(s) and %2 post(s) across %3 units. Section: %4; post: %5. Hold left mouse and drag along the new road centerline.")
      .arg(sections)
      .arg(_captured_pattern.size() - sections)
      .arg(length, 0, 'f', 2)
      .arg(QString::fromStdString(assetBasename(section_name)))
      .arg(QString::fromStdString(assetBasename(post_name)))
      );
    return true;
  }

  void FenceTool::clearCapturedPattern()
  {
    _capturing_pattern_area = false;
    _reference_stroke_active = false;
    _reference_erase_stroke = false;
    _reference_last_brush_position.reset();
    _reference_mask.clear();
    _reference_mask_lines.clear();
    _reference_centerline.clear();
    _captured_pattern.clear();
    _captured_pattern_groups.clear();
    _captured_pattern_length = 0.0f;
    _captured_full_corridor = false;
    _pattern_locked = false;
    _painting_path = false;
    if (_side_combo)
      _side_combo->setEnabled(true);
    if (_offset_spin)
      _offset_spin->setEnabled(true);
    refreshPatternLabel();
    rebuildPlacementPreview();
    updatePanelStatus("Captured area pattern cleared. Continuous spacing mode is active.");
  }

  void FenceTool::refreshPatternLabel()
  {
    if (!_pattern_label)
      return;

    QString const section = _section_source.file_key
      ? QString("Section: %1\n  Scale %2; length %3; axis %4")
          .arg(QString::fromStdString(_section_source.name))
          .arg(_section_source.scale, 0, 'f', 3)
          .arg(_section_source.inferred_length, 0, 'f', 2)
          .arg(_section_source.inferred_axis == FenceAxis::local_x ? "Local X" : "Local Z")
      : QString("Section: not captured");
    QString const post = _post_source.file_key
      ? QString("Post: %1\n  Scale %2")
          .arg(QString::fromStdString(_post_source.name))
          .arg(_post_source.scale, 0, 'f', 3)
      : QString("Post: not captured");
    QString const area = _captured_pattern.empty()
      ? QString("Area pattern: not captured (continuous mode)")
      : QString("Reference pattern: %1 object(s) across %2 units (%3)")
          .arg(_captured_pattern.size())
          .arg(_captured_pattern_length, 0, 'f', 2)
          .arg(_captured_full_corridor ? "full corridor" : "single run");
    _pattern_label->setText(section + "\n" + post + "\n" + area);
  }

  FenceTool::FenceAxis FenceTool::resolvedAxis() const
  {
    if (!_axis_combo || _axis_combo->currentIndex() == 0)
      return _section_source.inferred_axis;
    return _axis_combo->currentIndex() == 1 ? FenceAxis::local_x : FenceAxis::local_z;
  }

  void FenceTool::addControlPoint(glm::vec3 point)
  {
    point = terrainPosition(mapView()->getWorld(), point);
    if (!_control_points.empty() && horizontalDistance(_control_points.back(), point) < 0.5f)
    {
      updatePanelStatus("That path point is too close to the previous point.");
      return;
    }

    _control_points.push_back(point);
    rebuildPreview();
  }

  void FenceTool::removeLastControlPoint()
  {
    if (!_control_points.empty())
      _control_points.pop_back();
    rebuildPreview();
  }

  void FenceTool::clearPath()
  {
    _control_points.clear();
    _path_preview.clear();
    _left_preview.clear();
    _right_preview.clear();
    _live_guide.clear();
    _placements.clear();
    _placement_segments.clear();
    _post_segments.clear();
    updatePanelStatus("Path cleared. Click to place a new start point.");
    mapView()->invalidate();
  }

  void FenceTool::rebuildPreview()
  {
    _path_preview.clear();
    _live_guide.clear();

    if (_control_points.size() == 1)
      _path_preview = _control_points;
    else if (_control_points.size() >= 2)
    {
      float const sample_step = std::clamp(
        _spacing_spin ? static_cast<float>(_spacing_spin->value()) / 3.0f : 1.0f,
        0.5f, 2.0f);

      // Uniform Catmull-Rom can overshoot or briefly reverse at an angled
      // joint when freehand control-point spacing is uneven. Chaikin corner
      // cutting stays inside each source segment, so fence groups cannot gain
      // a phantom backtracking placement while the path still bends smoothly.
      std::vector<glm::vec3> smoothed = _control_points;
      for (int pass = 0; pass < 2 && smoothed.size() >= 3; ++pass)
      {
        std::vector<glm::vec3> next;
        next.reserve(smoothed.size() * 2);
        next.push_back(smoothed.front());
        for (std::size_t index = 0; index + 1 < smoothed.size(); ++index)
        {
          next.push_back(smoothed[index] * 0.75f
            + smoothed[index + 1] * 0.25f);
          next.push_back(smoothed[index] * 0.25f
            + smoothed[index + 1] * 0.75f);
        }
        next.push_back(smoothed.back());
        smoothed = std::move(next);
      }

      _path_preview.push_back(
        terrainPosition(mapView()->getWorld(), smoothed.front()));
      for (std::size_t segment = 0; segment + 1 < smoothed.size(); ++segment)
      {
        glm::vec3 const& first = smoothed[segment];
        glm::vec3 const& second = smoothed[segment + 1];
        float const segment_length = horizontalDistance(first, second);
        int const samples = std::max(1,
          static_cast<int>(std::ceil(segment_length / sample_step)));
        for (int sample = 1; sample <= samples; ++sample)
        {
          float const t = static_cast<float>(sample)
            / static_cast<float>(samples);
          glm::vec3 point = first * (1.0f - t) + second * t;
          _path_preview.push_back(terrainPosition(mapView()->getWorld(), point));
        }
      }
    }

    rebuildPlacementPreview();
  }

  std::vector<glm::vec3> FenceTool::buildOffsetPath(float side_sign)
  {
    std::vector<glm::vec3> result;
    result.reserve(_path_preview.size());
    float const offset = _offset_spin ? static_cast<float>(_offset_spin->value()) : 0.0f;

    for (std::size_t index = 0; index < _path_preview.size(); ++index)
    {
      glm::vec3 const& previous = _path_preview[index ? index - 1 : index];
      glm::vec3 const& next = _path_preview[
        index + 1 < _path_preview.size() ? index + 1 : index];
      glm::vec2 direction{next.x - previous.x, next.z - previous.z};
      if (glm::length(direction) < 0.001f)
        direction = {1.0f, 0.0f};
      else
        direction = glm::normalize(direction);

      glm::vec3 point = _path_preview[index]
        + glm::vec3{-direction.y * side_sign * offset, 0.0f,
                     direction.x * side_sign * offset};
      result.push_back(terrainPosition(mapView()->getWorld(), point));
    }
    return result;
  }

  void FenceTool::rebuildPlacementPreview()
  {
    _left_preview.clear();
    _right_preview.clear();
    _placements.clear();
    _placement_segments.clear();
    _post_segments.clear();

    if (_path_preview.size() < 2)
    {
      updatePanelStatus();
      mapView()->invalidate();
      return;
    }

    if (_captured_full_corridor && !_captured_pattern.empty())
    {
      appendPlacements(_path_preview);
    }
    else
    {
      int const side = _side_combo ? _side_combo->currentIndex() : 2;
      if (side == 0 || side == 2)
      {
        _left_preview = buildOffsetPath(1.0f);
        appendPlacements(_left_preview);
      }
      if (side == 1 || side == 2)
      {
        _right_preview = buildOffsetPath(-1.0f);
        appendPlacements(_right_preview);
      }
    }

    for (FencePlacement const& placement : _placements)
    {
      if (placement.kind == PieceKind::post)
      {
        glm::vec3 bottom = placement.position;
        glm::vec3 top = placement.position;
        bottom.y += 0.1f;
        top.y += 1.6f;
        _post_segments.push_back(bottom);
        _post_segments.push_back(top);
        continue;
      }

      FenceAxis const axis = resolvedAxis();
      float const axis_min = axis == FenceAxis::local_x
        ? _section_source.bounds_min.x : _section_source.bounds_min.z;
      float const axis_max = axis == FenceAxis::local_x
        ? _section_source.bounds_max.x : _section_source.bounds_max.z;
      glm::vec3 first = placement.position + placement.direction
        * (axis_min * placement.scale);
      glm::vec3 second = placement.position + placement.direction
        * (axis_max * placement.scale);
      first.y += 0.3f;
      second.y += 0.3f;
      _placement_segments.push_back(first);
      _placement_segments.push_back(second);
    }

    updatePanelStatus();
    mapView()->invalidate();
  }

  void FenceTool::appendPlacements(std::vector<glm::vec3> const& path)
  {
    if (path.size() < 2 || _placements.size() >= maximum_fence_placements)
      return;

    std::vector<float> cumulative(path.size(), 0.0f);
    for (std::size_t index = 1; index < path.size(); ++index)
      cumulative[index] = cumulative[index - 1] + horizontalDistance(path[index - 1], path[index]);

    float const total_length = cumulative.back();
    auto sample_path = [&](float distance, float lateral_offset)
      -> std::pair<glm::vec3, glm::vec2>
    {
      distance = std::clamp(distance, 0.0f, total_length);
      auto upper = std::upper_bound(cumulative.begin(), cumulative.end(), distance);
      std::size_t segment = upper == cumulative.begin()
        ? 0 : static_cast<std::size_t>(std::distance(cumulative.begin(), upper) - 1);
      segment = std::min(segment, path.size() - 2);
      float const segment_length = cumulative[segment + 1] - cumulative[segment];
      float const t = segment_length > 0.001f
        ? (distance - cumulative[segment]) / segment_length : 0.0f;

      glm::vec3 const delta = path[segment + 1] - path[segment];
      glm::vec2 flat_direction{delta.x, delta.z};
      if (glm::length(flat_direction) < 0.001f)
        flat_direction = {1.0f, 0.0f};
      else
        flat_direction = glm::normalize(flat_direction);
      glm::vec3 const lateral{-flat_direction.y, 0.0f, flat_direction.x};
      glm::vec3 position = path[segment] * (1.0f - t) + path[segment + 1] * t
        + lateral * lateral_offset;
      position = terrainPosition(mapView()->getWorld(), position);
      return {position, flat_direction};
    };

    if (!_captured_pattern.empty() && _captured_pattern_length >= 1.0f)
    {
      bool const reverse_pattern = _reverse_pattern_check
        && _reverse_pattern_check->isChecked();
      float const corridor_nudge = _corridor_nudge_spin
        ? static_cast<float>(_corridor_nudge_spin->value()) : 0.0f;
      float const width_scale = _width_scale_spin
        ? static_cast<float>(_width_scale_spin->value()) : 1.0f;
      using PlacementKey = std::tuple<int, long long, long long, long long,
        long long, long long, long long, long long>;
      std::set<PlacementKey> emitted_placements;

      auto append_captured = [&](float repeat_start,
                                 CapturedPatternObject const& captured)
      {
        if (_placements.size() >= maximum_fence_placements
            || captured.group_index >= _captured_pattern_groups.size())
          return;

        CapturedPatternGroup const& group
          = _captured_pattern_groups[captured.group_index];
        float group_distance = group.distance;
        float group_lateral = group.lateral_offset * width_scale;
        glm::vec2 local_offset = captured.group_local_offset;
        float local_yaw = captured.rotation.y;
        if (reverse_pattern)
        {
          group_distance = _captured_pattern_length - group_distance;
          group_lateral = -group_lateral;
          local_offset = -local_offset;
          local_yaw = std::remainder(local_yaw - 180.0f, 360.0f);
        }
        group_lateral += corridor_nudge;
        float const distance = repeat_start + group_distance;
        if (distance < 0.0f || distance > total_length)
          return;

        auto [position, flat_direction] = sample_path(distance, group_lateral);
        glm::vec3 const lateral{-flat_direction.y, 0.0f, flat_direction.x};
        position += glm::vec3{flat_direction.x, 0.0f, flat_direction.y}
          * local_offset.x;
        position += lateral * (local_offset.y * width_scale);
        position = terrainPosition(mapView()->getWorld(), position);
        position.y += captured.height_offset;

        glm::vec3 direction{flat_direction.x, 0.0f, flat_direction.y};
        float const path_yaw = std::atan2(flat_direction.x, flat_direction.y) * 180.0f / pi;
        glm::vec3 rotation = captured.rotation;
        rotation.y = std::remainder(path_yaw + local_yaw, 360.0f);

        if (captured.kind == PieceKind::section
            && _align_slope_check && _align_slope_check->isChecked())
        {
          FenceAxis const axis = resolvedAxis();
          float const yaw_radians = rotation.y * pi / 180.0f;
          glm::vec2 const model_axis_direction = axis == FenceAxis::local_x
            ? glm::vec2{std::sin(yaw_radians), std::cos(yaw_radians)}
            : glm::vec2{-std::cos(yaw_radians), std::sin(yaw_radians)};
          float const axis_min = (axis == FenceAxis::local_x
            ? _section_source.bounds_min.x : _section_source.bounds_min.z)
            * captured.scale;
          float const axis_max = (axis == FenceAxis::local_x
            ? _section_source.bounds_max.x : _section_source.bounds_max.z)
            * captured.scale;

          glm::vec3 first_endpoint = position;
          first_endpoint.x += model_axis_direction.x * axis_min;
          first_endpoint.z += model_axis_direction.y * axis_min;
          first_endpoint = terrainPosition(mapView()->getWorld(), first_endpoint);
          glm::vec3 second_endpoint = position;
          second_endpoint.x += model_axis_direction.x * axis_max;
          second_endpoint.z += model_axis_direction.y * axis_max;
          second_endpoint = terrainPosition(mapView()->getWorld(), second_endpoint);

          float const horizontal_span = std::max(0.001f,
            (axis_max - axis_min));
          if (horizontal_span > 0.001f)
          {
            float const slope = std::atan2(
              second_endpoint.y - first_endpoint.y, horizontal_span);
            position.y = first_endpoint.y + captured.height_offset
              - axis_min * std::sin(slope);
            direction = glm::normalize(glm::vec3{
              model_axis_direction.x * std::cos(slope),
              std::sin(slope),
              model_axis_direction.y * std::cos(slope)
            });
            float const slope_degrees = slope * 180.0f / pi;
            if (axis == FenceAxis::local_x)
              rotation.x = -slope_degrees;
            else
              rotation.z = -slope_degrees;
          }
        }

        auto quantize = [](float value, float precision)
        {
          return std::llround(static_cast<double>(value) * precision);
        };
        PlacementKey const placement_key{
          static_cast<int>(captured.kind),
          quantize(position.x, 100.0f),
          quantize(position.y, 100.0f),
          quantize(position.z, 100.0f),
          quantize(rotation.x, 10.0f),
          quantize(rotation.y, 10.0f),
          quantize(rotation.z, 10.0f),
          quantize(captured.scale, 1000.0f)
        };
        if (!emitted_placements.insert(placement_key).second)
          return;

        _placements.push_back(
          {position, direction, rotation, captured.kind, captured.scale});
      };

      for (float repeat_start = 0.0f;
           repeat_start < total_length && _placements.size() < maximum_fence_placements;
           repeat_start += _captured_pattern_length)
      {
        for (CapturedPatternObject const& captured : _captured_pattern)
          append_captured(repeat_start, captured);
      }
      return;
    }

    float const requested_spacing = std::max(0.1f,
      _spacing_spin ? static_cast<float>(_spacing_spin->value()) : 1.0f);
    int const piece_count = std::max(1, static_cast<int>(std::ceil(total_length / requested_spacing)));
    float const fitted_spacing = total_length / static_cast<float>(piece_count);
    FenceAxis const axis = resolvedAxis();
    float const yaw_correction = _yaw_offset_spin
      ? static_cast<float>(_yaw_offset_spin->value()) : 0.0f;

    auto append_at_distance = [&](float distance, PieceKind kind)
    {
      if (_placements.size() >= maximum_fence_placements)
        return;

      auto upper = std::upper_bound(cumulative.begin(), cumulative.end(), distance);
      std::size_t segment = upper == cumulative.begin()
        ? 0 : static_cast<std::size_t>(std::distance(cumulative.begin(), upper) - 1);
      segment = std::min(segment, path.size() - 2);
      float const segment_length = cumulative[segment + 1] - cumulative[segment];
      float const t = segment_length > 0.001f
        ? (distance - cumulative[segment]) / segment_length : 0.0f;
      glm::vec3 position = path[segment] * (1.0f - t) + path[segment + 1] * t;
      position = terrainPosition(mapView()->getWorld(), position);
      float const height_offset = kind == PieceKind::section
        ? (_height_offset_spin ? static_cast<float>(_height_offset_spin->value()) : 0.0f)
        : (_post_height_offset_spin ? static_cast<float>(_post_height_offset_spin->value()) : 0.0f);
      position.y += height_offset;

      glm::vec3 delta = path[segment + 1] - path[segment];
      float const horizontal_length = glm::length(glm::vec2{delta.x, delta.z});
      if (horizontal_length < 0.001f)
        return;
      glm::vec3 direction = delta / std::sqrt(horizontal_length * horizontal_length
                                               + delta.y * delta.y);
      glm::vec2 const flat_direction = glm::normalize(glm::vec2{delta.x, delta.z});
      float const path_yaw = std::atan2(flat_direction.x, flat_direction.y) * 180.0f / pi;
      float const yaw = path_yaw + (axis == FenceAxis::local_z ? 90.0f : 0.0f)
                        + yaw_correction;
      float const slope = std::atan2(delta.y, horizontal_length) * 180.0f / pi;

      glm::vec3 rotation{0.0f, yaw, 0.0f};
      if (_align_slope_check && _align_slope_check->isChecked())
      {
        if (kind == PieceKind::section)
        {
          if (axis == FenceAxis::local_x)
            rotation.x = -slope;
          else
            rotation.z = -slope;
        }
        else
        {
          rotation.x = _post_source.rotation.x;
          rotation.z = _post_source.rotation.z;
        }
      }
      else
      {
        PatternSource const& source = kind == PieceKind::section
          ? _section_source : _post_source;
        rotation.x = source.rotation.x;
        rotation.z = source.rotation.z;
      }

      PatternSource const& source = kind == PieceKind::section
        ? _section_source : _post_source;
      _placements.push_back({position, direction, rotation, kind, source.scale});
    };

    for (int piece = 0; piece < piece_count; ++piece)
      append_at_distance((static_cast<float>(piece) + 0.5f) * fitted_spacing,
                         PieceKind::section);

    if (_post_source.file_key)
    {
      for (int boundary = 0; boundary <= piece_count; ++boundary)
        append_at_distance(static_cast<float>(boundary) * fitted_spacing,
                           PieceKind::post);
    }
  }

  void FenceTool::commitFence()
  {
    if (!_captured_pattern.empty() && !_pattern_locked)
    {
      updatePanelStatus("Lock the captured pattern before committing.");
      return;
    }
    if (!_section_source.file_key)
    {
      updatePanelStatus("Choose a fence section before committing.");
      return;
    }
    if (!_post_source.file_key)
    {
      updatePanelStatus("Choose a fence post before committing.");
      return;
    }
    if (_control_points.size() < 2 || _placements.empty())
    {
      updatePanelStatus("Add at least two path points before committing.");
      return;
    }
    if (_placements.size() >= maximum_fence_placements)
    {
      updatePanelStatus("The preview reached the 5,000-object safety limit. Shorten the path or increase spacing.");
      return;
    }
    if (NOGGIT_CUR_ACTION)
      return;

    NOGGIT_ACTION_MGR->beginAction(mapView(), ActionFlags::eOBJECTS_ADDED);
    for (FencePlacement const& placement : _placements)
    {
      PatternSource const& source = placement.kind == PieceKind::section
        ? _section_source : _post_source;
      mapView()->getWorld()->addM2AndGetInstance(
        *source.file_key, placement.position, placement.scale, placement.rotation,
        nullptr, true, true);
    }
    NOGGIT_ACTION_MGR->endAction();

    std::size_t const sections = std::count_if(_placements.begin(), _placements.end(),
      [](FencePlacement const& placement) { return placement.kind == PieceKind::section; });
    std::size_t const posts = _placements.size() - sections;
    clearPath();
    updatePanelStatus(QString("Placed %1 section(s) and %2 post(s) as one undo action. Pattern remains active.")
                      .arg(sections).arg(posts));
    mapView()->mainWindow()->statusBar()->showMessage(
      QString("Fence Builder: placed %1 section(s) and %2 post(s). Undo removes the complete run.")
        .arg(sections).arg(posts),
      6000);
  }

  void FenceTool::updatePanelStatus(QString const& message)
  {
    std::size_t const sections = std::count_if(_placements.begin(), _placements.end(),
      [](FencePlacement const& placement) { return placement.kind == PieceKind::section; });
    std::size_t const posts = _placements.size() - sections;
    if (_path_label)
      _path_label->setText(QString("%1 control point(s); %2 section(s), %3 post(s)")
        .arg(_control_points.size()).arg(sections).arg(posts));

    if (!_status_label)
      return;
    if (!message.isEmpty())
    {
      _status_label->setText(message);
      return;
    }

    if (_capturing_pattern_area)
      _status_label->setText(QString("Reference selection: %1 painted cell(s). Ctrl+paint erases; choose OK - Use Selection when finished.")
        .arg(_reference_mask.size()));
    else if (!_section_source.file_key)
      _status_label->setText("Choose Select Reference and paint over the existing fence corridor.");
    else if (!_post_source.file_key)
      _status_label->setText("Choose the fence post M2. Posts are placed at every section boundary.");
    else if (_control_points.size() < 2)
      _status_label->setText("Click at least two points along the road centerline.");
    else if (_placements.size() >= maximum_fence_placements)
      _status_label->setText("Preview reached the 5,000-object safety limit.");
    else
      _status_label->setText(_captured_pattern.empty()
        ? QString("Ready to place %1 section(s) and %2 post(s) in continuous mode. Yellow bars are sections; orange markers are posts.")
            .arg(sections).arg(posts)
        : QString("Ready to place %1 section(s) and %2 post(s) using the exact captured %3-unit full-corridor pattern. Side and road-edge offset are already encoded by the reference.")
            .arg(sections).arg(posts).arg(_captured_pattern_length, 0, 'f', 2));
  }
}
