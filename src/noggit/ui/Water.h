// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <cstdint>
#include <noggit/BoolToggleProperty.hpp>
#include <noggit/TileIndex.hpp>

class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;
class World;
class QComboBox;
class QRadioButton;
class QButtonGroup;

namespace Noggit
{
  struct unsigned_int_property;

  namespace Ui
  {
    class water : public QWidget
    {
      Q_OBJECT

    public:
      water ( unsigned_int_property* current_layer
            , BoolToggleProperty* display_all_layers
            , QWidget* parent = nullptr
            );

      void updatePos(TileIndex const& newTile);
      void updateData();

      void changeWaterType(int waterint);

      void paintLiquid (World*, glm::vec3 const& pos, bool add, float delta_time);
      void beginStroke(glm::vec3 const& cursor_pos);
      void endStroke();

      void changeRadius(float change);
      void setRadius(float radius);
      void changeOrientation(float change);
      void changeAngle(float change);
      void change_height(float change);

      void lockPos(glm::vec3 const& cursor_pos);
      void toggle_lock();
      void toggle_angled_mode();

      float brushRadius() const;
      float innerRadius() const;
      float angle() const;
      float orientation() const;
      bool angled_mode() const;
      bool locked() const;
      bool use_ref_pos() const;
      bool showLiquidVertices() const;
      int liquidAttributeOverlay() const;
      int heightFalloff() const;
      std::uint64_t surfaceToken() const;
      glm::vec3 ref_pos() const;

      QSize sizeHint() const override;

    signals:
      void regenerate_water_opacity (float factor);
      void crop_water();
      void clear_fishable_flags();
      void clear_fatigue_flags();
      void clear_all_liquid_flags();
      void clear_fishing_flags_outside_liquid();
      void regenerate_liquid_flags();

    private:
      static constexpr float RIVER_OPACITY_VALUE = 0.0337f;
      static constexpr float OCEAN_OPACITY_VALUE = 0.007f;

      float get_opacity_factor() const;

      int _liquid_id;
      liquid_basic_types _liquid_type;
      float _radius;

      float _angle;
      float _orientation;

      BoolToggleProperty _locked;
      BoolToggleProperty _angled_mode;

      BoolToggleProperty _override_liquid_id;
      BoolToggleProperty _override_height;
      BoolToggleProperty _edit_current_layer;
      BoolToggleProperty _show_liquid_vertices;

      int _opacity_mode;
      float _custom_opacity_factor;
      int _edit_channel = 0;
      float _depth_value = 1.f;
      float _uv_scale = 4.f;
      float _uv_rotation = 0.f;
      float _height_strength = 4.f;
      float _inner_radius = 0.f;
      int _height_falloff = 1;
      bool _stroke_active = false;
      glm::vec3 _stroke_origin{};

      glm::vec3 _lock_pos;

      QDoubleSpinBox* _radius_spin;
      QDoubleSpinBox* _angle_spin;
      QDoubleSpinBox* _orientation_spin;

      QDoubleSpinBox* _x_spin;
      QDoubleSpinBox* _z_spin;
      QDoubleSpinBox* _h_spin;

      QRadioButton* river_button;
      QRadioButton* ocean_button;
      QRadioButton* custom_button;
      QButtonGroup* transparency_toggle;

      QComboBox* waterType;
      QSpinBox* waterLayer;

      unsigned_int_property* _current_layer;
      BoolToggleProperty* _display_all_layers;
      std::uint64_t _surface_token = 0;

      TileIndex tile;
    };
  }
}
