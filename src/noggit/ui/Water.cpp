// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/ChunkWater.hpp>
#include <noggit/MapHeaders.h>
#include <noggit/ui/Checkbox.hpp>
#include <noggit/ui/pushbutton.hpp>
#include <noggit/ui/Water.h>
#include <noggit/unsigned_int_property.hpp>
#include <noggit/World.h>

#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QRadioButton>
#include <QtCore/QSettings>

#include <atomic>
#include <cmath>

namespace Noggit
{
  namespace
  {
    std::atomic_uint64_t next_liquid_surface_token{1};
  }

  namespace Ui
  {
    water::water ( unsigned_int_property* current_layer
                 , BoolToggleProperty* display_all_layers
                 , QWidget* parent
                 )
      : QWidget (parent)
      , _liquid_id(5)
      , _liquid_type(liquid_basic_types_water)
      , _radius(10.0f)
      , _angle(10.0f)
      , _orientation(0.0f)
      , _locked(false)
      , _angled_mode(false)
      , _override_liquid_id(true)
      , _override_height(false)
      , _edit_current_layer(true)
      , _show_liquid_vertices(QSettings().value("waterEditor/showLiquidVertexGrid", false).toBool())
      , _opacity_mode(auto_opacity)
      , _custom_opacity_factor(RIVER_OPACITY_VALUE)
      , _lock_pos(glm::vec3(0.0f, 0.0f, 0.0f))
      , _current_layer(current_layer)
      , _display_all_layers(display_all_layers)
      , tile(0, 0)
    {
      setMinimumWidth(250);
      // setMaximumWidth(250);

      auto layout (new QFormLayout (this));

      auto brush_group(new QGroupBox("Brush", this));
      auto brush_layout (new QFormLayout (brush_group));

      _radius_spin = new QDoubleSpinBox (this);
      _radius_spin->setRange (0.f, 1000.f);
      connect ( _radius_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _radius = f; }
              );
      _radius_spin->setValue(_radius);
      brush_layout->addRow ("Radius", _radius_spin);

      waterType = new QComboBox(this);

      for (DBCFile::Iterator i = gLiquidTypeDB.begin(); i != gLiquidTypeDB.end(); ++i)
      {
        int liquid_id = i->getInt(LiquidTypeDB::ID);

        // filter WMO liquids
        if (liquid_id == LIQUID_WMO_Water || liquid_id == LIQUID_WMO_Ocean || liquid_id == LIQUID_WMO_Water_Interior
            || liquid_id == LIQUID_WMO_Magma || liquid_id == LIQUID_WMO_Slime)
            continue;

        std::stringstream ss;
        ss << liquid_id << "-" << LiquidTypeDB::getLiquidName(liquid_id);
        waterType->addItem (QString::fromUtf8(ss.str().c_str()), QVariant (liquid_id));

      }

      connect (waterType, qOverload<int> (&QComboBox::currentIndexChanged)
              , [&]
                {
                  changeWaterType(waterType->currentData().toInt());

                  // change auto opacity based on liquid type
                  if (_opacity_mode == custom_opacity || _opacity_mode == auto_opacity)
                      return;

                  // other liquid types shouldn't use opacity(depth)
                  int liquid_type = LiquidTypeDB::getLiquidType(_liquid_id);
                  if (liquid_type == liquid_basic_types_ocean) // ocean
                  {
                      ocean_button->setChecked(true);
                      _opacity_mode = ocean_opacity;
                  }
                  else // water. opacity doesn't matter for lava/slim
                  {
                      river_button->setChecked(true);
                      _opacity_mode = river_opacity;
                  }

                }
              );

      brush_layout->addRow (waterType);

      layout->addRow (brush_group);

      auto angle_group (new QGroupBox ("Angled mode", this));
      angle_group->setCheckable (true);
      angle_group->setChecked (_angled_mode.get());
      
      
      connect ( &_angled_mode, &BoolToggleProperty::changed
              , angle_group, &QGroupBox::setChecked
              );
      connect ( angle_group, &QGroupBox::toggled
              , &_angled_mode, &BoolToggleProperty::set
              );
      auto angle_layout (new QFormLayout (angle_group));

      _angle_spin = new QDoubleSpinBox (this);
      _angle_spin->setRange (0.00001f, 89.f);
      _angle_spin->setSingleStep (2.0f);
      connect ( _angle_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _angle = f; }
              );
      _angle_spin->setValue(_angle);
      angle_layout->addRow ("Angle", _angle_spin);

      _orientation_spin = new QDoubleSpinBox (this);
      _orientation_spin->setRange (0.f, 360.f);
      _orientation_spin->setWrapping (true);
      _orientation_spin->setValue(_orientation);
      _orientation_spin->setSingleStep (5.0f);
      connect ( _orientation_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _orientation = f; }
              );

      angle_layout->addRow ("Orienation", _orientation_spin);

      layout->addRow (angle_group);

      auto lock_group (new QGroupBox ("Lock", this));
      lock_group->setCheckable (true);
      lock_group->setChecked (_locked.get());
      auto lock_layout (new QFormLayout (lock_group));

      lock_layout->addRow("X:", _x_spin = new QDoubleSpinBox (this));
      lock_layout->addRow("Z:", _z_spin = new QDoubleSpinBox (this));
      lock_layout->addRow("H:", _h_spin = new QDoubleSpinBox (this));

      _x_spin->setRange (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
      _z_spin->setRange (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
      _h_spin->setRange (std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
      _x_spin->setDecimals (2);
      _z_spin->setDecimals (2);
      _h_spin->setDecimals (2);

      connect ( _x_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _lock_pos.x = f; }
              );
      connect ( _z_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _lock_pos.z = f; }
              );
      connect ( _h_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _lock_pos.y = f; }
              );

      connect ( &_locked, &BoolToggleProperty::changed
              , lock_group, &QGroupBox::setChecked
              );
      connect ( lock_group, &QGroupBox::toggled
              , &_locked, &BoolToggleProperty::set
              );

      layout->addRow(lock_group);

      auto override_group (new QGroupBox ("Paint channels", this));
      auto override_layout (new QFormLayout (override_group));

      auto liquid_id_channel = new CheckBox ("Change liquid type", &_override_liquid_id, this);
      liquid_id_channel->setToolTip("Applies the selected liquid type only to the current surface.");
      override_layout->addWidget(liquid_id_channel);
      auto height_channel = new CheckBox ("Reshape existing height", &_override_height, this);
      height_channel->setToolTip("Off paints coverage without flattening existing water. On applies the level or angled plane to vertices in the brush.");
      override_layout->addWidget(height_channel);

      layout->addRow(override_group);

      auto vertex_group = new QGroupBox("Vertex data", this);
      auto vertex_layout = new QFormLayout(vertex_group);
      auto edit_channel = new QComboBox(this);
      edit_channel->addItems({"Coverage", "Raise / lower", "Flatten", "Smooth",
                              "Depth / opacity", "UV projection", "Fishing zones",
                              "Fatigue / deep zones"});
      connect(edit_channel, qOverload<int>(&QComboBox::currentIndexChanged),
              [this](int value) { _edit_channel = value; });
      vertex_layout->addRow("Edit channel", edit_channel);

      auto clear_fishable = new pushbutton("Clear fishing flags on ADT",
                                           [this] { emit clear_fishable_flags(); });
      clear_fishable->setToolTip("Clears every fishing-zone bit on the current ADT. Undo is supported.");
      vertex_layout->addRow(clear_fishable);
      auto clear_fatigue = new pushbutton("Clear fatigue flags on ADT",
                                          [this] { emit clear_fatigue_flags(); });
      clear_fatigue->setToolTip("Clears every fatigue/deep-zone bit on the current ADT. Undo is supported.");
      vertex_layout->addRow(clear_fatigue);
      auto clear_all_flags = new pushbutton("Clear all liquid flags on ADT",
                                            [this] { emit clear_all_liquid_flags(); });
      clear_all_flags->setToolTip("Clears both liquid flag masks on the current ADT. Undo is supported.");
      vertex_layout->addRow(clear_all_flags);
      auto clear_outside_liquid = new pushbutton("Clear fishing flags outside liquid on ADT",
                                                 [this] { emit clear_fishing_flags_outside_liquid(); });
      clear_outside_liquid->setToolTip("Clears fishing bits only where no liquid surface exists. Water, ocean, lava, and slime flags are preserved. Undo is supported.");
      vertex_layout->addRow(clear_outside_liquid);
      auto regenerate_flags = new pushbutton("Regenerate liquid flags on ADT",
                                             [this] { emit regenerate_liquid_flags(); });
      regenerate_flags->setToolTip("Rebuilds flags from liquid data. Every occupied liquid cell becomes fishable, including lava and slime; fatigue remains limited to deep Ocean cells.");
      vertex_layout->addRow(regenerate_flags);

      auto show_liquid_vertices = new CheckBox("Show liquid vertex grid",
                                                &_show_liquid_vertices, this);
      show_liquid_vertices->setToolTip("Displays the active MH2O vertices and grid while shaping water.");
      vertex_layout->addRow(show_liquid_vertices);
      connect(&_show_liquid_vertices, &BoolToggleProperty::changed,
              [](bool enabled)
              {
                QSettings().setValue("waterEditor/showLiquidVertexGrid", enabled);
              });

      auto height_strength = new QDoubleSpinBox(this);
      height_strength->setRange(0.01, 100.0);
      height_strength->setDecimals(2);
      height_strength->setSingleStep(0.25);
      height_strength->setValue(_height_strength);
      height_strength->setToolTip("Raise/lower units per second, or convergence speed for flatten and smooth.");
      connect(height_strength, qOverload<double>(&QDoubleSpinBox::valueChanged),
              [this](double value) { _height_strength = static_cast<float>(value); });
      vertex_layout->addRow("Height strength", height_strength);

      auto inner_radius = new QDoubleSpinBox(this);
      inner_radius->setRange(0.0, 1.0);
      inner_radius->setDecimals(2);
      inner_radius->setSingleStep(0.05);
      inner_radius->setValue(_inner_radius);
      inner_radius->setToolTip("Fraction of the brush radius that receives full strength.");
      connect(inner_radius, qOverload<double>(&QDoubleSpinBox::valueChanged),
              [this](double value) { _inner_radius = static_cast<float>(value); });
      vertex_layout->addRow("Inner radius", inner_radius);

      auto height_falloff = new QComboBox(this);
      height_falloff->addItems({"Flat", "Linear", "Smooth"});
      height_falloff->setCurrentIndex(_height_falloff);
      connect(height_falloff, qOverload<int>(&QComboBox::currentIndexChanged),
              [this](int value) { _height_falloff = value; });
      vertex_layout->addRow("Height falloff", height_falloff);

      auto depth_value = new QDoubleSpinBox(this);
      depth_value->setRange(0.0, 1.0);
      depth_value->setDecimals(3);
      depth_value->setSingleStep(0.05);
      depth_value->setValue(_depth_value);
      connect(depth_value, qOverload<double>(&QDoubleSpinBox::valueChanged),
              [this](double value) { _depth_value = static_cast<float>(value); });
      vertex_layout->addRow("Depth", depth_value);

      auto uv_scale = new QDoubleSpinBox(this);
      uv_scale->setRange(0.0001, 10000.0);
      uv_scale->setDecimals(4);
      uv_scale->setValue(_uv_scale);
      connect(uv_scale, qOverload<double>(&QDoubleSpinBox::valueChanged),
              [this](double value) { _uv_scale = static_cast<float>(value); });
      vertex_layout->addRow("UV world scale", uv_scale);

      auto uv_rotation = new QDoubleSpinBox(this);
      uv_rotation->setRange(0.0, 360.0);
      uv_rotation->setWrapping(true);
      uv_rotation->setValue(_uv_rotation);
      connect(uv_rotation, qOverload<double>(&QDoubleSpinBox::valueChanged),
              [this](double value) { _uv_rotation = static_cast<float>(value); });
      vertex_layout->addRow("UV rotation", uv_rotation);
      layout->addRow(vertex_group);

      auto opacity_group (new QGroupBox ("Auto opacity", this));
      auto opacity_layout (new QFormLayout (opacity_group));

      auto auto_button(new QRadioButton("Auto", this));
      auto_button->setToolTip("Automatically uses river or ocean opacity based on liquid type.");
      river_button = new QRadioButton ("River", this);
      river_button->setToolTip(std::to_string(RIVER_OPACITY_VALUE).c_str());
      ocean_button = new QRadioButton ("Ocean", this);
      ocean_button->setToolTip(std::to_string(OCEAN_OPACITY_VALUE).c_str());
      custom_button = new QRadioButton ("Custom factor:", this);

      transparency_toggle = new QButtonGroup (this);
      transparency_toggle->addButton(auto_button, auto_opacity);
      transparency_toggle->addButton (river_button, river_opacity);
      transparency_toggle->addButton (ocean_button, ocean_opacity);
      transparency_toggle->addButton (custom_button, custom_opacity);

      connect ( transparency_toggle, qOverload<int> (&QButtonGroup::idClicked)
              , [&] (int id) { _opacity_mode = id; }
              );

      opacity_layout->addRow(auto_button);
      opacity_layout->addRow (river_button);
      opacity_layout->addRow (ocean_button);
      opacity_layout->addRow (custom_button);

      transparency_toggle->button (_opacity_mode)->setChecked (true);

      QDoubleSpinBox *opacity_spin = new QDoubleSpinBox (this);
      opacity_spin->setRange (0.f, 1.f);
      opacity_spin->setDecimals (4);
      opacity_spin->setSingleStep (0.02f);
      opacity_spin->setValue(_custom_opacity_factor);
      connect ( opacity_spin, qOverload<double> (&QDoubleSpinBox::valueChanged)
              , [&] (float f) { _custom_opacity_factor = f; }
              );
      opacity_layout->addRow (opacity_spin);

      layout->addRow (opacity_group);

      layout->addRow ( new pushbutton
                            ( "Regen ADT opacity"
                            , [this]
                              {
                                emit regenerate_water_opacity
                                  (get_opacity_factor());
                              }
                            )
                        );
      layout->addRow ( new pushbutton
                            ( "Crop water"
                            , [this]
                              {
                                emit crop_water();
                              }
                            )
                        );

      auto layer_group (new QGroupBox ("Layers", this));
      auto layer_layout (new QFormLayout (layer_group));

      layer_layout->addRow (new CheckBox("Show all layers", display_all_layers));
      auto edit_current = new CheckBox("Edit current layer only", &_edit_current_layer, this);
      edit_current->setToolTip("Keeps stacked and nearly-equal liquid surfaces independent. Disable for the legacy liquid-ID brush.");
      layer_layout->addRow(edit_current);
      layer_layout->addRow (new QLabel("Current layer:", this));

      waterLayer = new QSpinBox (this);
      waterLayer->setValue (current_layer->get());
      waterLayer->setRange (0, 100);
      layer_layout->addRow (waterLayer);

      auto new_surface = new pushbutton("New surface layer", [this]
      {
        _edit_current_layer.set(true);
        waterLayer->setValue(waterLayer->value() + 1);
        _surface_token = next_liquid_surface_token.fetch_add(1);
      });
      new_surface->setToolTip("Selects the next independent layer. Painting creates it where needed without replacing lower water.");
      layer_layout->addRow(new_surface);

      layout->addRow (layer_group);

      connect ( waterLayer, qOverload<int> (&QSpinBox::valueChanged)
              , current_layer, &unsigned_int_property::set
              );
      connect ( waterLayer, qOverload<int> (&QSpinBox::valueChanged)
              , this, [this] { _surface_token = 0; }
              );
      connect ( current_layer, &unsigned_int_property::changed
              , waterLayer, &QSpinBox::setValue
              );

      updateData();

    }

    void water::updatePos(TileIndex const& newTile)
    {
      if (newTile == tile) return;

      tile = newTile;

      updateData();
    }

    void water::updateData()
    {
      std::stringstream mt;
      mt << _liquid_id << " - " << LiquidTypeDB::getLiquidName(_liquid_id);
      waterType->setCurrentText (QString::fromStdString (mt.str()));
      _liquid_type = static_cast<liquid_basic_types>(LiquidTypeDB::getLiquidType(_liquid_id));
    }

    void water::changeWaterType(int waterint)
    {
      _liquid_id = waterint;

      updateData();
    }

    void water::changeRadius(float change)
    {
      _radius_spin->setValue(_radius + change);
    }

    void water::setRadius(float radius)
    {
      _radius_spin->setValue(radius);
    }

    void water::changeOrientation(float change)
    {
      _orientation += change;

      while (_orientation >= 360.0f)
      {
        _orientation -= 360.0f;
      }
      while (_orientation < 0.0f)
      {
        _orientation += 360.0f;
      }

      _orientation_spin->setValue(_orientation);
    }

    void water::changeAngle(float change)
    {
      _angle_spin->setValue(_angle + change);
    }

    void water::change_height(float change)
    {
      _h_spin->setValue(_lock_pos.y + change);
    }

    void water::paintLiquid (World* world, glm::vec3 const& pos, bool add, float delta_time)
    {
      int const target_layer = _edit_current_layer.get() ? static_cast<int>(_current_layer->get()) : -1;
      if (_edit_channel == 1)
      {
        if (target_layer >= 0)
          world->raiseLowerLiquid(pos, _radius,
                                  (add ? 1.f : -1.f) * _height_strength * delta_time,
                                  _inner_radius, _height_falloff, target_layer, _surface_token);
        return;
      }
      if (_edit_channel == 2)
      {
        if (target_layer >= 0)
        {
          glm::vec3 const& origin = _locked.get() ? _lock_pos : _stroke_origin;
          float const strength = 1.f - std::pow(0.5f, delta_time * _height_strength);
          world->flattenLiquid(pos, _radius, strength, _inner_radius, _height_falloff,
                               origin, math::degrees(_angled_mode.get() ? _angle : 0.f),
                               math::degrees(_angled_mode.get() ? _orientation : 0.f),
                               target_layer, _surface_token);
        }
        return;
      }
      if (_edit_channel == 3)
      {
        if (target_layer >= 0)
        {
          float const strength = 1.f - std::pow(0.5f, delta_time * _height_strength);
          world->smoothLiquid(pos, _radius, strength, _inner_radius, _height_falloff,
                              target_layer, _surface_token);
        }
        return;
      }
      if (_edit_channel == 4)
      {
        if (target_layer >= 0)
          world->paintLiquidDepth(pos, _radius, add ? _depth_value : 0.f, target_layer,
                                  _surface_token);
        return;
      }
      if (_edit_channel == 5)
      {
        if (target_layer >= 0)
          world->projectLiquidUV(pos, _radius, _uv_scale,
                                 math::degrees(add ? _uv_rotation : 0.f), target_layer,
                                 _surface_token);
        return;
      }
      if (_edit_channel == 6 || _edit_channel == 7)
      {
        world->paintLiquidAttribute(pos, _radius,
          _edit_channel == 6 ? LiquidAttribute::Fishable : LiquidAttribute::Fatigue,
          add, target_layer, _surface_token);
        return;
      }

      bool const fixed_angled_plane = _angled_mode.get() && _stroke_active;
      world->paintLiquid ( pos
                         , _radius
                         , _liquid_id
                         , add
                         , math::degrees (_angled_mode.get() ? _angle : 0.0f)
                         , math::degrees (_angled_mode.get() ? _orientation : 0.0f)
                         , _locked.get() || fixed_angled_plane
                         , _locked.get() ? _lock_pos : _stroke_origin
                         , _override_height.get()
                         , _override_liquid_id.get()
                         , get_opacity_factor()
                         , target_layer
                         , _surface_token
                         );
    }

    void water::beginStroke(glm::vec3 const& cursor_pos)
    {
      if (_stroke_active)
        return;

      _stroke_active = true;
      _stroke_origin = _locked.get()
        ? _lock_pos
        : glm::vec3(cursor_pos.x, cursor_pos.y + 1.f, cursor_pos.z);
    }

    void water::endStroke()
    {
      _stroke_active = false;
    }

    void water::lockPos(glm::vec3 const& cursor_pos)
    {
      QSignalBlocker const blocker_x(_x_spin);
      QSignalBlocker const blocker_z(_z_spin);
      QSignalBlocker const blocker_h(_h_spin);
      _lock_pos = cursor_pos;

      _x_spin->setValue(_lock_pos.x);
      _z_spin->setValue(_lock_pos.z);
      _h_spin->setValue(_lock_pos.y);

      if (!_locked.get())
      {
        toggle_lock();
      }
    }

    void water::toggle_lock()
    {
      _locked.toggle();
    }

    void water::toggle_angled_mode()
    {
      _angled_mode.toggle();
    }

    float water::brushRadius() const
    {
      return _radius;
    }

    float water::innerRadius() const
    {
      return _inner_radius;
    }

    float water::angle() const
    {
      return _angle;
    }

    float water::orientation() const
    {
      return _orientation;
    }

    bool water::angled_mode() const
    {
      return _angled_mode.get();
    }

    bool water::locked() const
    {
      return _locked.get();
    }

    bool water::use_ref_pos() const
    {
      return _locked.get() || (_angled_mode.get() && _stroke_active);
    }

    bool water::showLiquidVertices() const
    {
      bool const editing_flags = _edit_channel == 6 || _edit_channel == 7;
      bool const editing_vertices = _edit_channel >= 1 && _edit_channel <= 3;
      return _show_liquid_vertices.get() && (editing_vertices || editing_flags);
    }

    int water::liquidAttributeOverlay() const
    {
      if (_edit_channel == 6)
        return 1;
      if (_edit_channel == 7)
        return 2;
      return 0;
    }

    int water::heightFalloff() const
    {
      return _height_falloff;
    }

    std::uint64_t water::surfaceToken() const
    {
      return _surface_token;
    }

    glm::vec3 water::ref_pos() const
    {
      return _locked.get() ? _lock_pos : _stroke_origin;
    }

    float water::get_opacity_factor() const
    {
      switch (_opacity_mode)
      {
      default:          // values found by experimenting
      case river_opacity:  return RIVER_OPACITY_VALUE;
      case ocean_opacity:  return OCEAN_OPACITY_VALUE;
      case custom_opacity: return _custom_opacity_factor;
      case auto_opacity:
      {
        switch (_liquid_type)
        {
        case 0: return RIVER_OPACITY_VALUE;
        case 1: return OCEAN_OPACITY_VALUE;
        default:  return RIVER_OPACITY_VALUE; // lava and slime, opacity isn't used
        }
      }
      break;
      }
    }

    QSize water::sizeHint() const
    {
      return QSize(250, height());
    }
  }
}
